#include "ui/main_window.h"
#include "ui/filter_install_dialog.h"
#include "management/filter_install_service.h"

#include "bridge/settlement_bridge.h"
#include "common/app_config.h"
#include "common/business_clock.h"
#include "common/pricing_engine.h"
#include "domain/billing_batch_repository.h"
#include "domain/billing_migration.h"
#include "storage/capture_retention.h"
#include "ui/completion_dialog.h"
#include "ui/history_explorer_dialog.h"
#include "ui/job_detail_panel.h"
#include "ui/grip_splitter.h"
#include "ui/pricing_config.h"
#include "ui/quick_settle_dialog.h"
#include "ui/receipt_preview_panel.h"
#include "ui/settings_dialog.h"
#include "ui/user_color_palette.h"
#include "ui/billing_view_data.h"
#include "ui/watcher_bridge.h"

#include <QAbstractItemView>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QResizeEvent>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QSizePolicy>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_map>

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

std::filesystem::path backupUnreadableConfig(const std::filesystem::path& cfgPath)
{
  if (!std::filesystem::exists(cfgPath))
    return {};

  const std::string stamp =
    QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")).toStdString();
  const std::filesystem::path backup =
    cfgPath.string() + ".invalid-" + stamp + ".bak";

  std::error_code ec;
  std::filesystem::copy_file(cfgPath, backup,
                             std::filesystem::copy_options::overwrite_existing,
                             ec);
  return ec ? std::filesystem::path{} : backup;
}

}  // namespace

}  // namespace wpce::ui

// ── Includes for MainWindow implementation ───────────────────────────

