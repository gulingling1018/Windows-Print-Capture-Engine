#include "ui/main_window.h"

#include "bridge/settlement_bridge.h"
#include "common/app_config.h"
#include "common/pricing_engine.h"
#include "ui/grip_splitter.h"
#include "ui/completion_dialog.h"
#include "ui/history_explorer_dialog.h"
#include "ui/job_detail_panel.h"
#include "ui/grip_splitter.h"
#include "ui/pricing_config.h"
#include "ui/quick_settle_dialog.h"
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
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
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

}  // namespace (anonymous)

}  // namespace wpce::ui

// ── Includes for MainWindow implementation ───────────────────────────

#include "billing/billing_service.h"
#include "bridge/settlement_bridge.h"
#include "common/app_config.h"
#include "common/pricing_engine.h"
#include "common/printer_profile.h"
#include "ui/billing_batch_model.h"
#include "ui/grip_splitter.h"
#include "ui/completion_dialog.h"
#include "ui/job_detail_panel.h"
#include "ui/pricing_config.h"
#include "ui/quick_settle_dialog.h"
#include "ui/receipt_preview_panel.h"
#include "ui/settings_dialog.h"
#include "ui/user_color_palette.h"
#include "ui/user_group_delegate.h"
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

// ── MainWindow ────────────────────────────────────────────────────────

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
  loadOrCreateConfig();
  detectPrintersIfNeeded();

  setWindowTitle(QStringLiteral("WPCE 打印捕获控制台"));
  resize(config_.window.width, config_.window.height);
  setMinimumSize(1040, 640);

  bridge_ = new WatcherBridge(this);
  billingService_ = new BillingService(this);
  billingService_->setPricingConfig(&config_.pricing);
  billingService_->setPrinterProfiles(&config_.printers);
  batchModel_ = new BillingBatchModel(billingService_, this);

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
  auto* subtitle = new QLabel(QStringLiteral("按时间段批次 · 打印计价与结算"));
  subtitle->setObjectName(QStringLiteral("WindowSubtitle"));
  subtitle->setMinimumWidth(0);
  subtitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  titleBlock->addWidget(title);
  titleBlock->addWidget(subtitle);
  heroLayout->addLayout(titleBlock, 1);

  startBtn_ = new QPushButton(QStringLiteral("开始"));
  startBtn_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
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

  auto* historyBtn = new QPushButton(QStringLiteral("历史"));
  historyBtn->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
  setButtonRole(historyBtn, "quiet");
  makeIconButton(historyBtn, QStringLiteral("结算历史"));
  connect(historyBtn, &QPushButton::clicked, this, [this]() {
    if (historyDialog_ == nullptr)
      historyDialog_ = new HistoryExplorerDialog(resolveStateDir(), this);
    historyDialog_->loadOrders();
    historyDialog_->show();
    historyDialog_->raise();
  });

  heroLayout->addWidget(startBtn_);
  heroLayout->addWidget(stopBtn_);
  heroLayout->addWidget(openOutputBtn_);
  heroLayout->addWidget(historyBtn);
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
  metrics->addWidget(makeMetricCard(QStringLiteral("活跃批次"), userCountLabel_), 0);
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

  auto* treeTitle = new QLabel(QStringLiteral("批次 / 作业"));
  treeTitle->setObjectName(QStringLiteral("PanelHeader"));
  treeTitle->setMinimumWidth(0);
  treeTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

  resultCountLabel_ = new QLabel(QStringLiteral("0 批次 / 0 作业"));
  resultCountLabel_->setObjectName(QStringLiteral("MutedText"));
  resultCountLabel_->setMinimumWidth(0);
  resultCountLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

  searchEdit_ = new QLineEdit();
  searchEdit_->setObjectName(QStringLiteral("SearchBox"));
  searchEdit_->setPlaceholderText(QStringLiteral("搜索批次"));
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
  expandAllBtn_->setToolTip(QStringLiteral("展开所有批次下的作业"));
  setButtonRole(expandAllBtn_, "quiet");
  makeIconButton(expandAllBtn_, QStringLiteral("展开所有批次下的作业"));

  collapseAllBtn_ = new QPushButton(QStringLiteral("全部收起"));
  collapseAllBtn_->setIcon(style()->standardIcon(QStyle::SP_ArrowRight));
  collapseAllBtn_->setToolTip(QStringLiteral("收起所有批次，只保留批次行"));
  setButtonRole(collapseAllBtn_, "quiet");
  makeIconButton(collapseAllBtn_, QStringLiteral("收起所有批次，只保留批次行"));

  completeUserBtn_ = new QPushButton(QStringLiteral("结算"));
  completeUserBtn_->setIcon(style()->standardIcon(QStyle::SP_DialogApplyButton));
  completeUserBtn_->setToolTip(QStringLiteral("结算选中批次"));
  completeUserBtn_->setMinimumWidth(78);
  completeUserBtn_->setMaximumWidth(88);
  setButtonRole(completeUserBtn_, "primary");
  completeUserBtn_->setEnabled(false);

  quickSettleBtn_ = new QPushButton(QStringLiteral("快速结算"));
  quickSettleBtn_->setToolTip(QStringLiteral("打开悬浮结算窗口"));
  quickSettleBtn_->setMinimumWidth(78);
  quickSettleBtn_->setMaximumWidth(96);
  setButtonRole(quickSettleBtn_, "primary");
  quickSettleBtn_->setEnabled(false);

  treeHeader->addWidget(treeTitle);
  treeHeader->addWidget(resultCountLabel_);
  treeHeader->addStretch(1);
  treeHeader->addWidget(expandAllBtn_);
  treeHeader->addWidget(collapseAllBtn_);
  treeHeader->addWidget(searchEdit_);
  treeHeader->addWidget(clearFilterBtn_);
  treeHeader->addWidget(completeUserBtn_);
  treeHeader->addWidget(quickSettleBtn_);
  treePanelLayout->addLayout(treeHeader);

  // ── QTreeView ─────────────────────────────────────────────────────
  treeView_ = new QTreeView();
  treeView_->setObjectName(QStringLiteral("JobTree"));
  treeView_->setMinimumWidth(0);
  treeView_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
  treeView_->setModel(batchModel_);
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
  treeView_->setRootIsDecorated(true);

  // ── Context menu for operator corrections (P9) ───────────────────
  treeView_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(treeView_, &QTreeView::customContextMenuRequested,
          this, [this](const QPoint& pos) {
    QModelIndex index = treeView_->indexAt(pos);
    if (!index.isValid()) return;

    QMenu menu(this);

    if (batchModel_->isJobRow(index))
    {
      // Job row — offer removal / move.
      std::uint32_t jobId = batchModel_->jobIdAt(index);
      QString batchId = batchModel_->batchIdAt(index);
      const auto* batch = batchModel_->batchAt(index.parent());
      bool canModify = batch != nullptr && !batch->is_settled();

      auto* removeAction = menu.addAction(QStringLiteral("从批次移除作业"));
      removeAction->setEnabled(canModify);
      connect(removeAction, &QAction::triggered, this, [this, batchId, jobId]() {
        billingService_->removeJobFromBatch(batchId, jobId);
      });

      auto* deleteAction = menu.addAction(QStringLiteral("彻底删除作业"));
      deleteAction->setEnabled(canModify);
      connect(deleteAction, &QAction::triggered, this, [this, jobId]() {
        billingService_->deleteJob(jobId);
      });
    }
    else if (batchModel_->isBatchRow(index))
    {
      const auto* batch = batchModel_->batchAt(index);
      if (batch == nullptr) return;
      QString batchId = batchModel_->batchIdAt(index);

      if (!batch->is_settled())
      {
        auto* suspendAction = menu.addAction(QStringLiteral("挂单"));
        suspendAction->setEnabled(batch->is_active());
        connect(suspendAction, &QAction::triggered, this, [this, batchId]() {
          billingService_->suspend(batchId);
        });

        auto* resumeAction = menu.addAction(QStringLiteral("恢复接收"));
        resumeAction->setEnabled(batch->is_suspended());
        connect(resumeAction, &QAction::triggered, this, [this, batchId]() {
          billingService_->resume(batchId);
        });

        menu.addSeparator();

        auto* renameAction = menu.addAction(QStringLiteral("重命名批次..."));
        connect(renameAction, &QAction::triggered, this, [this, batchId, batch]() {
          bool ok = false;
          QString newName = QInputDialog::getText(this,
            QStringLiteral("重命名批次"), QStringLiteral("新名称:"),
            QLineEdit::Normal, QString::fromStdString(batch->display_name), &ok);
          if (ok && !newName.trimmed().isEmpty())
            billingService_->renameBatch(batchId, newName.trimmed());
        });
      }
    }

    if (!menu.actions().isEmpty())
      menu.exec(treeView_->viewport()->mapToGlobal(pos));
  });
  treeView_->header()->setHighlightSections(false);
  treeView_->header()->setStretchLastSection(false);
  treeView_->header()->setSectionsClickable(true);
  treeView_->header()->setMinimumSectionSize(28);
  treeView_->header()->setCascadingSectionResizes(false);

  treeView_->header()->setSectionResizeMode(BillingBatchModel::ColName,       QHeaderView::Interactive);
  treeView_->header()->setSectionResizeMode(BillingBatchModel::ColState,      QHeaderView::Interactive);
  treeView_->header()->setSectionResizeMode(BillingBatchModel::ColProgress,   QHeaderView::Interactive);
  treeView_->header()->setSectionResizeMode(BillingBatchModel::ColPages,      QHeaderView::Interactive);
  treeView_->header()->setSectionResizeMode(BillingBatchModel::ColDuplex,     QHeaderView::Interactive);
  treeView_->header()->setSectionResizeMode(BillingBatchModel::ColPaper,      QHeaderView::Interactive);
  treeView_->header()->setSectionResizeMode(BillingBatchModel::ColSubmitTime, QHeaderView::Interactive);
  treeView_->header()->setSectionResizeMode(BillingBatchModel::ColPrice,      QHeaderView::Interactive);

  treeView_->setColumnWidth(BillingBatchModel::ColName,       200);
  treeView_->setColumnWidth(BillingBatchModel::ColState,      62);
  treeView_->setColumnWidth(BillingBatchModel::ColProgress,   110);
  treeView_->setColumnWidth(BillingBatchModel::ColPages,      48);
  treeView_->setColumnWidth(BillingBatchModel::ColDuplex,     70);
  treeView_->setColumnWidth(BillingBatchModel::ColPaper,      70);
  treeView_->setColumnWidth(BillingBatchModel::ColSubmitTime, 130);
  treeView_->setColumnWidth(BillingBatchModel::ColPrice,      70);

  emptyStateLabel_ = new QLabel(QStringLiteral("暂无打印作业"));
  emptyStateLabel_->setObjectName(QStringLiteral("EmptyState"));
  emptyStateLabel_->setAlignment(Qt::AlignCenter);

  tableStack_ = new QStackedWidget();
  tableStack_->setMinimumWidth(0);
  tableStack_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
  tableStack_->addWidget(emptyStateLabel_);
  tableStack_->addWidget(treeView_);
  treePanelLayout->addWidget(tableStack_, 1);

  // ── Right side: detail panel ──────────────────────────────────────
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
  connect(quickSettleBtn_, &QPushButton::clicked, this, [this]() {
    if (quickSettleDialog_ == nullptr)
    {
      quickSettleDialog_ = new QuickSettleDialog(batchModel_, billingService_, &config_.pricing, this);
      connect(quickSettleDialog_, &QuickSettleDialog::settleRequested,
              this, [this](const QString& batchId) { billingService_->settle(batchId); });
      connect(quickSettleDialog_, &QuickSettleDialog::hangUpRequested,
              this, [this](const QString& batchId) { billingService_->suspend(batchId); });
      connect(quickSettleDialog_, &QuickSettleDialog::resumeRequested,
              this, [this](const QString& batchId) { billingService_->resume(batchId); });
    }
    int targetIndex = -1;
    QModelIndex current = treeView_->currentIndex();
    while (current.isValid() && !batchModel_->isBatchRow(current))
      current = current.parent();
    if (current.isValid())
      targetIndex = current.row();
    else
      targetIndex = batchModel_->currentActiveRow();
    if (targetIndex < 0)
      targetIndex = batchModel_->nextRelevantRow(-1);
    if (targetIndex >= 0)
      quickSettleDialog_->setCurrentBatchIndex(targetIndex);
    quickSettleDialog_->show();
    quickSettleDialog_->raise();
    quickSettleDialog_->activateWindow();
  });
  connect(expandAllBtn_, &QPushButton::clicked, this, [this]() {
    for (int i = 0; i < batchModel_->rowCount(); ++i)
    {
      if (treeView_->isRowHidden(i, QModelIndex())) continue;
      treeView_->expand(batchModel_->index(i, 0));
    }
  });
  connect(collapseAllBtn_, &QPushButton::clicked, this, [this]() {
    for (int i = 0; i < batchModel_->rowCount(); ++i)
      treeView_->collapse(batchModel_->index(i, 0));
  });
  connect(clearFilterBtn_, &QPushButton::clicked, this, [this]() {
    searchEdit_->clear();
    applySearchFilter();
  });
  connect(searchEdit_, &QLineEdit::textChanged, this, &MainWindow::applySearchFilter);
  connect(treeView_->selectionModel(), &QItemSelectionModel::currentChanged,
          this, &MainWindow::onTreeSelectionChanged);

  // ── WatcherBridge → BillingService wiring ─────────────────────────
  connect(bridge_, &WatcherBridge::jobSnapshotReceived,
          billingService_, &BillingService::onJobSnapshot);
  connect(bridge_, &WatcherBridge::metadataReceived,
          billingService_, &BillingService::onMetadata);
  connect(bridge_, &WatcherBridge::lifecycleEventReceived,
          billingService_, &BillingService::onLifecycleEvent);
  connect(bridge_, &WatcherBridge::watcherError, this,
          [this](const QString& error) {
            statusBar()->showMessage(QStringLiteral("警告: %1").arg(error), 5000);
            updateButtonStates(); updatePrinterLabel();
          });
  connect(bridge_, &WatcherBridge::storageWarning, this,
          [this](const QString& warning) { statusBar()->showMessage(warning, 8000); });
  connect(bridge_, &WatcherBridge::watcherPermanentFailure, this,
          [this](const QString&, const QString& error) {
            statusBar()->showMessage(QStringLiteral("严重: %1").arg(error), 0);
            updateButtonStates(); updatePrinterLabel();
          });
  connect(bridge_, &WatcherBridge::printerSkipped, this, &MainWindow::onPrinterSkipped);
  connect(bridge_, &WatcherBridge::sessionFinished, this, [this]() {
    billingService_->flushSave(); updateButtonStates(); updatePrinterLabel();
  });
  connect(bridge_, &WatcherBridge::watcherHeartbeat, this, &MainWindow::onWatcherHeartbeat);

  // ── BillingService → UI refresh ───────────────────────────────────
  connect(billingService_, &BillingService::batchesChanged, this, [this]() {
    refreshSummaryCards(); refreshEmptyState(); refreshGroupRowSpans(); refreshReceiptPanel();
  });
  connect(billingService_, &BillingService::batchUpdated, this, [this](const QString&) {
    refreshSummaryCards(); refreshReceiptPanel(); refreshSelectedDetail();
  });
  connect(billingService_, &BillingService::orderSettled, this, &MainWindow::onBatchSettled);

  connect(receiptPanel_, &ReceiptPreviewPanel::settleRequested,
          this, [this](const QString& batchId) { billingService_->settle(batchId); });

  // ── Heartbeat timer ───────────────────────────────────────────────
  heartbeatCheckTimer_ = new QTimer(this);
  heartbeatCheckTimer_->setInterval(3500);
  connect(heartbeatCheckTimer_, &QTimer::timeout, this, &MainWindow::checkStalePrinters);
  heartbeatCheckTimer_->start();

  updateButtonStates(); updatePrinterLabel();
  refreshSummaryCards(); refreshEmptyState(); refreshGroupRowSpans();

  QString filterMsg = ensureFilterInstalled();
  if (!filterMsg.isEmpty()) statusBar()->showMessage(filterMsg, 10000);

  QTimer::singleShot(100, this, &MainWindow::autoStart);
}

