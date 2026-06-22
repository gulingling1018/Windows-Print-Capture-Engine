#include "ui/main_window.h"

#include "common/app_config.h"
#include "ui/grip_splitter.h"
#include "ui/completion_dialog.h"
#include "ui/job_detail_panel.h"
#include "ui/grip_splitter.h"
#include "ui/pricing_config.h"
#include "ui/receipt_preview_panel.h"
#include "ui/settings_dialog.h"
#include "ui/user_color_palette.h"
#include "ui/user_group_delegate.h"
#include "ui/user_group_model.h"
#include "ui/user_task_model.h"
#include "ui/watcher_bridge.h"

#include <QAbstractItemView>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QResizeEvent>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QSizePolicy>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <winspool.h>
#include <shlobj.h>
#endif

namespace wpce::ui
{

namespace
{

void repolish(QWidget* widget)
{
  if (widget == nullptr) return;
  widget->style()->unpolish(widget);
  widget->style()->polish(widget);
  widget->update();
}

QFrame* makeMetricCard(const QString& caption, QLabel* value)
{
  auto* card = new QFrame();
  card->setObjectName(QStringLiteral("MetricCard"));
  card->setMinimumWidth(116);
  card->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);

  auto* layout = new QVBoxLayout(card);
  layout->setContentsMargins(14, 12, 14, 12);
  layout->setSpacing(6);

  auto* captionLabel = new QLabel(caption);
  captionLabel->setObjectName(QStringLiteral("MetricLabel"));

  if (value->objectName().isEmpty())
    value->setObjectName(QStringLiteral("MetricValue"));
  value->setTextInteractionFlags(Qt::TextSelectableByMouse);

  layout->addWidget(captionLabel);
  layout->addWidget(value);
  return card;
}

void setButtonRole(QPushButton* button, const char* role)
{
  button->setProperty("buttonRole", role);
  button->setCursor(Qt::PointingHandCursor);
}

void makeIconButton(QPushButton* button, const QString& tooltip)
{
  button->setText(QString());
  button->setToolTip(tooltip);
  button->setFixedSize(42, 42);
  button->setIconSize(QSize(18, 18));
  button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

QString compactPathText(const QString& path)
{
  constexpr qsizetype kMaxChars = 82;
  if (path.size() <= kMaxChars) return path;

  constexpr qsizetype kMarkerSize = 3;
  const qsizetype left = (kMaxChars - kMarkerSize) / 2;
  const qsizetype right = kMaxChars - kMarkerSize - left;
  return path.left(left) + QStringLiteral("...") + path.right(right);
}

}  // namespace

// ── XPS Filter Auto-Install ───────────────────────────────────────────
//
// The wpce_xps_filter.dll COM filter must be registered under
// HKLM\CLSID\{GUID} and injected into each v4 printer's XPS pipeline
// config in the DriverStore.  Both operations require Administrator
// privilege.
//
// Once installed, the spooler loads the filter automatically for every
// XPS print job — no further admin rights needed.  Normal-user runs
// benefit from the one-time setup.
//
// Strategy:
//   Admin run     → auto-install silently, then continue as normal.
//   Non-admin run → if already installed, proceed normally.
//                    if not installed, alert the user once.
//
// The sentinel file %ProgramFiles%\WPCE\wpce_filter_installed avoids
// re-running the spooler restart on every admin launch.

namespace
{

const wchar_t* filterClsid() noexcept
{
  return L"{B1C8E3F0-2D4A-5B6C-7D8E-9F0A1B2C3D4E}";
}

std::wstring filterInstallPath()
{
  wchar_t progFiles[MAX_PATH];
  if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES, nullptr,
                                 0, progFiles)))
    return std::wstring(progFiles) + L"\\WPCE\\wpce_xps_filter.dll";
  return L"C:\\Program Files\\WPCE\\wpce_xps_filter.dll";
}

bool isFilterRegistered()
{
  std::wstring keyPath = L"CLSID\\";
  keyPath += filterClsid();
  HKEY hKey = nullptr;
  LONG rc = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(),
                             0, KEY_READ, &hKey);
  if (rc == ERROR_SUCCESS) { ::RegCloseKey(hKey); return true; }
  return false;
}