#include "billing/billing_service.h"
#include <QFileDialog>
#include <QScopedValueRollback>
#include <QEvent>
#include "bridge/settlement_bridge.h"
#include "common/app_config.h"
#include "common/printer_profile.h"
#include "ui/billing_batch_model.h"
#include "ui/completion_dialog.h"
#include "ui/job_detail_panel.h"
#include "ui/quick_settle_dialog.h"
#include "ui/receipt_preview_panel.h"
#include "ui/settings_dialog.h"
#include "ui/user_color_palette.h"
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
#include <unordered_map>

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
  // Printer detection deferred to background after UI is interactive.

  setWindowTitle(QStringLiteral("WPCE 打印捕获控制台"));
  resize(config_.window.width, config_.window.height);
  setMinimumSize(1040, 640);

  bridge_ = new WatcherBridge(this);
  billingService_ = new BillingService(this);
  billingService_->setPricingConfig(&config_.pricing);
  billingService_->setPrinterProfiles(&config_.printers);
  batchModel_ = new BillingBatchModel(billingService_, this);

  // Restore persisted state before building UI so the tree has data on first paint.
  {
    // Create daily root directory first.
    const QString dailyRoot = resolveOutputRoot();
    QDir().mkpath(dailyRoot);
    cleanupExpiredCapturedData();

    const QString stateDir = resolveStateDir();
    QDir().mkpath(stateDir);
    migrateLegacyDailyStateIfNeeded();
    billingService_->setStateDirectory(stateDir);
    billingService_->setOrderBucketPath(resolveOrderBucketPath());
    QDir().mkpath(resolveHistoryRoot());
    // setOrderBucketPath has already loaded and validated this bucket.

    // Ensure per-user directories exist for any loaded batches.
    for (const auto& batch : billingService_->batches())
      ensureUserDirectories(QString::fromStdString(batch.display_name));
  }

  auto* central = new QWidget(this);
  central->setObjectName(QStringLiteral("AppRoot"));
  central->setMinimumWidth(0);
  central->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
  auto* root = new QVBoxLayout(central);
  root->setContentsMargins(24, 20, 24, 18);
  root->setSpacing(16);
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
      historyDialog_ = new HistoryExplorerDialog(resolveHistoryRoot(), this);
    historyDialog_->loadOrders();
    historyDialog_->show();
    historyDialog_->raise();
  });

  heroLayout->addWidget(startBtn_);
  heroLayout->addWidget(stopBtn_);
  heroLayout->addWidget(openOutputBtn_);
  heroLayout->addWidget(historyBtn);
  heroLayout->addWidget(settingsBtn_);
  auto* xpsSettings=new QPushButton(QStringLiteral("XPS 设置"));
  heroLayout->addWidget(xpsSettings);
  connect(xpsSettings,&QPushButton::clicked,this,[this]{
    FilterInstallDialog dialog(resolveStorageRoot(),this);
    dialog.exec();
  });
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
  searchEdit_->setPlaceholderText(QStringLiteral("搜索文档"));
  searchEdit_->setClearButtonEnabled(true);
  searchEdit_->setMinimumWidth(180);
  searchEdit_->setMaximumWidth(280);

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
  treeView_->setUniformRowHeights(true);
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

  treeView_->setColumnWidth(BillingBatchModel::ColName,       250);
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
  connect(quickSettleBtn_, &QPushButton::clicked, this, &MainWindow::showQuickSettleDialog);
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
  bridge_->setPrintTicketAcknowledgement([this](std::uint32_t id) {
    billingService_->flushSave();
    return billingService_->storageReady() && !billingService_->hasPendingSave() &&
        billingService_->deferredEventCount() == 0 && billingService_->jobRecord(id) != nullptr;
  });
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
    if (quickSettleDialog_ != nullptr) {
      quickSettleDialog_->refresh();
      quickSettleDialog_->ensurePriorityView();
    }
  });

  // Debounce batchUpdated: rapid metadata/lifecycle events for the same
  // batch should not each trigger a full UI repaint (receipt panel rebuild,
  // detail panel update, etc.).  Coalesce within 80 ms.
  batchUpdateDebounce_ = new QTimer(this);
  batchUpdateDebounce_->setSingleShot(true);
  batchUpdateDebounce_->setInterval(80);
  connect(batchUpdateDebounce_, &QTimer::timeout, this, [this]() {
    refreshSummaryCards(); refreshReceiptPanel(); refreshSelectedDetail();
    if (quickSettleDialog_ != nullptr) quickSettleDialog_->refresh();
  });
  connect(billingService_, &BillingService::batchUpdated, this, [this](const QString&) {
    if (!batchUpdateDebounce_->isActive())
      batchUpdateDebounce_->start();
  });
  connect(billingService_, &BillingService::orderSettled, this, &MainWindow::onBatchSettled);

  // Create per-user data subdirectories when a new batch is created.
  connect(billingService_, &BillingService::storageError, this, [this](const QString& message) {
    statusBar()->showMessage(message, 0);
  });
  connect(billingService_, &BillingService::businessDayChanged, this, [this](const QString&) {
    lastOutputDir_ = resolveOutputRoot();
    outputDirLabel_->setText(compactPathText(lastOutputDir_));
    outputDirLabel_->setToolTip(lastOutputDir_);
    detailPanel_->clear();
    refreshSummaryCards(); refreshReceiptPanel(); updateButtonStates();
    statusBar()->showMessage(QStringLiteral("已切换到新的业务日期，昨日订单已保留。"), 10000);
  }, Qt::QueuedConnection);

  connect(billingService_, &BillingService::batchCreated, this, [this](const QString& batchId) {
    const auto* batch = billingService_->batchById(batchId.toStdString());
    if (batch != nullptr)
      ensureUserDirectories(QString::fromStdString(batch->display_name));
    if (quickSettleDialog_ != nullptr)
      quickSettleDialog_->onBatchCreated(batchId);
  });

  connect(receiptPanel_, &ReceiptPreviewPanel::settleRequested,
          this, [this](const QString& batchId) { billingService_->settle(batchId); });
  connect(receiptPanel_, &ReceiptPreviewPanel::cardClicked,
          this, [this](const QString& batchId) {
    QModelIndex idx = batchModel_->batchIndex(batchId);
    if (idx.isValid())
    {
      treeView_->selectionModel()->setCurrentIndex(idx,
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
      treeView_->expand(idx);
      treeView_->scrollTo(idx, QAbstractItemView::PositionAtCenter);
    }
  });

  // ── Heartbeat timer ───────────────────────────────────────────────
  heartbeatCheckTimer_ = new QTimer(this);
  heartbeatCheckTimer_->setInterval(3500);
  connect(heartbeatCheckTimer_, &QTimer::timeout, this, &MainWindow::checkStalePrinters);
  heartbeatCheckTimer_->start();

  updateButtonStates(); updatePrinterLabel();
  refreshSummaryCards(); refreshEmptyState(); refreshGroupRowSpans();
  selectLastBatch();

  auto* filterProbe=new management::FilterInstallService(this);
  connect(filterProbe,&management::FilterInstallService::completed,this,[this](const auto& result){
    statusBar()->showMessage(management::FilterInstallService::summary(result),10000);
  });
  const auto appDirectory=QDir(QCoreApplication::applicationDirPath());
  filterProbe->start(management::FilterInstallService::Action::Probe,
      appDirectory.filePath("scripts/install-xps-filter.ps1"),appDirectory.filePath("wpce_xps_filter.dll"),
      resolveStorageRoot());

  QTimer::singleShot(100, this, &MainWindow::autoStart);
  if (config_.startup.open_quick_settle_at_start || config_.startup.silent_startup)
  {
    QTimer::singleShot(250, this, &MainWindow::showQuickSettleDialog);
  }
  QTimer::singleShot(800, this, &MainWindow::detectPrintersAsync);
}