MainWindow::~MainWindow()
{
  config_.window.width = width();
  config_.window.height = height();
  common::AppConfig::ensure_config_file(common::AppConfig::default_config_path());
  config_.save_to_json(common::AppConfig::default_config_path());
  billingService_->flushSave();
  bridge_->stopWatching();
}

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
    treeView_->setColumnWidth(BillingBatchModel::ColName,       compactTree ? 160 : 200);
    treeView_->setColumnWidth(BillingBatchModel::ColState,      compactTree ? 56 : 62);
    treeView_->setColumnWidth(BillingBatchModel::ColProgress,   compactTree ? 90 : 110);
    treeView_->setColumnWidth(BillingBatchModel::ColPages,      compactTree ? 40 : 48);
    treeView_->setColumnWidth(BillingBatchModel::ColDuplex,     compactTree ? 56 : 70);
    treeView_->setColumnWidth(BillingBatchModel::ColPaper,      compactTree ? 58 : 70);
    treeView_->setColumnWidth(BillingBatchModel::ColSubmitTime, compactTree ? 110 : 130);
    treeView_->setColumnWidth(BillingBatchModel::ColPrice,      compactTree ? 56 : 70);
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
                                             minimumUsefulSideWidth, 480);
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
  billingService_->flushSave();
  billingService_->clear();
  detailPanel_->clear();
  refreshReceiptPanel();

  lastOutputDir_ = resolveOutputRoot();
  QDir().mkpath(lastOutputDir_);
  const QString stateDir = resolveStateDir();
  QDir().mkpath(stateDir);
  billingService_->setStateDirectory(stateDir);
  billingService_->load();

  const QString nativeOutputDir = QDir::toNativeSeparators(lastOutputDir_);
  outputDirLabel_->setText(compactPathText(nativeOutputDir));
  outputDirLabel_->setToolTip(nativeOutputDir);

  bridge_->setStallTimeout(std::chrono::seconds{config_.monitoring.stall_timeout_seconds});
  bridge_->startWatching(lastOutputDir_);
  updateButtonStates(); updatePrinterLabel(); refreshSummaryCards();
  statusBar()->showMessage(QStringLiteral("监听中 -> %1").arg(QDir::toNativeSeparators(lastOutputDir_)));
}