bool isFilterSentinelPresent()
{
  std::wstring sentinel = filterInstallPath();
  auto pos = sentinel.rfind(L'\\');
  if (pos != std::wstring::npos)
    sentinel = sentinel.substr(0, pos) + L"\\wpce_filter_installed";
  return ::GetFileAttributesW(sentinel.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool isRunningAsAdmin()
{
  BOOL isAdmin = FALSE;
  PSID adminGroup = nullptr;
  SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
  if (::AllocateAndInitializeSid(&ntAuth, 2,
        SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
        0, 0, 0, 0, 0, 0, &adminGroup))
  {
    ::CheckTokenMembership(nullptr, adminGroup, &isAdmin);
    ::FreeSid(adminGroup);
  }
  return isAdmin != FALSE;
}

bool tryInstallFilter(const std::wstring& exeDir)
{
  if (isFilterSentinelPresent())
    return true;

  std::wstring installDir = filterInstallPath();
  auto pos = installDir.rfind(L'\\');
  if (pos != std::wstring::npos)
  {
    std::wstring dir = installDir.substr(0, pos);
    ::CreateDirectoryW(dir.c_str(), nullptr);
  }

  // Copy filter DLL to permanent location.
  std::wstring srcDll = exeDir + L"\\wpce_xps_filter.dll";
  if (::GetFileAttributesW(srcDll.c_str()) == INVALID_FILE_ATTRIBUTES)
    return false;

  ::CopyFileW(srcDll.c_str(), installDir.c_str(), FALSE);

  // Register COM server.
  HMODULE hMod = ::LoadLibraryW(installDir.c_str());
  if (hMod)
  {
    using RegFn = HRESULT (WINAPI*)();
    auto regServer = reinterpret_cast<RegFn>(
      ::GetProcAddress(hMod, "DllRegisterServer"));
    if (regServer) regServer();
    ::FreeLibrary(hMod);
  }

  // Inject filter into v4 printer pipeline configs.
  const wchar_t* driverStore =
    L"C:\\Windows\\System32\\DriverStore\\FileRepository";

  WIN32_FIND_DATAW fd{};
  std::wstring searchPath = std::wstring(driverStore) + L"\\*.inf";
  HANDLE hFind = ::FindFirstFileW(searchPath.c_str(), &fd);
  if (hFind != INVALID_HANDLE_VALUE)
  {
    do
    {
      std::wstring infPath =
        std::wstring(driverStore) + L"\\" + fd.cFileName;
      std::wstring dirPath = infPath;
      auto dot = dirPath.rfind(L'.');
      if (dot != std::wstring::npos)
        dirPath = dirPath.substr(0, dot);

      // Read INF — check for v4 XPSDrv markers.
      HANDLE hInf = ::CreateFileW(infPath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (hInf == INVALID_HANDLE_VALUE) continue;

      std::string infContent;
      DWORD sz = ::GetFileSize(hInf, nullptr);
      if (sz > 0 && sz < 1048576)
      {
        infContent.resize(sz);
        DWORD read = 0;
        ::ReadFile(hInf, infContent.data(), sz, &read, nullptr);
      }
      ::CloseHandle(hInf);

      bool isV4 = (infContent.find("XPSDrvFilter") != std::string::npos ||
                   infContent.find("FilterPipelineConfig") != std::string::npos ||
                   infContent.find("[DriverConfig]") != std::string::npos);
      if (!isV4) continue;

      std::wstring configPath =
        dirPath + L"\\FilterPipelineConfig.xml";
      if (::GetFileAttributesW(configPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        continue;

      // Read config XML.
      HANDLE hCfg = ::CreateFileW(configPath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (hCfg == INVALID_HANDLE_VALUE) continue;
      std::string cfgXml;
      DWORD cSz = ::GetFileSize(hCfg, nullptr);
      if (cSz > 0 && cSz < 1048576)
      {
        cfgXml.resize(cSz);
        DWORD read = 0;
        ::ReadFile(hCfg, cfgXml.data(), cSz, &read, nullptr);
      }
      ::CloseHandle(hCfg);
      if (cfgXml.empty()) continue;

      // Already injected?
      std::string clsidA(filterClsid(), filterClsid() + wcslen(filterClsid()));
      if (cfgXml.find(clsidA) != std::string::npos) continue;

      // Find <Filters> tag.
      auto fpos = cfgXml.find("<Filters>");
      if (fpos == std::string::npos)
      { fpos = cfgXml.find("<Filters "); if (fpos == std::string::npos) continue; }
      auto fclose = cfgXml.find('>', fpos);
      if (fclose == std::string::npos) continue;

      // Build filter fragment.
      char dllPathA[MAX_PATH];
      ::WideCharToMultiByte(CP_UTF8, 0, installDir.c_str(), -1,
                            dllPathA, MAX_PATH, nullptr, nullptr);
      std::string filterXml = "\r\n  <Filter xmlns=\"http://schemas.microsoft.com/windows/2005/06/printing/FilterPipeline\" dll=\"";
      filterXml += dllPathA;
      filterXml += "\" clsid=\"";
      filterXml += clsidA;
      filterXml += "\" name=\"WPCE Capture\">\r\n    <Input><InputPart type=\"IFixedDocumentSequence\"/></Input>\r\n    <Output><OutputPart type=\"IFixedDocumentSequence\"/></Output>\r\n  </Filter>";

      cfgXml.insert(fclose + 1, filterXml);

      // Backup + write.
      std::wstring backupPath = configPath + L".wpce-backup";
      if (::GetFileAttributesW(backupPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        ::CopyFileW(configPath.c_str(), backupPath.c_str(), TRUE);

      HANDLE hW = ::CreateFileW(configPath.c_str(), GENERIC_WRITE,
        0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (hW != INVALID_HANDLE_VALUE)
      {
        DWORD written = 0;
        ::WriteFile(hW, cfgXml.data(),
                    static_cast<DWORD>(cfgXml.size()), &written, nullptr);
        ::CloseHandle(hW);
      }

    } while (::FindNextFileW(hFind, &fd));
    ::FindClose(hFind);
  }

  // Restart spooler.
  SC_HANDLE scm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (scm)
  {
    SC_HANDLE svc = ::OpenServiceW(scm, L"Spooler",
      SERVICE_STOP | SERVICE_START);
    if (svc)
    {
      SERVICE_STATUS st{};
      ::ControlService(svc, SERVICE_CONTROL_STOP, &st);
      for (int i = 0; i < 30; ++i)
      {
        ::QueryServiceStatus(svc, &st);
        if (st.dwCurrentState == SERVICE_STOPPED) break;
        ::Sleep(500);
      }
      ::StartServiceW(svc, 0, nullptr);
      ::CloseServiceHandle(svc);
    }
    ::CloseServiceHandle(scm);
  }

  // Write sentinel.
  std::wstring sentinel = installDir;
  auto sp = sentinel.rfind(L'\\');
  if (sp != std::wstring::npos)
    sentinel = sentinel.substr(0, sp) + L"\\wpce_filter_installed";
  HANDLE hS = ::CreateFileW(sentinel.c_str(), GENERIC_WRITE,
    0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hS != INVALID_HANDLE_VALUE)
  {
    const char* msg = "installed";
    DWORD w = 0;
    ::WriteFile(hS, msg, 9, &w, nullptr);
    ::CloseHandle(hS);
  }

  return true;
}

/// Check filter status and install if running as admin.
/// Returns a user-visible message if action is needed.
QString ensureFilterInstalled()
{
  if (isFilterSentinelPresent())
    return {};  // everything set up

  QString exeDir = QCoreApplication::applicationDirPath();

  if (isRunningAsAdmin())
  {
    // Auto-install — we have the required privileges.
    bool ok = tryInstallFilter(exeDir.toStdWString());
    if (ok)
      return QStringLiteral("XPS 过滤器已自动安装并注册，打印参数捕获已就绪");
    else
      return QStringLiteral("XPS 过滤器安装失败 — wpce_xps_filter.dll 未找到");
  }
  else
  {
    // Not admin — user needs to run once as admin.
    // Check if DLL exists next to the exe.
    QString dllPath = exeDir + QStringLiteral("/wpce_xps_filter.dll");
    if (!QFile::exists(dllPath))
      return QStringLiteral("wpce_xps_filter.dll 未找到 — 打印参数捕获不可用");

    // Filter not installed yet, but DLL exists.  Prompt once.
    static bool warned = false;
    if (!warned)
    {
      warned = true;
      return QStringLiteral("⚡ 首次使用请以管理员身份运行一次以安装 XPS 过滤器（之后无需管理员权限）");
    }
    return {};
  }
}

}  // namespace

// ── MainWindow ────────────────────────────────────────────────────────

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
  loadOrCreateConfig();

  setWindowTitle(QStringLiteral("WPCE 打印捕获控制台"));
  resize(config_.window.width, config_.window.height);
  setMinimumSize(1040, 640);

  bridge_ = new WatcherBridge(this);
  groupModel_ = new UserGroupModel(this);
  userTaskModel_ = new UserTaskModel(this);
  userTaskModel_->setPricingConfig(&config_.pricing);

  auto* central = new QWidget(this);
  central->setObjectName(QStringLiteral("AppRoot"));
  central->setMinimumWidth(0);
  central->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
  auto* root = new QVBoxLayout(central);
  root->setContentsMargins(20, 18, 20, 16);
  root->setSpacing(14);
  setCentralWidget(central);

  // ── Hero bar ─────────────────────────────────────────────────────
  auto* hero = new QFrame();
  hero->setObjectName(QStringLiteral("HeroBar"));
  hero->setMinimumWidth(0);
  hero->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  auto* heroLayout = new QHBoxLayout(hero);
  heroLayout->setContentsMargins(18, 16, 18, 16);
  heroLayout->setSpacing(18);

  auto* titleBlock = new QVBoxLayout();
  titleBlock->setContentsMargins(0, 0, 0, 0);
  titleBlock->setSpacing(4);

  auto* title = new QLabel(QStringLiteral("WPCE 打印捕获控制台"));
  title->setObjectName(QStringLiteral("WindowTitle"));
  title->setMinimumWidth(0);
  title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  auto* subtitle = new QLabel(QStringLiteral("按用户分组 · 打印作业参数审计"));
  subtitle->setObjectName(QStringLiteral("WindowSubtitle"));
  subtitle->setMinimumWidth(0);
  subtitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  titleBlock->addWidget(title);
  titleBlock->addWidget(subtitle);
  heroLayout->addLayout(titleBlock, 1);

  startBtn_ = new QPushButton(QStringLiteral("开始监听"));
  startBtn_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
  startBtn_->setText(QStringLiteral("开始"));
  startBtn_->setToolTip(QStringLiteral("开始监听"));
  startBtn_->setFixedHeight(42);
  startBtn_->setMinimumWidth(80);
  startBtn_->setMaximumWidth(96);
  startBtn_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  setButtonRole(startBtn_, "primary");

  stopBtn_ = new QPushButton(QStringLiteral("停止"));
  stopBtn_->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
  setButtonRole(stopBtn_, "danger");
  makeIconButton(stopBtn_, QStringLiteral("停止监听"));

  openOutputBtn_ = new QPushButton(QStringLiteral("打开目录"));
  openOutputBtn_->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
  setButtonRole(openOutputBtn_, "quiet");
  makeIconButton(openOutputBtn_, QStringLiteral("打开输出目录"));

  settingsBtn_ = new QPushButton(QStringLiteral("设置"));
  settingsBtn_->setIcon(style()->standardIcon(QStyle::SP_FileDialogDetailedView));
  setButtonRole(settingsBtn_, "quiet");
  makeIconButton(settingsBtn_, QStringLiteral("设置"));

  heroLayout->addWidget(startBtn_);
  heroLayout->addWidget(stopBtn_);
  heroLayout->addWidget(openOutputBtn_);
  heroLayout->addWidget(settingsBtn_);
  root->addWidget(hero);

  // ── Metrics ───────────────────────────────────────────────────────
  auto* metrics = new QHBoxLayout();
  metrics->setContentsMargins(0, 0, 0, 0);
  metrics->setSpacing(12);

  sessionStateLabel_ = new QLabel(QStringLiteral("待启动"));
  sessionStateLabel_->setObjectName(QStringLiteral("StatusPill"));
  sessionStateLabel_->setProperty("state", "idle");

  printerCountLabel_ = new QLabel(QStringLiteral("0"));
  userCountLabel_ = new QLabel(QStringLiteral("0"));

  outputDirLabel_ = new QLabel(QStringLiteral("尚未创建"));
  outputDirLabel_->setObjectName(QStringLiteral("PathValue"));
  outputDirLabel_->setWordWrap(false);
  outputDirLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

  metrics->addWidget(makeMetricCard(QStringLiteral("监听状态"), sessionStateLabel_), 0);
  metrics->addWidget(makeMetricCard(QStringLiteral("打印机"), printerCountLabel_), 0);
  metrics->addWidget(makeMetricCard(QStringLiteral("活跃用户"), userCountLabel_), 0);
  metrics->addWidget(makeMetricCard(QStringLiteral("输出目录"), outputDirLabel_), 1);
  root->addLayout(metrics);

  // ── Tree panel ────────────────────────────────────────────────────
  auto* treePanel = new QFrame();
  treePanel->setObjectName(QStringLiteral("TablePanel"));
  treePanel->setMinimumWidth(0);
  treePanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  auto* treePanelLayout = new QVBoxLayout(treePanel);
  treePanelLayout->setContentsMargins(16, 14, 16, 16);
  treePanelLayout->setSpacing(12);

  auto* treeHeader = new QHBoxLayout();
  treeHeader->setContentsMargins(0, 0, 0, 0);
  treeHeader->setSpacing(10);

  auto* treeTitle = new QLabel(QStringLiteral("用户作业"));
  treeTitle->setObjectName(QStringLiteral("PanelHeader"));
  treeTitle->setMinimumWidth(0);
  treeTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

  resultCountLabel_ = new QLabel(QStringLiteral("0 用户 / 0 作业"));
  resultCountLabel_->setObjectName(QStringLiteral("MutedText"));
  resultCountLabel_->setMinimumWidth(0);
  resultCountLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

  searchEdit_ = new QLineEdit();
  searchEdit_->setObjectName(QStringLiteral("SearchBox"));
  searchEdit_->setPlaceholderText(QStringLiteral("搜索用户"));
  searchEdit_->setClearButtonEnabled(true);
  searchEdit_->setMinimumWidth(96);
  searchEdit_->setMaximumWidth(140);

  clearFilterBtn_ = new QPushButton(QStringLiteral("重置"));
  clearFilterBtn_->setIcon(style()->standardIcon(QStyle::SP_DialogResetButton));
  setButtonRole(clearFilterBtn_, "quiet");
  clearFilterBtn_->setEnabled(false);
  makeIconButton(clearFilterBtn_, QStringLiteral("重置筛选"));

  expandAllBtn_ = new QPushButton(QStringLiteral("全部展开"));
  expandAllBtn_->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
  expandAllBtn_->setToolTip(QStringLiteral("展开所有用户下的作业"));
  setButtonRole(expandAllBtn_, "quiet");
  makeIconButton(expandAllBtn_, QStringLiteral("展开所有用户下的作业"));

  collapseAllBtn_ = new QPushButton(QStringLiteral("全部收起"));
  collapseAllBtn_->setIcon(style()->standardIcon(QStyle::SP_ArrowRight));
  collapseAllBtn_->setToolTip(QStringLiteral("收起所有用户，只保留父级用户行"));
  setButtonRole(collapseAllBtn_, "quiet");
  makeIconButton(collapseAllBtn_, QStringLiteral("收起所有用户，只保留父级用户行"));

  completeUserBtn_ = new QPushButton(QStringLiteral("结算选中用户"));
  completeUserBtn_->setIcon(style()->standardIcon(QStyle::SP_DialogApplyButton));
  completeUserBtn_->setText(QStringLiteral("结算"));
  completeUserBtn_->setToolTip(QStringLiteral("结算选中用户"));
  completeUserBtn_->setMinimumWidth(78);
  completeUserBtn_->setMaximumWidth(88);
  setButtonRole(completeUserBtn_, "primary");
  completeUserBtn_->setEnabled(false);

  treeHeader->addWidget(treeTitle);
  treeHeader->addWidget(resultCountLabel_);
  treeHeader->addStretch(1);
  treeHeader->addWidget(expandAllBtn_);
  treeHeader->addWidget(collapseAllBtn_);
  treeHeader->addWidget(searchEdit_);
  treeHeader->addWidget(clearFilterBtn_);
  treeHeader->addWidget(completeUserBtn_);
  treePanelLayout->addLayout(treeHeader);

  // ── QTreeView ─────────────────────────────────────────────────────
  treeView_ = new QTreeView();
  treeView_->setObjectName(QStringLiteral("JobTree"));
  treeView_->setMinimumWidth(0);
  treeView_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
  treeView_->setModel(groupModel_);
  treeView_->setSelectionBehavior(QAbstractItemView::SelectRows);
  treeView_->setSelectionMode(QAbstractItemView::SingleSelection);
  treeView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  treeView_->setAlternatingRowColors(true);
  treeView_->setSortingEnabled(false);
  treeView_->setWordWrap(false);
  treeView_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  treeView_->setAnimated(true);
  treeView_->setIndentation(24);
  treeView_->setExpandsOnDoubleClick(true);
  treeView_->setRootIsDecorated(true);  // branch area for top-level items
  treeView_->header()->setHighlightSections(false);
  treeView_->header()->setStretchLastSection(false);
  treeView_->header()->setSectionsClickable(true);   // allow drag-to-resize
  treeView_->header()->setMinimumSectionSize(28);
  treeView_->header()->setCascadingSectionResizes(false);

  // ── Column resize modes ──────────────────────────────────────────
  //  Stretch    — name column absorbs residual width
  //  Interactive — all others user-resizable like Excel
  treeView_->header()->setSectionResizeMode(UserGroupModel::ColName,       QHeaderView::Stretch);
  treeView_->header()->setSectionResizeMode(UserGroupModel::ColPhase,      QHeaderView::Interactive);
  treeView_->header()->setSectionResizeMode(UserGroupModel::ColPages,      QHeaderView::Interactive);
  treeView_->header()->setSectionResizeMode(UserGroupModel::ColColor,      QHeaderView::Interactive);
  treeView_->header()->setSectionResizeMode(UserGroupModel::ColDuplex,     QHeaderView::Interactive);
  treeView_->header()->setSectionResizeMode(UserGroupModel::ColProgress,   QHeaderView::Interactive);
  treeView_->header()->setSectionResizeMode(UserGroupModel::ColPaper,      QHeaderView::Interactive);
  treeView_->header()->setSectionResizeMode(UserGroupModel::ColSubmitTime, QHeaderView::Interactive);

  treeView_->setColumnWidth(UserGroupModel::ColPhase,      62);
  treeView_->setColumnWidth(UserGroupModel::ColProgress,   96);
  treeView_->setColumnWidth(UserGroupModel::ColPages,      44);
  treeView_->setColumnWidth(UserGroupModel::ColColor,      52);
  treeView_->setColumnWidth(UserGroupModel::ColDuplex,     48);
  treeView_->setColumnWidth(UserGroupModel::ColPaper,      70);
  treeView_->setColumnWidth(UserGroupModel::ColSubmitTime, 120);

  auto* groupDelegate = new UserGroupDelegate(treeView_);
  treeView_->setItemDelegate(groupDelegate);
  connect(groupDelegate, &UserGroupDelegate::documentClicked,
          this, [this](const QModelIndex& index) {
            const auto rowData = groupModel_->jobDataAt(index);
            if (!rowData.has_value()) return;
            const QString docName = QString::fromStdString(rowData->job.document_name);
            if (docName.isEmpty()) return;
            const QString jobFile = lastOutputDir_
              + QStringLiteral("/jobs/job-")
              + QString::number(rowData->job.job_id)
              + QStringLiteral(".json");
            if (QFile::exists(jobFile))
              QDesktopServices::openUrl(QUrl::fromLocalFile(jobFile));
            else
              QDesktopServices::openUrl(QUrl::fromLocalFile(lastOutputDir_));
            statusBar()->showMessage(
              QStringLiteral("已打开: Job %1 — %2")
                .arg(rowData->job.job_id).arg(docName), 3000);
          });

  emptyStateLabel_ = new QLabel(QStringLiteral("暂无打印作业"));
  emptyStateLabel_->setObjectName(QStringLiteral("EmptyState"));
  emptyStateLabel_->setAlignment(Qt::AlignCenter);

  tableStack_ = new QStackedWidget();
  tableStack_->setMinimumWidth(0);
  tableStack_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
  tableStack_->addWidget(emptyStateLabel_);
  tableStack_->addWidget(treeView_);
  treePanelLayout->addWidget(tableStack_, 1);

  // ── Right side: detail panel (full height, no timeline) ──────────
  detailPanel_ = new JobDetailPanel();
  detailPanel_->setMinimumWidth(340);
  detailPanel_->setMaximumWidth(520);
  detailPanel_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

  workspaceSplitter_ = new GripSplitter(Qt::Horizontal);
  workspaceSplitter_->setObjectName(QStringLiteral("WorkspaceSplitter"));
  workspaceSplitter_->setMinimumWidth(0);
  workspaceSplitter_->setChildrenCollapsible(false);
  workspaceSplitter_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
  workspaceSplitter_->addWidget(treePanel);
  workspaceSplitter_->addWidget(detailPanel_);
  workspaceSplitter_->setStretchFactor(0, 1);
  workspaceSplitter_->setStretchFactor(1, 0);
  workspaceSplitter_->setSizes(QList<int>{720, 380});

  // ── Bottom panel: receipt preview cards ───────────────────────────
  receiptPanel_ = new ReceiptPreviewPanel();

  verticalSplitter_ = new GripSplitter(Qt::Vertical);
  verticalSplitter_->setObjectName(QStringLiteral("VerticalSplitter"));
  verticalSplitter_->setMinimumWidth(0);
  verticalSplitter_->setChildrenCollapsible(false);
  verticalSplitter_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
  verticalSplitter_->addWidget(workspaceSplitter_);
  verticalSplitter_->addWidget(receiptPanel_);
  verticalSplitter_->setStretchFactor(0, 4);
  verticalSplitter_->setStretchFactor(1, 0);
  verticalSplitter_->setSizes(QList<int>{560, 170});
  root->addWidget(verticalSplitter_, 1);

  scheduleWorkspaceRebalance();

  statusBar()->showMessage(QStringLiteral("就绪"));

  // ── Button wiring ─────────────────────────────────────────────────
  connect(startBtn_, &QPushButton::clicked, this, &MainWindow::onStartClicked);
  connect(stopBtn_, &QPushButton::clicked, this, &MainWindow::onStopClicked);
  connect(openOutputBtn_, &QPushButton::clicked, this, &MainWindow::onOpenOutputClicked);
  connect(settingsBtn_, &QPushButton::clicked, this, &MainWindow::onSettingsClicked);
  connect(completeUserBtn_, &QPushButton::clicked, this, &MainWindow::onCompleteClicked);
  connect(expandAllBtn_, &QPushButton::clicked, this, [this]() {
    for (int i = 0; i < groupModel_->rowCount(); ++i)
    {
      if (treeView_->isRowHidden(i, QModelIndex())) continue;
      treeView_->expand(groupModel_->index(i, 0));
    }
    statusBar()->showMessage(QStringLiteral("已展开所有用户作业"), 2000);
  });
  connect(collapseAllBtn_, &QPushButton::clicked, this, [this]() {
    for (int i = 0; i < groupModel_->rowCount(); ++i)
      treeView_->collapse(groupModel_->index(i, 0));
    statusBar()->showMessage(QStringLiteral("已收起所有用户作业"), 2000);
  });
  connect(clearFilterBtn_, &QPushButton::clicked, this, [this]() {
    searchEdit_->clear();
  });

  // ── Search ────────────────────────────────────────────────────────
  connect(searchEdit_, &QLineEdit::textChanged, this, [this](const QString&) {
    applySearchFilter();
  });

  // ── Tree selection ────────────────────────────────────────────────
  connect(treeView_->selectionModel(), &QItemSelectionModel::currentRowChanged,
          this, &MainWindow::onTreeSelectionChanged);

  // ── Model data-changed → refresh ──────────────────────────────────
  connect(groupModel_, &QAbstractItemModel::dataChanged, this, [this]() {
    refreshSelectedDetail();
    refreshSummaryCards();
    refreshEmptyState();
    refreshReceiptPanel();
    scheduleWorkspaceRebalance();
  });
  connect(groupModel_, &QAbstractItemModel::rowsInserted, this, [this]() {
    refreshGroupRowSpans();
    applySearchFilter();
    refreshSummaryCards();
    refreshEmptyState();
    refreshReceiptPanel();
    scheduleWorkspaceRebalance();
    // Auto-expand all users so operators see every job at a glance.
    for (int i = 0; i < groupModel_->groupCount(); ++i)
      treeView_->expand(groupModel_->index(i, 0));
    QTimer::singleShot(0, this, &MainWindow::selectFirstIncompleteUser);
  });
  connect(groupModel_, &QAbstractItemModel::modelReset, this, [this]() {
    refreshGroupRowSpans();
    refreshSummaryCards();
    refreshEmptyState();
    refreshSelectedDetail();
    refreshReceiptPanel();
    scheduleWorkspaceRebalance();
  });

  // ── Bridge → Model ────────────────────────────────────────────────
  connect(bridge_, &WatcherBridge::jobSnapshotReceived,
          groupModel_, &UserGroupModel::onJobSnapshot);
  connect(bridge_, &WatcherBridge::jobSnapshotReceived,
          userTaskModel_, &UserTaskModel::onJobSnapshot);
  connect(bridge_, &WatcherBridge::metadataReceived,
          groupModel_, &UserGroupModel::onMetadata);
  connect(bridge_, &WatcherBridge::metadataReceived,
          this, [this](const core::MetadataSnapshot& metadata) {
            Q_UNUSED(metadata);
            refreshSelectedDetail();
          });
  connect(bridge_, &WatcherBridge::metadataReceived,
          userTaskModel_, &UserTaskModel::onMetadata);
  connect(bridge_, &WatcherBridge::lifecycleEventReceived,
          groupModel_, &UserGroupModel::onLifecycleEvent);
  connect(bridge_, &WatcherBridge::lifecycleEventReceived,
          userTaskModel_, &UserTaskModel::onLifecycleEvent);
  connect(bridge_, &WatcherBridge::watcherError, this,
          [this](const QString& error) {
            statusBar()->showMessage(QStringLiteral("警告: %1").arg(error), 5000);
            updateButtonStates();
            updatePrinterLabel();
          });
  connect(bridge_, &WatcherBridge::storageWarning, this,
          [this](const QString& warning) {
            statusBar()->showMessage(warning, 8000);
          });
  connect(bridge_, &WatcherBridge::watcherPermanentFailure, this,
          [this](const QString& printerName, const QString& error) {
            Q_UNUSED(printerName);
            statusBar()->showMessage(
              QStringLiteral("严重: %1").arg(error), 0);
            updateButtonStates();
            updatePrinterLabel();
          });
  connect(bridge_, &WatcherBridge::printerSkipped,
          this, &MainWindow::onPrinterSkipped);
  connect(bridge_, &WatcherBridge::sessionFinished, this,
          [this]() {
            userTaskModel_->flushSave();
            updateButtonStates();
            updatePrinterLabel();
          });
  connect(bridge_, &WatcherBridge::watcherHeartbeat,
          this, &MainWindow::onWatcherHeartbeat);

  // ── Group completion ──────────────────────────────────────────────
  connect(groupModel_, &UserGroupModel::userCompleted,
          this, &MainWindow::onUserCompleted);
  connect(groupModel_, &UserGroupModel::groupCountChanged,
          this, [this](int total, int active) {
            Q_UNUSED(total);
            userCountLabel_->setText(QString::number(active));
          });

  // ── Heartbeat timer ───────────────────────────────────────────────
  heartbeatCheckTimer_ = new QTimer(this);
  heartbeatCheckTimer_->setInterval(3500);
  connect(heartbeatCheckTimer_, &QTimer::timeout,
          this, &MainWindow::checkStalePrinters);
  heartbeatCheckTimer_->start();

  // ── Receipt panel wiring ──────────────────────────────────────────
  connect(userTaskModel_, &QAbstractItemModel::dataChanged,
          this, &MainWindow::refreshReceiptPanel);
  connect(userTaskModel_, &QAbstractItemModel::rowsInserted,
          this, &MainWindow::refreshReceiptPanel);
  connect(userTaskModel_, &QAbstractItemModel::modelReset,
          this, &MainWindow::refreshReceiptPanel);
  connect(groupModel_, &QAbstractItemModel::dataChanged,
          this, &MainWindow::refreshReceiptPanel);
  connect(groupModel_, &QAbstractItemModel::rowsInserted,
          this, &MainWindow::refreshReceiptPanel);
  connect(receiptPanel_, &ReceiptPreviewPanel::settleRequested,
          this, [this](const QString& userId) {
            groupModel_->completeUser(userId);
            userTaskModel_->completeUserTask(userId);
          });

  // ── Initialise ────────────────────────────────────────────────────
  updateButtonStates();
  updatePrinterLabel();
  refreshSummaryCards();
  refreshEmptyState();
  refreshGroupRowSpans();

  // ── XPS filter auto-install check ──────────────────────────────────
  // If running as admin, installs the filter silently (COM registration +
  // pipeline injection + spooler restart).  After one admin run, all
  // subsequent normal-user launches work without any setup.
  QString filterMsg = ensureFilterInstalled();
  if (!filterMsg.isEmpty())
    statusBar()->showMessage(filterMsg, 10000);

  QTimer::singleShot(100, this, &MainWindow::autoStart);
}

MainWindow::~MainWindow()
{
  config_.window.width = width();
  config_.window.height = height();
  common::AppConfig::ensure_config_file(common::AppConfig::default_config_path());
  config_.save_to_json(common::AppConfig::default_config_path());
  userTaskModel_->flushSave();
  bridge_->stopWatching();
}

// ── Actions ─────────────────────────────────────────────────────────

void MainWindow::resizeEvent(QResizeEvent* event)
{
  QMainWindow::resizeEvent(event);
  scheduleWorkspaceRebalance();
}

void MainWindow::scheduleWorkspaceRebalance()
{
  if (splitterRebalancePending_) return;

  splitterRebalancePending_ = true;
  QTimer::singleShot(0, this, [this]() {
    splitterRebalancePending_ = false;
    rebalanceWorkspaceSplitters();
  });
}

void MainWindow::rebalanceWorkspaceSplitters()
{
  if (workspaceSplitter_ == nullptr || verticalSplitter_ == nullptr) return;

  if (treeView_ != nullptr)
  {
    const int treeWidth = treeView_->viewport() != nullptr ? treeView_->viewport()->width() : treeView_->width();
    const bool compactTree = treeWidth > 0 && treeWidth < 760;
    treeView_->setColumnWidth(UserGroupModel::ColPhase, compactTree ? 56 : 60);
    treeView_->setColumnWidth(UserGroupModel::ColProgress, compactTree ? 84 : 92);
    treeView_->setColumnWidth(UserGroupModel::ColPages, compactTree ? 36 : 40);
    treeView_->setColumnWidth(UserGroupModel::ColColor, compactTree ? 44 : 48);
    treeView_->setColumnWidth(UserGroupModel::ColDuplex, compactTree ? 40 : 44);
    treeView_->setColumnWidth(UserGroupModel::ColPaper, compactTree ? 58 : 66);
    treeView_->setColumnWidth(UserGroupModel::ColSubmitTime, compactTree ? 104 : 116);
  }

  const int workspaceWidth = workspaceSplitter_->width();
  if (workspaceWidth > 0)
  {
    const QList<int> sizes = workspaceSplitter_->sizes();
    const int currentSideWidth = sizes.size() > 1 ? sizes.at(1) : 0;
    const int minimumUsefulSideWidth = std::min(400, std::max(340, workspaceWidth / 3));

    if (currentSideWidth < minimumUsefulSideWidth)
    {
      const int targetSideWidth = std::clamp((workspaceWidth * 36) / 100,
                                             minimumUsefulSideWidth,
                                             480);
      const int targetTreeWidth = std::max(320, workspaceWidth - targetSideWidth);
      workspaceSplitter_->setSizes(QList<int>{targetTreeWidth, targetSideWidth});
    }
  }

  const int verticalHeight = verticalSplitter_->height();
  if (verticalHeight > 0)
  {
    const QList<int> sizes = verticalSplitter_->sizes();
    const int minimumWorkspaceHeight = std::min(480, std::max(360, (verticalHeight * 65) / 100));
    if (sizes.size() > 1 && sizes.at(0) < minimumWorkspaceHeight)
    {
      const int maxWorkspaceHeight = std::max(200, verticalHeight - 170);
      const int workspaceHeight = std::clamp((verticalHeight * 72) / 100,
                                             std::min(minimumWorkspaceHeight, maxWorkspaceHeight),
                                             maxWorkspaceHeight);
      verticalSplitter_->setSizes(QList<int>{workspaceHeight, std::max(170, verticalHeight - workspaceHeight)});
    }
  }

  detailPanel_->raise();
}

void MainWindow::onStartClicked()
{
  if (bridge_->isRunning()) return;

  userTaskModel_->flushSave();
  groupModel_->clear();
  userTaskModel_->clear();
  detailPanel_->clear();
  refreshReceiptPanel();

  lastOutputDir_ = resolveOutputRoot();
  QDir().mkpath(lastOutputDir_);

  const QString tasksFile = lastOutputDir_ + QStringLiteral("/user_tasks.json");
  userTaskModel_->setAutoSavePath(tasksFile);
  if (QFile::exists(tasksFile))
  {
    userTaskModel_->loadFromJson(tasksFile);
    refreshReceiptPanel();
  }

  const QString nativeOutputDir = QDir::toNativeSeparators(lastOutputDir_);
  outputDirLabel_->setText(compactPathText(nativeOutputDir));
  outputDirLabel_->setToolTip(nativeOutputDir);

  bridge_->setStallTimeout(std::chrono::seconds{config_.monitoring.stall_timeout_seconds});
  bridge_->startWatching(lastOutputDir_);
  updateButtonStates();
  updatePrinterLabel();
  refreshSummaryCards();

  statusBar()->showMessage(
    QStringLiteral("监听中 -> %1").arg(QDir::toNativeSeparators(lastOutputDir_)));
}

void MainWindow::onStopClicked()
{
  bridge_->stopWatching();
  userTaskModel_->flushSave();
  updateButtonStates();
  updatePrinterLabel();
  statusBar()->showMessage(QStringLiteral("已停止"));
}

void MainWindow::onOpenOutputClicked()
{
  if (lastOutputDir_.isEmpty()) return;
  QDesktopServices::openUrl(QUrl::fromLocalFile(lastOutputDir_));
}

void MainWindow::onSettingsClicked()
{
  SettingsDialog dialog(config_, this);
  if (dialog.exec() == QDialog::Accepted)
  {
    userTaskModel_->setPricingConfig(&config_.pricing);
    applyConfig();
    statusBar()->showMessage(QStringLiteral("设置已保存"), 3000);
  }
}

void MainWindow::onCompleteClicked()
{
  QModelIndex current = treeView_->currentIndex();
  if (!current.isValid())
  {
    statusBar()->showMessage(QStringLiteral("请先选择一个用户"), 3000);
    return;
  }

  // Walk up to the group row.
  while (current.isValid() && !groupModel_->isGroupRow(current))
    current = current.parent();

  if (!current.isValid() || groupModel_->isGroupCompleted(current))
  {
    statusBar()->showMessage(QStringLiteral("请选择一个未结算的用户"), 3000);
    return;
  }

  const QString userId = groupModel_->userIdAt(current);
  const auto summary = groupModel_->groupSummaryAt(current);
  int jobCount = 0;
  double totalCost = 0.0;
  std::vector<JobCostLine> costLines;

  for (int r = 0; r < groupModel_->rowCount(current); ++r)
  {
    QModelIndex childIdx = groupModel_->index(r, 0, current);
    auto jd = groupModel_->jobDataAt(childIdx);
    if (!jd.has_value()) continue;

    ++jobCount;
    const double jobCost = calculate_job_cost(jd->metadata, config_.pricing);
    totalCost += jobCost;

    JobCostLine line;
    line.documentName = QString::fromStdString(jd->job.document_name);
    line.pages = parse_total_pages(jd->metadata);
    if (jd->metadata.color.state == core::FieldState::Reported)
      line.colorMode = QString::fromStdString(jd->metadata.color.value);
    if (jd->metadata.duplex.state == core::FieldState::Reported)
      line.duplexMode = QString::fromStdString(jd->metadata.duplex.value);
    if (jd->metadata.paper_size.state == core::FieldState::Reported)
      line.paperSize = QString::fromStdString(jd->metadata.paper_size.value);
    line.price = jobCost;
    costLines.push_back(std::move(line));
  }

  if (jobCount <= 0)
  {
    statusBar()->showMessage(QStringLiteral("该用户没有作业"), 3000);
    return;
  }

  // ── Confirm with dialog ────────────────────────────────
  CompletionDialog dlg(userId, costLines, config_.pricing, this);
  if (dlg.exec() != QDialog::Accepted)
    return;

  groupModel_->completeUser(userId);
  userTaskModel_->completeUserTask(userId);
  statusBar()->showMessage(
    QStringLiteral("已结算: %1  (%2 作业, %3 页, ¥%4)")
      .arg(userId)
      .arg(jobCount)
      .arg(summary.simplexPages + summary.duplexPages + summary.specialPaperPages)
      .arg(QString::number(totalCost, 'f', 2)),
    5000);
}

void MainWindow::onTreeSelectionChanged()
{
  refreshSelectedDetail();

  // Walk to group row and enable/disable complete button.
  QModelIndex current = treeView_->currentIndex();
  while (current.isValid() && !groupModel_->isGroupRow(current))
    current = current.parent();

  completeUserBtn_->setEnabled(
    current.isValid() && !groupModel_->isGroupCompleted(current));

  // ── Current-user price ──────────────────────────────────────────
  if (current.isValid() && !groupModel_->isGroupCompleted(current))
  {
    const QString userId = groupModel_->userIdAt(current);
    double total = 0.0;
    for (int r = 0; r < groupModel_->rowCount(current); ++r)
    {
      QModelIndex childIdx = groupModel_->index(r, 0, current);
      auto jd = groupModel_->jobDataAt(childIdx);
      if (jd.has_value())
        total += calculate_job_cost(jd->metadata, config_.pricing);
    }
    // Current-user price is shown in the receipt card and detail panel.
    Q_UNUSED(userId);
    Q_UNUSED(total);
  }
  else
  {
    // No selection — receipt panel still shows all active orders.
  }

  scheduleWorkspaceRebalance();
}

void MainWindow::onPrinterSkipped(const QString& printerName)
{
  statusBar()->showMessage(
    QStringLiteral("已跳过打印机: %1").arg(printerName), 3000);
}

void MainWindow::onWatcherHeartbeat(const QString& printerName)
{
  printerHeartbeats_[printerName] = QDateTime::currentDateTimeUtc();
}

void MainWindow::onUserCompleted(const QString& userId)
{
  Q_UNUSED(userId);
  refreshSummaryCards();
  refreshReceiptPanel();
  selectFirstIncompleteUser();
  onTreeSelectionChanged();
}

void MainWindow::checkStalePrinters()
{
  if (!bridge_->isRunning()) return;

  const auto now = QDateTime::currentDateTimeUtc();
  QStringList stale;
  for (const QString& name : bridge_->watchedPrinters())
  {
    auto it = printerHeartbeats_.find(name);
    if (it == printerHeartbeats_.end())
    {
      printerHeartbeats_[name] = now;
      continue;
    }
    if (it->secsTo(now) > 8)
      stale.append(name);
  }

  if (!stale.isEmpty())
  {
    statusBar()->showMessage(
      QStringLiteral("打印机无响应: %1").arg(stale.join(QStringLiteral(", "))),
      5000);
  }
}

void MainWindow::autoStart()
{
  onStartClicked();
}

// ── UI state ─────────────────────────────────────────────────────────

void MainWindow::updateButtonStates()
{
  const bool running = bridge_->isRunning();
  startBtn_->setEnabled(!running);
  stopBtn_->setEnabled(running);
  openOutputBtn_->setEnabled(!lastOutputDir_.isEmpty());
}

void MainWindow::updatePrinterLabel()
{
  const bool running = bridge_->isRunning();
  sessionStateLabel_->setText(running ? QStringLiteral("运行中")
                                      : QStringLiteral("待启动"));
  sessionStateLabel_->setProperty("state", running ? "running" : "idle");
  repolish(sessionStateLabel_);

  const int n = running ? bridge_->watchedPrinters().size() : 0;
  printerCountLabel_->setText(QString::number(n));
}

void MainWindow::refreshSummaryCards()
{
  const int totalGroups = groupModel_->groupCount();
  const int totalJobs = groupModel_->totalJobCount();
  int activeUsers = 0;
  for (int i = 0; i < totalGroups; ++i)
  {
    QModelIndex idx = groupModel_->index(i, 0);
    if (!groupModel_->isGroupCompleted(idx))
      ++activeUsers;
  }

  userCountLabel_->setText(QString::number(activeUsers));
  resultCountLabel_->setText(
    QStringLiteral("%1 用户 / %2 作业").arg(totalGroups).arg(totalJobs));

  const bool filtersActive = !searchEdit_->text().trimmed().isEmpty();
  clearFilterBtn_->setEnabled(filtersActive);

  // Update complete button state.
  onTreeSelectionChanged();
}

void MainWindow::applySearchFilter()
{
  if (treeView_ == nullptr || groupModel_ == nullptr) return;

  const QString norm = searchEdit_->text().trimmed();
  treeView_->setUpdatesEnabled(false);
  for (int i = 0; i < groupModel_->rowCount(); ++i)
  {
    QModelIndex idx = groupModel_->index(i, 0);
    const QString userId = groupModel_->userIdAt(idx);
    const bool match = norm.isEmpty() ||
      userId.contains(norm, Qt::CaseInsensitive);
    treeView_->setRowHidden(i, QModelIndex(), !match);
    if (match && !norm.isEmpty())
      treeView_->expand(idx);
    else if (norm.isEmpty())
      treeView_->collapse(idx);  // restore collapsed default when filter cleared
  }
  treeView_->setUpdatesEnabled(true);
  clearFilterBtn_->setEnabled(!norm.isEmpty());
  refreshSummaryCards();
}

void MainWindow::refreshEmptyState()
{
  refreshSummaryCards();

  const bool hasVisible = groupModel_->groupCount() > 0;
  tableStack_->setCurrentWidget(
    hasVisible ? static_cast<QWidget*>(treeView_)
               : static_cast<QWidget*>(emptyStateLabel_));

  if (hasVisible) return;

  emptyStateLabel_->setText(
    bridge_->isRunning()
      ? QStringLiteral("暂无打印作业")
      : QStringLiteral("点击「开始监听」启动"));
}

void MainWindow::refreshGroupRowSpans()
{
  if (treeView_ == nullptr || groupModel_ == nullptr) return;

  for (int i = 0; i < groupModel_->groupCount(); ++i)
    treeView_->setFirstColumnSpanned(i, QModelIndex(), true);
}

void MainWindow::refreshSelectedDetail()
{
  QModelIndex current = treeView_->currentIndex();
  if (!current.isValid())
  {
    detailPanel_->clear();
    return;
  }

  if (groupModel_->isGroupRow(current))
  {
    // Group row — show rich summary card with aggregated stats.
    const QString userId = groupModel_->userIdAt(current);
    const auto summary = groupModel_->groupSummaryAt(current);
    bool completed = groupModel_->isGroupCompleted(current);

    // Count active (non-terminal) child jobs.
    int activeJobs = 0;
    for (int r = 0; r < groupModel_->rowCount(current); ++r)
    {
      QModelIndex childIdx = groupModel_->index(r, 0, current);
      auto jd = groupModel_->jobDataAt(childIdx);
      if (jd.has_value() && !is_terminal_phase(jd->phase))
        ++activeJobs;
    }

    detailPanel_->showGroupSummary(userId, summary, activeJobs, completed);
  }
  else
  {
    // Job row — delegate to existing detail panel.
    auto jd = groupModel_->jobDataAt(current);
    if (jd.has_value())
      detailPanel_->showJob(jd->job, jd->metadata);
  }
}

void MainWindow::selectFirstIncompleteUser()
{
  if (groupModel_->groupCount() == 0) return;

  QModelIndex current = treeView_->currentIndex();
  while (current.isValid() && !groupModel_->isGroupRow(current))
    current = current.parent();

  if (current.isValid()
      && !groupModel_->isGroupCompleted(current)
      && !treeView_->isRowHidden(current.row(), QModelIndex()))
  {
    return;
  }

  for (int i = 0; i < groupModel_->groupCount(); ++i)
  {
    if (treeView_->isRowHidden(i, QModelIndex())) continue;

    QModelIndex idx = groupModel_->index(i, 0);
    if (!groupModel_->isGroupCompleted(idx))
    {
      treeView_->selectionModel()->setCurrentIndex(
        idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
      treeView_->expand(idx);
      treeView_->scrollTo(idx, QAbstractItemView::PositionAtCenter);
      return;
    }
  }
}

void MainWindow::refreshReceiptPanel()
{
  if (receiptPanel_ == nullptr || groupModel_ == nullptr || userTaskModel_ == nullptr)
    return;

  receiptPanel_->clearCards();

  // Pass 1: active (in-progress) users first.
  for (int i = 0; i < groupModel_->groupCount(); ++i)
  {
    QModelIndex groupIdx = groupModel_->index(i, 0);
    if (groupModel_->isGroupCompleted(groupIdx))
      continue;

    const QString userId = groupModel_->userIdAt(groupIdx);
    const auto summary = groupModel_->groupSummaryAt(groupIdx);

    // Find matching task in userTaskModel for order-id and price.
    const auto* task = userTaskModel_->taskForUser(userId);
    QString taskId = (task != nullptr) ? task->taskId : QStringLiteral("—");
    double totalPrice = (task != nullptr) ? task->totalPrice : 0.0;
    if (totalPrice <= 0.0)
    {
      // Fallback: compute price on the fly from child jobs.
      for (int r = 0; r < groupModel_->rowCount(groupIdx); ++r)
      {
        QModelIndex childIdx = groupModel_->index(r, 0, groupIdx);
        auto jd = groupModel_->jobDataAt(childIdx);
        if (jd.has_value())
          totalPrice += calculate_job_cost(jd->metadata, config_.pricing);
      }
    }

    // Gather first/last document names.
    QString firstDoc, lastDoc;
    int childCount = groupModel_->rowCount(groupIdx);
    if (childCount > 0)
    {
      auto first = groupModel_->jobDataAt(groupModel_->index(0, 0, groupIdx));
      if (first.has_value())
        firstDoc = QString::fromStdString(first->job.document_name);
      if (childCount > 1)
      {
        auto last = groupModel_->jobDataAt(groupModel_->index(childCount - 1, 0, groupIdx));
        if (last.has_value())
          lastDoc = QString::fromStdString(last->job.document_name);
      }
      else
      {
        lastDoc = firstDoc;
      }
    }

    int totalPages = summary.simplexPages + summary.duplexPages + summary.specialPaperPages;
    QColor userColor = groupIdx.data(UserGroupModel::RowColorRole).value<QColor>();

    receiptPanel_->addCard(taskId, userId, firstDoc, lastDoc,
                           totalPages, totalPrice, userColor, /*completed=*/false);
  }

  // Pass 2: completed users as grayed-out placeholders.
  for (int i = 0; i < groupModel_->groupCount(); ++i)
  {
    QModelIndex groupIdx = groupModel_->index(i, 0);
    if (!groupModel_->isGroupCompleted(groupIdx))
      continue;

    const QString userId = groupModel_->userIdAt(groupIdx);
    const auto summary = groupModel_->groupSummaryAt(groupIdx);

    // Recover task info from UserTaskModel (completed tasks are no longer
    // in the active index — look up by user's task in the full task list).
    QString taskId = QStringLiteral("—");
    double totalPrice = 0.0;
    for (int t = 0; t < userTaskModel_->rowCount(); ++t)
    {
      const auto* task = userTaskModel_->taskAtRow(t);
      if (task != nullptr && task->userId == userId)
      {
        taskId = task->taskId;
        totalPrice = task->totalPrice;
        break;
      }
    }

    // Gather first/last document names.
    QString firstDoc, lastDoc;
    int childCount = groupModel_->rowCount(groupIdx);
    if (childCount > 0)
    {
      auto first = groupModel_->jobDataAt(groupModel_->index(0, 0, groupIdx));
      if (first.has_value())
        firstDoc = QString::fromStdString(first->job.document_name);
      if (childCount > 1)
      {
        auto last = groupModel_->jobDataAt(groupModel_->index(childCount - 1, 0, groupIdx));
        if (last.has_value())
          lastDoc = QString::fromStdString(last->job.document_name);
      }
      else
      {
        lastDoc = firstDoc;
      }
    }

    int totalPages = summary.simplexPages + summary.duplexPages + summary.specialPaperPages;
    QColor userColor = groupIdx.data(UserGroupModel::RowColorRole).value<QColor>();

    receiptPanel_->addCard(taskId, userId, firstDoc, lastDoc,
                           totalPages, totalPrice, userColor, /*completed=*/true);
  }
}

// ── Config helpers ──────────────────────────────────────────────────

void MainWindow::loadOrCreateConfig()
{
  const auto cfgPath = common::AppConfig::default_config_path();
  if (std::filesystem::exists(cfgPath))
  {
    config_ = common::AppConfig::defaults();
    if (!config_.load_from_json(cfgPath))
    {
      config_ = common::AppConfig::defaults();
      config_.save_to_json(cfgPath);
    }
  }
  else
  {
    config_ = common::AppConfig::defaults();
    common::AppConfig::ensure_config_file(cfgPath);
  }
}

void MainWindow::applyConfig()
{
  userTaskModel_->setPricingConfig(&config_.pricing);
  const QString outputRoot = QString::fromStdString(config_.storage.output_root);
  outputDirLabel_->setText(compactPathText(outputRoot));
  outputDirLabel_->setToolTip(outputRoot);
}

QString MainWindow::resolveOutputRoot() const
{
  QString root = QString::fromStdString(config_.storage.output_root);
  if (QDir::isRelativePath(root))
  {
    const QString exeDir = QCoreApplication::applicationDirPath();
    root = QDir(exeDir).filePath(root);
  }
  if (config_.storage.use_timestamp_subdir)
  {
    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    root = QDir(root).filePath(QStringLiteral("capture-%1").arg(ts));
  }
  return QDir::toNativeSeparators(root);
}

}  // namespace wpce::ui