MainWindow::~MainWindow()
{
  config_.window.width = width();
  config_.window.height = height();
  common::AppConfig::ensure_config_file(common::AppConfig::default_config_path());
  config_.save_to_json(common::AppConfig::default_config_path());
  (void)bridge_->stopWatching();
  billingService_->flushSave();

  if (quickSettleDialog_ != nullptr)
  {
    quickSettleDialog_->close();
    delete quickSettleDialog_;
    quickSettleDialog_ = nullptr;
  }
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
    if (treeWidth > 0)
    {
      const bool compactTree = treeWidth < 760;
      // Only force column widths when the tree is very narrow and columns
      // look broken; otherwise respect user or header-auto sizing.
      if (compactTree)
      {
        treeView_->setColumnWidth(BillingBatchModel::ColName,       160);
        treeView_->setColumnWidth(BillingBatchModel::ColState,      56);
        treeView_->setColumnWidth(BillingBatchModel::ColProgress,   90);
        treeView_->setColumnWidth(BillingBatchModel::ColPages,      40);
        treeView_->setColumnWidth(BillingBatchModel::ColDuplex,     56);
        treeView_->setColumnWidth(BillingBatchModel::ColPaper,      58);
        treeView_->setColumnWidth(BillingBatchModel::ColSubmitTime, 110);
        treeView_->setColumnWidth(BillingBatchModel::ColPrice,      56);
      }
      else
      {
        treeView_->setColumnWidth(BillingBatchModel::ColName,       250);
        treeView_->setColumnWidth(BillingBatchModel::ColState,      62);
        treeView_->setColumnWidth(BillingBatchModel::ColProgress,   110);
        treeView_->setColumnWidth(BillingBatchModel::ColPages,      48);
        treeView_->setColumnWidth(BillingBatchModel::ColDuplex,     70);
        treeView_->setColumnWidth(BillingBatchModel::ColPaper,      70);
        treeView_->setColumnWidth(BillingBatchModel::ColSubmitTime, 130);
        treeView_->setColumnWidth(BillingBatchModel::ColPrice,      70);
      }
    }
  }

  const int workspaceWidth = workspaceSplitter_->width();
  if (workspaceWidth > 0)
  {
    const QList<int> sizes = workspaceSplitter_->sizes();
    const int currentSideWidth = sizes.size() > 1 ? sizes.at(1) : 0;
    const int minimumUsefulSideWidth = std::min(400, std::max(340, workspaceWidth / 3));
    // Only snap the splitter back when the side panel is unreasonably
    // narrow (below 200 px) or below the useful threshold with a tolerance.
    if (currentSideWidth < minimumUsefulSideWidth && currentSideWidth < 250)
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
    if (sizes.size() > 1 && sizes.at(0) < minimumWorkspaceHeight && sizes.at(0) < 250)
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
  if (closing_) return;
  if (bridge_->isRunning()) return;
  if (!bridge_->stopWatching(0))
  {
    statusBar()->showMessage(QStringLiteral("上次监听线程尚未停止，请稍后重试。"), 15000);
    return;
  }
  QCoreApplication::sendPostedEvents(billingService_, QEvent::MetaCall);
  const auto saved = billingService_->prepareShutdown();
  if (!saved.ready)
  {
    statusBar()->showMessage(saved.message, 15000);
    return;
  }
  detailPanel_->clear();
  refreshReceiptPanel();

  lastOutputDir_ = resolveOutputRoot();
  QDir().mkpath(lastOutputDir_);
  cleanupExpiredCapturedData();

  // Also ensure per-user dirs for any loaded batches are re-created.
  for (const auto& batch : billingService_->batches())
    ensureUserDirectories(QString::fromStdString(batch.display_name));

  // Always reload state — load() is idempotent (reads same file).
  {
    const QString stateDir = resolveStateDir();
    QDir().mkpath(stateDir);
    migrateLegacyDailyStateIfNeeded();
    billingService_->setStateDirectory(stateDir);
    billingService_->setOrderBucketPath(resolveOrderBucketPath());

    QDir().mkpath(resolveHistoryRoot());

    // setOrderBucketPath has already loaded and validated this bucket.
  }

  if (!billingService_->storageReady())
  {
    statusBar()->showMessage(billingService_->storageErrorMessage(), 15000);
    return;
  }

  const QString nativeOutputDir = QDir::toNativeSeparators(lastOutputDir_);
  outputDirLabel_->setText(compactPathText(nativeOutputDir));
  outputDirLabel_->setToolTip(nativeOutputDir);

  bridge_->setStallTimeout(std::chrono::seconds{config_.monitoring.stall_timeout_seconds});
  bridge_->setRescanInterval(std::chrono::seconds{config_.monitoring.rescan_interval_seconds});
  bridge_->setPollTimeout(std::chrono::milliseconds{config_.monitoring.poll_timeout_ms});
  const std::uint32_t maxStoredJobId = billingService_->maxStoredJobId();
  std::vector<core::JobSnapshot> storedJobs;
  for (const auto& batch : billingService_->batches())
    for (const auto id : batch.job_ids)
      if (const auto* record = billingService_->jobRecord(id)) storedJobs.push_back(record->snapshot);
  bridge_->seedStoredJobs(std::move(storedJobs));
  bridge_->seedMinSyntheticJobId(
      maxStoredJobId == std::numeric_limits<std::uint32_t>::max()
          ? maxStoredJobId
          : maxStoredJobId + 1);
  bridge_->startWatching(lastOutputDir_);
  updateButtonStates(); updatePrinterLabel(); refreshSummaryCards();
  refreshEmptyState(); refreshGroupRowSpans(); selectLastBatch();
  statusBar()->showMessage(QStringLiteral("监听中 -> %1").arg(QDir::toNativeSeparators(lastOutputDir_)));
}