void MainWindow::onStopClicked()
{
  bridge_->stopWatching();
  billingService_->flushSave();
  updateButtonStates(); updatePrinterLabel();
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
    billingService_->setPricingConfig(&config_.pricing);
    billingService_->setPrinterProfiles(&config_.printers);
    applyConfig();
    statusBar()->showMessage(QStringLiteral("设置已保存"), 3000);
  }
}

void MainWindow::onCompleteClicked()
{
  QModelIndex current = treeView_->currentIndex();
  if (!current.isValid()) { statusBar()->showMessage(QStringLiteral("请先选择一个批次"), 3000); return; }
  while (current.isValid() && !batchModel_->isBatchRow(current))
    current = current.parent();
  if (!current.isValid()) { statusBar()->showMessage(QStringLiteral("请选择一个未结算的批次"), 3000); return; }

  const QString batchId = batchModel_->batchIdAt(current);
  const auto* batch = batchModel_->batchAt(current);
  if (batch == nullptr || batch->is_settled()) { statusBar()->showMessage(QStringLiteral("请选择一个未结算的批次"), 3000); return; }

  std::vector<JobCostLine> costLines;
  for (int r = 0; r < batchModel_->rowCount(current); ++r)
  {
    QModelIndex childIdx = batchModel_->index(r, 0, current);
    const auto* job = batchModel_->jobAt(childIdx);
    if (job == nullptr) continue;
    JobCostLine line;
    line.documentName = QString::fromStdString(job->snapshot.document_name);
    line.pages = job->pricing.total_pages;
    if (job->metadata.color.state == core::FieldState::Reported)
      line.colorMode = QString::fromStdString(job->metadata.color.value);
    if (job->metadata.duplex.state == core::FieldState::Reported)
      line.duplexMode = QString::fromStdString(job->metadata.duplex.value);
    if (job->metadata.paper_size.state == core::FieldState::Reported)
      line.paperSize = QString::fromStdString(job->metadata.paper_size.value);
    if (job->metadata.file_extension.state == core::FieldState::Reported)
      line.fileExtension = QString::fromStdString(job->metadata.file_extension.value);
    line.pricePending = job->pricing.price_pending;
    line.pendingReason = QString::fromStdString(job->pricing.pending_reason);
    line.price = job->pricing.total_price;
    costLines.push_back(std::move(line));
  }

  if (costLines.empty()) { statusBar()->showMessage(QStringLiteral("该批次没有作业"), 3000); return; }

  CompletionDialog dlg(QString::fromStdString(batch->display_name),
                       costLines, config_.pricing, this);
  if (dlg.exec() != QDialog::Accepted) return;

  QString orderId = billingService_->settle(batchId);
  if (orderId.isEmpty()) { statusBar()->showMessage(QStringLiteral("结算失败"), 3000); return; }
  statusBar()->showMessage(
    QStringLiteral("已结算: %1 (订单 %2)")
      .arg(QString::fromStdString(batch->display_name)).arg(orderId), 5000);
}

void MainWindow::onTreeSelectionChanged()
{
  refreshSelectedDetail();
  QModelIndex current = treeView_->currentIndex();
  while (current.isValid() && !batchModel_->isBatchRow(current))
    current = current.parent();
  bool canSettle = current.isValid() && batchModel_->batchAt(current) != nullptr &&
                   !batchModel_->batchAt(current)->is_settled();
  completeUserBtn_->setEnabled(canSettle);
  quickSettleBtn_->setEnabled(canSettle || batchModel_->batchCount() > 0);
  scheduleWorkspaceRebalance();
}

void MainWindow::onPrinterSkipped(const QString& printerName)
{
  statusBar()->showMessage(QStringLiteral("已跳过打印机: %1").arg(printerName), 3000);
}

void MainWindow::onWatcherHeartbeat(const QString& printerName)
{
  printerHeartbeats_[printerName] = QDateTime::currentDateTimeUtc();
}

void MainWindow::onBatchSettled(const QString&, const QString&)
{
  refreshSummaryCards(); refreshReceiptPanel();
  selectNextRelevantBatch(); onTreeSelectionChanged();
}

void MainWindow::checkStalePrinters()
{
  if (!bridge_->isRunning()) return;
  const auto now = QDateTime::currentDateTimeUtc();
  QStringList stale;
  for (const QString& name : bridge_->watchedPrinters())
  {
    auto it = printerHeartbeats_.find(name);
    if (it == printerHeartbeats_.end()) { printerHeartbeats_[name] = now; continue; }
    if (it->secsTo(now) > 8) stale.append(name);
  }
  if (!stale.isEmpty())
    statusBar()->showMessage(QStringLiteral("打印机无响应: %1").arg(stale.join(QStringLiteral(", "))), 5000);
}