void MainWindow::onStopClicked()
{
  if (!bridge_->stopWatching())
  {
    statusBar()->showMessage(QStringLiteral("监听线程尚未停止，已保留线程和数据，请稍后重试。"), 0);
    return;
  }
  QCoreApplication::sendPostedEvents(billingService_, QEvent::MetaCall);
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
  config_.window.settings_width = dialog.width();
  config_.window.settings_height = dialog.height();
}

void MainWindow::onCompleteClicked()
{
  QModelIndex current = treeView_->currentIndex();
  if (!current.isValid()) { statusBar()->showMessage(QStringLiteral("请先选择一个批次"), 3000); return; }
  while (current.isValid() && !batchModel_->isBatchRow(current))
    current = current.parent();
  if (!current.isValid()) { statusBar()->showMessage(QStringLiteral("请选择一个未结算的批次"), 3000); return; }

  const QString batchId = batchModel_->batchIdAt(current);
  (void)billingService_->ensureBatchJobsLoaded(batchId);
  const auto* batch = batchModel_->batchAt(current);
  if (batch == nullptr || batch->is_settled()) { statusBar()->showMessage(QStringLiteral("请选择一个未结算的批次"), 3000); return; }

  std::vector<JobCostLine> costLines;
  for (int r = 0; r < batchModel_->rowCount(current); ++r)
  {
    QModelIndex childIdx = batchModel_->index(r, 0, current);
    const auto* job = batchModel_->jobAt(childIdx);
    if (job == nullptr) continue;
    JobCostLine line;
    line.jobId = job->snapshot.job_id;
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

  const auto finalPrices = dlg.finalPrices();
  for (std::size_t i = 0; i < costLines.size() && i < finalPrices.size(); ++i)
  {
    if (!costLines[i].pricePending) continue;
    if (!billingService_->setManualPrice(costLines[i].jobId, finalPrices[i]))
    {
      statusBar()->showMessage(QStringLiteral("计费调整写入失败，未执行结算"), 5000);
      return;
    }
  }

  QString orderId = billingService_->settle(batchId);
  if (orderId.isEmpty()) { statusBar()->showMessage(QStringLiteral("结算失败"), 3000); return; }
  statusBar()->showMessage(
    QStringLiteral("已结算: %1 (订单 %2)")
      .arg(QString::fromStdString(batch->display_name)).arg(orderId), 5000);
}

void MainWindow::onTreeSelectionChanged()
{
  QModelIndex current = treeView_->currentIndex();
  while (current.isValid() && !batchModel_->isBatchRow(current))
    current = current.parent();
  if (current.isValid())
    (void)billingService_->ensureBatchJobsLoaded(batchModel_->batchIdAt(current));

  refreshSelectedDetail();
  bool canSettle = current.isValid() && batchModel_->batchAt(current) != nullptr &&
                   !batchModel_->batchAt(current)->is_settled();
  completeUserBtn_->setEnabled(canSettle);
  quickSettleBtn_->setEnabled(true);
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

bool MainWindow::isSilentStartup() const
{
  return config_.startup.silent_startup;
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

  // Update settle button state based on current selection.
  QModelIndex current = treeView_->currentIndex();
  while (current.isValid() && !batchModel_->isBatchRow(current))
    current = current.parent();
  bool canSettle = current.isValid() && batchModel_->batchAt(current) != nullptr &&
                   !batchModel_->batchAt(current)->is_settled();
  completeUserBtn_->setEnabled(canSettle);
  quickSettleBtn_->setEnabled(true);
}

void MainWindow::applySearchFilter()
{
  if (treeView_ == nullptr || batchModel_ == nullptr) return;
  const QString norm = searchEdit_->text().trimmed();
  treeView_->setUpdatesEnabled(false);
  for (int i = 0; i < batchModel_->rowCount(); ++i)
  {
    QModelIndex idx = batchModel_->index(i, 0);
    const QString batchId = idx.data(BillingBatchModel::BatchIdRole).toString();
    const bool match = norm.isEmpty() || billingService_->batchMatchesSearch(batchId, norm);
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

void MainWindow::selectLastBatch()
{
  if (treeView_ == nullptr || batchModel_ == nullptr) return;
  for (int row = batchModel_->batchCount() - 1; row >= 0; --row)
  {
    if (treeView_->isRowHidden(row, QModelIndex())) continue;
    QModelIndex idx = batchModel_->index(row, 0);
    if (!idx.isValid()) continue;
    treeView_->selectionModel()->setCurrentIndex(
      idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    treeView_->expand(idx);
    treeView_->scrollTo(idx, QAbstractItemView::PositionAtBottom);
    return;
  }
}

void MainWindow::showQuickSettleDialog()
{
  if (quickSettleDialog_ == nullptr)
  {
    quickSettleDialog_ = new QuickSettleDialog(batchModel_, billingService_, &config_.pricing,
                                                config_.window.quick_settle_width,
                                                config_.window.quick_settle_height,
                                                config_.window.quick_settle_collapsed_height,
                                                config_.window.quick_settle_collapsed,
                                                nullptr);
    connect(quickSettleDialog_, &QuickSettleDialog::geometryChanged,
            this, [this](int w, int h, int ch, bool collapsed) {
      config_.window.quick_settle_width = w;
      config_.window.quick_settle_height = h;
      config_.window.quick_settle_collapsed_height = ch;
      config_.window.quick_settle_collapsed = collapsed;
    });
    connect(quickSettleDialog_, &QuickSettleDialog::settleRequested,
            this, [this](const QString& batchId) { billingService_->settle(batchId); });
    connect(quickSettleDialog_, &QuickSettleDialog::hangUpRequested,
            this, [this](const QString& batchId) { billingService_->suspend(batchId); });
    connect(quickSettleDialog_, &QuickSettleDialog::resumeRequested,
            this, [this](const QString& batchId) { billingService_->resume(batchId); });
    connect(quickSettleDialog_, &QuickSettleDialog::batchFocusChanged,
            this, [this](const QString& batchId) {
      QModelIndex idx = batchModel_->batchIndex(batchId);
      if (idx.isValid())
      {
        treeView_->selectionModel()->setCurrentIndex(idx,
          QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        treeView_->expand(idx);
        treeView_->scrollTo(idx, QAbstractItemView::PositionAtCenter);
      }
    });
  }

  int targetIndex = -1;
  QModelIndex current = treeView_->currentIndex();
  while (current.isValid() && !batchModel_->isBatchRow(current))
    current = current.parent();
  if (current.isValid())
    targetIndex = current.row();
  if (targetIndex < 0)
    targetIndex = batchModel_->nextRelevantRow(-1);
  if (targetIndex >= 0)
    quickSettleDialog_->setCurrentBatchIndex(targetIndex);

  quickSettleDialog_->show();
  quickSettleDialog_->raise();
  quickSettleDialog_->activateWindow();
}

void MainWindow::refreshReceiptPanel()
{
  if (receiptPanel_ == nullptr || batchModel_ == nullptr || billingService_ == nullptr) return;
  receiptPanel_->clearCards();
  const auto& batches = billingService_->batches();

  for (const auto& batch : batches)
  {
    if (batch.is_settled()) continue;
    QString batchId = QString::fromStdString(batch.batch_id);
    QString displayName = QString::fromStdString(batch.display_name);
    QString firstDoc = QString::fromStdString(batch.first_doc_anchor.document_name);
    QString lastDoc = QString::fromStdString(batch.last_doc_anchor.document_name);
    int totalPages = batch.simplex_pages + batch.duplex_pages;
    QColor userColor = userBackgroundColor(batchId);
    bool hasPending = batch.pending_count > 0;
    bool suspended = batch.is_suspended();
    receiptPanel_->addCard(batchId, displayName, firstDoc, lastDoc,
      totalPages, batch.total_price, userColor, false, hasPending, suspended);
  }
  for (auto it = batches.rbegin(); it != batches.rend(); ++it)
  {
    if (!it->is_settled()) continue;
    QString batchId = QString::fromStdString(it->batch_id);
    QString displayName = QString::fromStdString(it->display_name);
    QString firstDoc = QString::fromStdString(it->first_doc_anchor.document_name);
    QString lastDoc = QString::fromStdString(it->last_doc_anchor.document_name);
    int totalPages = it->simplex_pages + it->duplex_pages;
    QColor userColor = userBackgroundColor(batchId);
    receiptPanel_->addCard(batchId, displayName, firstDoc, lastDoc,
      totalPages, it->total_price, userColor, true, false, false);
  }
}

void MainWindow::loadOrCreateConfig()
{
  const auto cfgPath = common::AppConfig::default_config_path();
  if (std::filesystem::exists(cfgPath))
  {
    config_ = common::AppConfig::defaults();
    if (!config_.load_from_json(cfgPath))
    {
      const auto backup = backupUnreadableConfig(cfgPath);
      config_ = common::AppConfig::defaults();
      if (!backup.empty())
      {
        fprintf(stderr,
                "[WPCE] Config load failed; preserved original as %s and using in-memory defaults\n",
                backup.string().c_str());
      }
      else
      {
        fprintf(stderr,
                "[WPCE] Config load failed; using in-memory defaults without overwriting the file\n");
      }
    }
  }
  else { config_ = common::AppConfig::defaults(); common::AppConfig::ensure_config_file(cfgPath); }
}

void MainWindow::applyConfig()
{
  billingService_->setPricingConfig(&config_.pricing);
  billingService_->setPrinterProfiles(&config_.printers);
  const QString outputRoot = resolveStorageRoot();
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

void MainWindow::detectPrintersAsync()
{
  auto* thread = QThread::create([this]() {
    auto detected = common::detect_printer_capabilities();
    QMetaObject::invokeMethod(this, [this, detected = std::move(detected)]() mutable {
      if (detected.profiles.empty()) return;

      // Merge: preserve user-configured color capability for known printers.
      std::unordered_map<std::string, bool> existing;
      for (const auto& p : config_.printers.profiles)
        existing[p.printer_name] = p.is_color_capable;

      config_.printers = std::move(detected);
      for (auto& p : config_.printers.profiles)
      {
        auto it = existing.find(p.printer_name);
        if (it != existing.end())
          p.is_color_capable = it->second;
      }

      billingService_->setPrinterProfiles(&config_.printers);
      config_.save_to_json(common::AppConfig::default_config_path());
    }, Qt::QueuedConnection);
  });
  connect(thread, &QThread::finished, thread, &QObject::deleteLater);
  thread->start();
}

QString MainWindow::resolveStorageRoot() const
{
  QString root = QString::fromStdString(config_.storage.output_root);
  if (QDir::isRelativePath(root))
  {
    const QString exeDir = QCoreApplication::applicationDirPath();
    root = QDir(exeDir).filePath(root);
  }
  return QDir::toNativeSeparators(root);
}

QString MainWindow::resolveOutputRoot() const
{
  // Captured spooler artifacts are partitioned by day.
  const QString today = QString::fromStdString(common::current_business_date().yyyy_mm_dd);
  return QDir::toNativeSeparators(QDir(resolveStorageRoot()).filePath(today));
}

QString MainWindow::resolveStateDir() const
{
  return QDir::toNativeSeparators(QDir(resolveStorageRoot()).filePath(QStringLiteral("_state")));
}

QString MainWindow::resolveOrderBucketPath() const
{
  return QDir::toNativeSeparators(
      QDir(resolveOutputRoot()).filePath(QStringLiteral("orders.json")));
}

QString MainWindow::resolveHistoryRoot() const
{
  return resolveStorageRoot();
}

QString MainWindow::resolveUserDir(const QString& displayName) const
{
  return QDir::toNativeSeparators(QDir(resolveOutputRoot()).filePath(displayName));
}

void MainWindow::ensureUserDirectories(const QString& displayName)
{
  const QString userDir = resolveUserDir(displayName);
  QDir().mkpath(userDir);
  QDir().mkpath(userDir + QStringLiteral("/jobs"));
  QDir().mkpath(userDir + QStringLiteral("/pages"));
}

void MainWindow::cleanupExpiredCapturedData()
{
  const auto result = storage::cleanup_expired_capture(
      std::filesystem::path(resolveStorageRoot().toStdWString()),
      common::current_business_date().yyyy_mm_dd, config_.storage.retention_days);
  if (!result.errors.empty())
    statusBar()->showMessage(QStringLiteral("部分过期捕获文件未能清理，订单和原文件已保留。"), 8000);
}

void MainWindow::migrateLegacyDailyStateIfNeeded()
{
  const auto result = domain::migrate_legacy_billing(
      std::filesystem::path(resolveStorageRoot().toStdWString()),
      common::current_business_date().yyyy_mm_dd);
  if (!result.ok())
    statusBar()->showMessage(QStringLiteral("旧账务数据有 %1 项迁移异常，原文件已保留，请核对后重试。")
                               .arg(result.errors.size()), 15000);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
  event->ignore();
  if (closing_) return;
  QScopedValueRollback<bool> closingGuard(closing_, true);
  auto reply = QMessageBox::question(this, QStringLiteral("确认退出"),
                                     QStringLiteral("确定要退出 WPCE 吗？"),
                                     QMessageBox::Yes | QMessageBox::No);
  if (reply != QMessageBox::Yes) return;
  if (!bridge_->stopWatching())
  {
    QMessageBox::warning(this, QStringLiteral("暂不能退出"),
        QStringLiteral("监听线程尚未停止。窗口和数据已保留，请稍后再次尝试退出。"));
    return;
  }
  QCoreApplication::sendPostedEvents(billingService_, QEvent::MetaCall);
  const auto result = billingService_->prepareShutdown();
  const auto captureErrors = bridge_->storageErrors();
  if (result.ready && captureErrors.isEmpty())
  {
    event->accept();
    return;
  }

  QMessageBox failure(QMessageBox::Warning, QStringLiteral("数据尚未安全保存"),
      result.ready ? QStringLiteral("采集文件曾写入失败，请先导出恢复数据并核对。") : result.message,
      QMessageBox::NoButton, this);
  if (!captureErrors.isEmpty()) failure.setDetailedText(captureErrors.join('\n'));
  auto* retry = failure.addButton(QStringLiteral("重试保存"), QMessageBox::AcceptRole);
  auto* exportButton = failure.addButton(QStringLiteral("导出恢复数据"), QMessageBox::ActionRole);
  failure.addButton(QStringLiteral("保留窗口"), QMessageBox::RejectRole);
  failure.exec();
  if (failure.clickedButton() == retry)
  {
    const auto retried = billingService_->prepareShutdown();
    if (retried.ready && bridge_->storageErrors().isEmpty()) event->accept();
    else statusBar()->showMessage(retried.ready ? QStringLiteral("请先导出并核对采集故障恢复数据。") : retried.message, 0);
  }
  else if (failure.clickedButton() == exportButton)
  {
    const auto suggested = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
        .filePath(QStringLiteral("WPCE-恢复数据-%1.json").arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz")));
    const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("导出恢复数据（需人工核对恢复）"),
        suggested, QStringLiteral("JSON 文件 (*.json)"));
    if (path.isEmpty()) return;
    QString error;
    if (billingService_->exportRecovery(path, &error, captureErrors))
    {
      bridge_->acknowledgeRecoveryExport();
      QMessageBox::information(this, QStringLiteral("恢复数据已导出"),
          QStringLiteral("已保存当前任务、冻结订单、暂存事件及采集故障信息。请人工核对恢复；窗口保持打开，原订单未被覆盖。"));
    }
    else QMessageBox::warning(this, QStringLiteral("导出失败"), error);
  }
}

}  // namespace wpce::ui