void MainWindow::autoStart() { onStartClicked(); }

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
  sessionStateLabel_->setText(running ? QStringLiteral("运行中") : QStringLiteral("待启动"));
  sessionStateLabel_->setProperty("state", running ? "running" : "idle");
  repolish(sessionStateLabel_);
  const int n = running ? bridge_->watchedPrinters().size() : 0;
  printerCountLabel_->setText(QString::number(n));
}

void MainWindow::refreshSummaryCards()
{
  const int totalBatches = batchModel_->batchCount();
  int activeBatches = billingService_->activeBatchCount();
  int totalJobs = billingService_->todayJobCount();
  userCountLabel_->setText(QString::number(activeBatches));
  resultCountLabel_->setText(QStringLiteral("%1 批次 / %2 作业").arg(totalBatches).arg(totalJobs));
  const bool filtersActive = !searchEdit_->text().trimmed().isEmpty();
  clearFilterBtn_->setEnabled(filtersActive);
  onTreeSelectionChanged();
}

void MainWindow::applySearchFilter()
{
  if (treeView_ == nullptr || batchModel_ == nullptr) return;
  const QString norm = searchEdit_->text().trimmed();
  treeView_->setUpdatesEnabled(false);
  for (int i = 0; i < batchModel_->rowCount(); ++i)
  {
    QModelIndex idx = batchModel_->index(i, 0);
    const QString displayName = idx.data(BillingBatchModel::DisplayNameRole).toString();
    const bool match = norm.isEmpty() || displayName.contains(norm, Qt::CaseInsensitive);
    treeView_->setRowHidden(i, QModelIndex(), !match);
    if (match && !norm.isEmpty()) treeView_->expand(idx);
    else if (norm.isEmpty()) treeView_->collapse(idx);
  }
  treeView_->setUpdatesEnabled(true);
  clearFilterBtn_->setEnabled(!norm.isEmpty());
  refreshSummaryCards();
}

void MainWindow::refreshEmptyState()
{
  refreshSummaryCards();
  const bool hasVisible = batchModel_->batchCount() > 0;
  tableStack_->setCurrentWidget(hasVisible ? static_cast<QWidget*>(treeView_) : static_cast<QWidget*>(emptyStateLabel_));
  if (hasVisible) return;
  emptyStateLabel_->setText(bridge_->isRunning() ? QStringLiteral("暂无打印作业") : QStringLiteral("点击「开始监听」启动"));
}

void MainWindow::refreshGroupRowSpans()
{
  if (treeView_ == nullptr || batchModel_ == nullptr) return;
  for (int i = 0; i < batchModel_->batchCount(); ++i)
    treeView_->setFirstColumnSpanned(i, QModelIndex(), true);
}

void MainWindow::refreshSelectedDetail()
{
  QModelIndex current = treeView_->currentIndex();
  if (!current.isValid()) { detailPanel_->clear(); return; }

  if (batchModel_->isBatchRow(current))
  {
    const auto* batch = batchModel_->batchAt(current);
    if (batch == nullptr) { detailPanel_->clear(); return; }
    UserGroupSummary summary;
    summary.jobCount = static_cast<int>(batch->job_ids.size());
    summary.simplexPages = batch->simplex_pages;
    summary.duplexPages = batch->duplex_pages;
    summary.duplexSheets = (batch->duplex_pages + 1) / 2;
    summary.totalPrice = batch->total_price;
    summary.pendingCount = batch->pending_count;
    if (!batch->first_doc_anchor.document_name.empty())
      summary.specialItems.append(QStringLiteral("首: %1").arg(QString::fromStdString(batch->first_doc_anchor.document_name)));
    if (!batch->last_doc_anchor.document_name.empty())
      summary.specialItems.append(QStringLiteral("尾: %1").arg(QString::fromStdString(batch->last_doc_anchor.document_name)));
    int activeJobs = 0;
    for (int r = 0; r < batchModel_->rowCount(current); ++r)
    {
      QModelIndex childIdx = batchModel_->index(r, 0, current);
      const auto* job = batchModel_->jobAt(childIdx);
      if (job != nullptr && !is_terminal_phase(job->phase)) ++activeJobs;
    }
    detailPanel_->showGroupSummary(QString::fromStdString(batch->display_name), summary, activeJobs, batch->is_settled());
  }
  else
  {
    const auto* job = batchModel_->jobAt(current);
    if (job != nullptr) detailPanel_->showJob(job->snapshot, job->metadata, job->pricing);
  }
}

void MainWindow::selectNextRelevantBatch()
{
  if (batchModel_->batchCount() == 0) return;
  QModelIndex current = treeView_->currentIndex();
  while (current.isValid() && !batchModel_->isBatchRow(current))
    current = current.parent();
  int fromRow = current.isValid() ? current.row() : -1;
  int next = batchModel_->nextRelevantRow(fromRow);
  if (next < 0) return;
  QModelIndex idx = batchModel_->index(next, 0);
  if (!treeView_->isRowHidden(next, QModelIndex()))
  {
    treeView_->selectionModel()->setCurrentIndex(idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    treeView_->expand(idx);
    treeView_->scrollTo(idx, QAbstractItemView::PositionAtCenter);
  }
}

void MainWindow::refreshReceiptPanel()
{
  if (receiptPanel_ == nullptr || batchModel_ == nullptr || billingService_ == nullptr) return;
  receiptPanel_->clearCards();
  const auto& batches = billingService_->batches();

  for (const auto& batch : batches)
  {
    if (batch.is_settled()) continue;
    QString firstDoc = QString::fromStdString(batch.first_doc_anchor.document_name);
    QString lastDoc = QString::fromStdString(batch.last_doc_anchor.document_name);
    int totalPages = batch.simplex_pages + batch.duplex_pages;
    QColor userColor = userBackgroundColor(QString::fromStdString(batch.batch_id));
    bool hasPending = batch.pending_count > 0;
    receiptPanel_->addCard(QString::fromStdString(batch.batch_id),
      QString::fromStdString(batch.display_name), firstDoc, lastDoc,
      totalPages, batch.total_price, userColor, false, hasPending);
  }
  for (auto it = batches.rbegin(); it != batches.rend(); ++it)
  {
    if (!it->is_settled()) continue;
    QString firstDoc = QString::fromStdString(it->first_doc_anchor.document_name);
    QString lastDoc = QString::fromStdString(it->last_doc_anchor.document_name);
    int totalPages = it->simplex_pages + it->duplex_pages;
    QColor userColor = userBackgroundColor(QString::fromStdString(it->batch_id));
    receiptPanel_->addCard(QString::fromStdString(it->batch_id),
      QString::fromStdString(it->display_name), firstDoc, lastDoc,
      totalPages, it->total_price, userColor, true, false);
  }
}

void MainWindow::loadOrCreateConfig()
{
  const auto cfgPath = common::AppConfig::default_config_path();
  if (std::filesystem::exists(cfgPath))
  {
    config_ = common::AppConfig::defaults();
    if (!config_.load_from_json(cfgPath)) { config_ = common::AppConfig::defaults(); config_.save_to_json(cfgPath); }
  }
  else { config_ = common::AppConfig::defaults(); common::AppConfig::ensure_config_file(cfgPath); }
}

void MainWindow::applyConfig()
{
  billingService_->setPricingConfig(&config_.pricing);
  billingService_->setPrinterProfiles(&config_.printers);
  const QString outputRoot = QString::fromStdString(config_.storage.output_root);
  outputDirLabel_->setText(compactPathText(outputRoot));
  outputDirLabel_->setToolTip(outputRoot);
}

void MainWindow::detectPrintersIfNeeded()
{
  if (!config_.printers.profiles.empty()) return;
  config_.printers = common::detect_printer_capabilities();
  if (!config_.printers.profiles.empty())
    config_.save_to_json(common::AppConfig::default_config_path());
}

QString MainWindow::resolveOutputRoot() const
{
  QString root = QString::fromStdString(config_.storage.output_root);
  if (QDir::isRelativePath(root))
  { const QString exeDir = QCoreApplication::applicationDirPath(); root = QDir(exeDir).filePath(root); }
  if (config_.storage.use_timestamp_subdir)
  { const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")); root = QDir(root).filePath(QStringLiteral("capture-%1").arg(ts)); }
  return QDir::toNativeSeparators(root);
}

QString MainWindow::resolveStateDir() const
{
  QString root = QString::fromStdString(config_.storage.output_root);
  if (QDir::isRelativePath(root))
  { const QString exeDir = QCoreApplication::applicationDirPath(); root = QDir(exeDir).filePath(root); }
  return QDir::toNativeSeparators(QDir(root).filePath(QStringLiteral("_state")));
}

}  // namespace wpce::ui
