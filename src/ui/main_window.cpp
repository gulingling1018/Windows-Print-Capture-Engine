#include "ui/main_window.h"
#include "ui/document_link_delegate.h"
#include "ui/job_detail_panel.h"
#include "ui/job_table_model.h"
#include "ui/lifecycle_timeline.h"
#include "ui/statistics_panel.h"
#include "ui/user_color_palette.h"
#include "ui/user_task_model.h"
#include "ui/user_task_panel.h"
#include "ui/watcher_bridge.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMetaType>
#include <QSortFilterProxyModel>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

namespace wpce::ui
{

namespace
{

class JobFilterProxyModel final : public QSortFilterProxyModel
{
 public:
  explicit JobFilterProxyModel(QObject* parent = nullptr)
      : QSortFilterProxyModel(parent)
  {
    setDynamicSortFilter(true);
    setSortRole(JobTableModel::SortRole);
  }

  void setSearchText(const QString& text)
  {
    const QString normalized = text.trimmed();
    if (searchText_ == normalized)
    {
      return;
    }
    searchText_ = normalized;
    invalidateFilter();
  }

  void setPhaseKey(const QString& key)
  {
    if (phaseKey_ == key)
    {
      return;
    }
    phaseKey_ = key;
    invalidateFilter();
  }

 protected:
  bool filterAcceptsRow(const int sourceRow,
                        const QModelIndex& sourceParent) const override
  {
    const auto* model = sourceModel();
    if (model == nullptr)
    {
      return false;
    }

    if (!phaseKey_.isEmpty())
    {
      const QModelIndex phaseIndex =
        model->index(sourceRow, JobTableModel::ColPhase, sourceParent);
      if (phaseIndex.data(JobTableModel::PhaseKeyRole).toString() != phaseKey_)
      {
        return false;
      }
    }

    if (searchText_.isEmpty())
    {
      return true;
    }

    for (int column = 0; column < model->columnCount(sourceParent); ++column)
    {
      const QModelIndex index = model->index(sourceRow, column, sourceParent);
      const QString value = index.data(Qt::DisplayRole).toString();
      if (value.contains(searchText_, Qt::CaseInsensitive))
      {
        return true;
      }
    }

    return false;
  }

  bool lessThan(const QModelIndex& left,
                const QModelIndex& right) const override
  {
    const QVariant lhs = left.data(JobTableModel::SortRole);
    const QVariant rhs = right.data(JobTableModel::SortRole);

    const auto isNumeric = [](const QVariant& value)
    {
      const int type = value.typeId();
      return type == QMetaType::Int ||
             type == QMetaType::UInt ||
             type == QMetaType::LongLong ||
             type == QMetaType::ULongLong;
    };

    if (isNumeric(lhs) && isNumeric(rhs))
    {
      return lhs.toLongLong() < rhs.toLongLong();
    }

    return QString::localeAwareCompare(lhs.toString(), rhs.toString()) < 0;
  }

 private:
  QString searchText_;
  QString phaseKey_;
};

void repolish(QWidget* widget)
{
  if (widget == nullptr)
  {
    return;
  }

  widget->style()->unpolish(widget);
  widget->style()->polish(widget);
  widget->update();
}

QFrame* makeMetricCard(const QString& caption, QLabel* value)
{
  auto* card = new QFrame();
  card->setObjectName(QStringLiteral("MetricCard"));

  auto* layout = new QVBoxLayout(card);
  layout->setContentsMargins(14, 12, 14, 12);
  layout->setSpacing(6);

  auto* captionLabel = new QLabel(caption);
  captionLabel->setObjectName(QStringLiteral("MetricLabel"));

  if (value->objectName().isEmpty())
  {
    value->setObjectName(QStringLiteral("MetricValue"));
  }
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

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
  setWindowTitle(QStringLiteral("WPCE 打印捕获控制台"));
  resize(1280, 760);
  setMinimumSize(1040, 640);

  bridge_ = new WatcherBridge(this);
  tableModel_ = new JobTableModel(this);
  userTaskModel_ = new UserTaskModel(this);
  jobProxy_ = new JobFilterProxyModel(this);
  jobProxy_->setSourceModel(tableModel_);

  auto* central = new QWidget(this);
  central->setObjectName(QStringLiteral("AppRoot"));
  auto* root = new QVBoxLayout(central);
  root->setContentsMargins(20, 18, 20, 16);
  root->setSpacing(14);
  setCentralWidget(central);

  auto* hero = new QFrame();
  hero->setObjectName(QStringLiteral("HeroBar"));
  auto* heroLayout = new QHBoxLayout(hero);
  heroLayout->setContentsMargins(18, 16, 18, 16);
  heroLayout->setSpacing(18);

  auto* titleBlock = new QVBoxLayout();
  titleBlock->setContentsMargins(0, 0, 0, 0);
  titleBlock->setSpacing(4);

  auto* title = new QLabel(QStringLiteral("WPCE 打印捕获控制台"));
  title->setObjectName(QStringLiteral("WindowTitle"));
  auto* subtitle = new QLabel(QStringLiteral("打印作业、参数与生命周期审计"));
  subtitle->setObjectName(QStringLiteral("WindowSubtitle"));
  titleBlock->addWidget(title);
  titleBlock->addWidget(subtitle);
  heroLayout->addLayout(titleBlock, 1);

  startBtn_ = new QPushButton(QStringLiteral("开始监听"));
  startBtn_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
  setButtonRole(startBtn_, "primary");

  stopBtn_ = new QPushButton(QStringLiteral("停止"));
  stopBtn_->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
  setButtonRole(stopBtn_, "danger");

  openOutputBtn_ = new QPushButton(QStringLiteral("打开目录"));
  openOutputBtn_->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
  setButtonRole(openOutputBtn_, "quiet");

  heroLayout->addWidget(startBtn_);
  heroLayout->addWidget(stopBtn_);
  heroLayout->addWidget(openOutputBtn_);
  root->addWidget(hero);

  auto* metrics = new QHBoxLayout();
  metrics->setContentsMargins(0, 0, 0, 0);
  metrics->setSpacing(12);

  sessionStateLabel_ = new QLabel(QStringLiteral("待启动"));
  sessionStateLabel_->setObjectName(QStringLiteral("StatusPill"));
  sessionStateLabel_->setProperty("state", "idle");

  printerCountLabel_ = new QLabel(QStringLiteral("0"));
  jobCountLabel_ = new QLabel(QStringLiteral("0"));

  outputDirLabel_ = new QLabel(QStringLiteral("尚未创建"));
  outputDirLabel_->setObjectName(QStringLiteral("PathValue"));
  outputDirLabel_->setWordWrap(true);

  metrics->addWidget(makeMetricCard(QStringLiteral("监听状态"), sessionStateLabel_), 0);
  metrics->addWidget(makeMetricCard(QStringLiteral("打印机"), printerCountLabel_), 0);
  metrics->addWidget(makeMetricCard(QStringLiteral("作业"), jobCountLabel_), 0);
  metrics->addWidget(makeMetricCard(QStringLiteral("输出目录"), outputDirLabel_), 1);
  root->addLayout(metrics);

  auto* tablePanel = new QFrame();
  tablePanel->setObjectName(QStringLiteral("TablePanel"));
  auto* tablePanelLayout = new QVBoxLayout(tablePanel);
  tablePanelLayout->setContentsMargins(16, 14, 16, 16);
  tablePanelLayout->setSpacing(12);

  auto* tableHeader = new QHBoxLayout();
  tableHeader->setContentsMargins(0, 0, 0, 0);
  tableHeader->setSpacing(10);

  auto* tableTitle = new QLabel(QStringLiteral("作业队列"));
  tableTitle->setObjectName(QStringLiteral("PanelHeader"));
  resultCountLabel_ = new QLabel(QStringLiteral("显示 0 / 0"));
  resultCountLabel_->setObjectName(QStringLiteral("MutedText"));

  searchEdit_ = new QLineEdit();
  searchEdit_->setObjectName(QStringLiteral("SearchBox"));
  searchEdit_->setPlaceholderText(QStringLiteral("搜索"));
  searchEdit_->setClearButtonEnabled(true);

  phaseFilter_ = new QComboBox();
  phaseFilter_->setObjectName(QStringLiteral("PhaseFilter"));
  phaseFilter_->addItem(QStringLiteral("全部阶段"), QString());
  phaseFilter_->addItem(QStringLiteral("已接收"), QStringLiteral("accepted"));
  phaseFilter_->addItem(QStringLiteral("假脱机"), QStringLiteral("spooling"));
  phaseFilter_->addItem(QStringLiteral("打印中"), QStringLiteral("printing"));
  phaseFilter_->addItem(QStringLiteral("已完成"), QStringLiteral("completed"));
  phaseFilter_->addItem(QStringLiteral("失败"), QStringLiteral("failed"));
  phaseFilter_->addItem(QStringLiteral("已取消"), QStringLiteral("cancelled"));
  phaseFilter_->addItem(QStringLiteral("停滞"), QStringLiteral("stalled"));
  phaseFilter_->setSizeAdjustPolicy(QComboBox::AdjustToContents);

  clearFilterBtn_ = new QPushButton(QStringLiteral("重置"));
  clearFilterBtn_->setIcon(style()->standardIcon(QStyle::SP_DialogResetButton));
  setButtonRole(clearFilterBtn_, "quiet");

  tableHeader->addWidget(tableTitle);
  tableHeader->addWidget(resultCountLabel_);
  tableHeader->addStretch(1);
  tableHeader->addWidget(searchEdit_);
  tableHeader->addWidget(phaseFilter_);
  tableHeader->addWidget(clearFilterBtn_);
  tablePanelLayout->addLayout(tableHeader);

  tableView_ = new QTableView();
  tableView_->setObjectName(QStringLiteral("JobTable"));
  tableView_->setModel(jobProxy_);
  tableView_->setSelectionBehavior(QAbstractItemView::SelectRows);
  tableView_->setSelectionMode(QAbstractItemView::SingleSelection);
  tableView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  tableView_->setAlternatingRowColors(true);
  tableView_->setSortingEnabled(true);
  tableView_->setShowGrid(false);
  tableView_->setWordWrap(false);
  tableView_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  tableView_->verticalHeader()->hide();
  tableView_->verticalHeader()->setDefaultSectionSize(42);
  tableView_->horizontalHeader()->setHighlightSections(false);
  tableView_->horizontalHeader()->setStretchLastSection(false);
  tableView_->horizontalHeader()->setSectionsClickable(true);
  tableView_->horizontalHeader()->setSectionsMovable(false);

  // All columns Interactive so user can drag-resize.
  // ColDocument uses Stretch to absorb remaining space.
  tableView_->horizontalHeader()->setSectionResizeMode(JobTableModel::ColJobId, QHeaderView::Interactive);
  tableView_->horizontalHeader()->setSectionResizeMode(JobTableModel::ColDocument, QHeaderView::Stretch);
  tableView_->horizontalHeader()->setSectionResizeMode(JobTableModel::ColUser, QHeaderView::Interactive);
  tableView_->horizontalHeader()->setSectionResizeMode(JobTableModel::ColPhase, QHeaderView::Interactive);
  tableView_->horizontalHeader()->setSectionResizeMode(JobTableModel::ColProgress, QHeaderView::Interactive);
  tableView_->horizontalHeader()->setSectionResizeMode(JobTableModel::ColTotalPages, QHeaderView::Interactive);
  tableView_->horizontalHeader()->setSectionResizeMode(JobTableModel::ColCopies, QHeaderView::Interactive);
  tableView_->horizontalHeader()->setSectionResizeMode(JobTableModel::ColPrice, QHeaderView::Interactive);
  tableView_->horizontalHeader()->setSectionResizeMode(JobTableModel::ColSubmitTime, QHeaderView::Interactive);

  // Snap to content on first load, then user overrides by dragging.
  tableView_->resizeColumnsToContents();
  tableView_->horizontalHeader()->setMinimumSectionSize(40);

  tableView_->sortByColumn(JobTableModel::ColSubmitTime, Qt::DescendingOrder);

  // ── Document link delegate: blue underlined, click to open ──────────
  auto* docDelegate = new DocumentLinkDelegate(tableView_);
  tableView_->setItemDelegateForColumn(JobTableModel::ColDocument, docDelegate);
  connect(docDelegate, &DocumentLinkDelegate::documentClicked,
          this, [this](const QString& docName)
          {
            if (!docName.isEmpty() && !lastOutputDir_.isEmpty())
            {
              // Try to open the captured job file for that document.
              // Scan the jobs directory for matching file.
              QDir jobsDir(lastOutputDir_ + QStringLiteral("/jobs"));
              if (jobsDir.exists())
              {
                const auto entries = jobsDir.entryInfoList(QDir::Files);
                for (const auto& entry : entries)
                {
                  if (entry.fileName().contains(docName, Qt::CaseInsensitive) ||
                      entry.fileName().startsWith(QStringLiteral("job-")))
                  {
                    QDesktopServices::openUrl(QUrl::fromLocalFile(entry.absoluteFilePath()));
                    break;
                  }
                }
              }
              // Fallback: open the output directory itself.
              if (!jobsDir.exists())
              {
                QDesktopServices::openUrl(QUrl::fromLocalFile(lastOutputDir_));
              }
            }
            statusBar()->showMessage(
              QStringLiteral("已打开文档: %1").arg(docName), 3000);
          });

  emptyStateLabel_ = new QLabel(QStringLiteral("暂无打印作业"));
  emptyStateLabel_->setObjectName(QStringLiteral("EmptyState"));
  emptyStateLabel_->setAlignment(Qt::AlignCenter);

  tableStack_ = new QStackedWidget();
  tableStack_->addWidget(emptyStateLabel_);
  tableStack_->addWidget(tableView_);
  tablePanelLayout->addWidget(tableStack_, 1);

  detailPanel_ = new JobDetailPanel();
  timeline_ = new LifecycleTimeline();

  // Vertical splitter: user can drag to resize detail vs timeline heights.
  auto* sideSplitter = new QSplitter(Qt::Vertical);
  sideSplitter->setObjectName(QStringLiteral("SideSplitter"));
  sideSplitter->setHandleWidth(6);
  sideSplitter->setChildrenCollapsible(false);
  sideSplitter->addWidget(detailPanel_);
  sideSplitter->addWidget(timeline_);
  sideSplitter->setStretchFactor(0, 3);
  sideSplitter->setStretchFactor(1, 2);
  sideSplitter->setSizes(QList<int>{300, 200});

  auto* splitter = new QSplitter(Qt::Horizontal);
  splitter->setObjectName(QStringLiteral("WorkspaceSplitter"));
  splitter->setHandleWidth(6);
  splitter->setChildrenCollapsible(false);
  splitter->addWidget(tablePanel);
  splitter->addWidget(sideSplitter);
  splitter->setStretchFactor(0, 3);
  splitter->setStretchFactor(1, 2);
  splitter->setSizes(QList<int>{760, 420});

  // ── Bottom panel: statistics + user task management ──────────────────

  statisticsPanel_ = new StatisticsPanel();
  userTaskPanel_ = new UserTaskPanel(userTaskModel_);

  auto* bottomPanel = new QFrame();
  bottomPanel->setObjectName(QStringLiteral("BottomPanel"));
  auto* bottomLayout = new QVBoxLayout(bottomPanel);
  bottomLayout->setContentsMargins(0, 0, 0, 0);
  bottomLayout->setSpacing(12);
  bottomLayout->addWidget(statisticsPanel_);
  bottomLayout->addWidget(userTaskPanel_, 1);

  // ── Wrap top and bottom in a vertical splitter ──────────────────────

  auto* verticalSplitter = new QSplitter(Qt::Vertical);
  verticalSplitter->setObjectName(QStringLiteral("VerticalSplitter"));
  verticalSplitter->setHandleWidth(6);
  verticalSplitter->setChildrenCollapsible(false);
  verticalSplitter->addWidget(splitter);
  verticalSplitter->addWidget(bottomPanel);
  verticalSplitter->setStretchFactor(0, 3);   // top = 75%
  verticalSplitter->setStretchFactor(1, 1);   // bottom = 25%
  verticalSplitter->setSizes(QList<int>{520, 180});
  root->addWidget(verticalSplitter, 1);

  statusBar()->showMessage(QStringLiteral("就绪"));

  connect(startBtn_, &QPushButton::clicked, this, &MainWindow::onStartClicked);
  connect(stopBtn_, &QPushButton::clicked, this, &MainWindow::onStopClicked);
  connect(openOutputBtn_, &QPushButton::clicked, this, &MainWindow::onOpenOutputClicked);
  connect(clearFilterBtn_, &QPushButton::clicked, this,
          [this]()
          {
            searchEdit_->clear();
            phaseFilter_->setCurrentIndex(0);
          });

  connect(searchEdit_, &QLineEdit::textChanged, this,
          [this](const QString& text)
          {
            static_cast<JobFilterProxyModel*>(jobProxy_)->setSearchText(text);
            refreshEmptyState();
            selectFirstVisibleRow();
          });
  connect(phaseFilter_, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this,
          [this](int)
          {
            static_cast<JobFilterProxyModel*>(jobProxy_)
              ->setPhaseKey(phaseFilter_->currentData().toString());
            refreshEmptyState();
            selectFirstVisibleRow();
          });

  connect(tableView_->selectionModel(), &QItemSelectionModel::currentRowChanged,
          this, &MainWindow::onTableSelectionChanged);

  connect(tableModel_, &QAbstractItemModel::dataChanged, this,
          [this](const QModelIndex&, const QModelIndex&, const QList<int>&)
          {
            refreshSelectedJob();
            refreshSummaryCards();
            refreshEmptyState();
          });
  connect(tableModel_, &QAbstractItemModel::rowsInserted, this,
          [this](const QModelIndex&, int, int)
          {
            refreshSummaryCards();
            refreshEmptyState();
            QTimer::singleShot(0, this, &MainWindow::selectFirstVisibleRow);
          });
  connect(tableModel_, &QAbstractItemModel::modelReset, this,
          [this]()
          {
            refreshSummaryCards();
            refreshEmptyState();
            refreshSelectedJob();
          });

  connect(jobProxy_, &QAbstractItemModel::rowsInserted, this,
          [this](const QModelIndex&, int, int)
          {
            refreshEmptyState();
            selectFirstVisibleRow();
          });
  connect(jobProxy_, &QAbstractItemModel::rowsRemoved, this,
          [this](const QModelIndex&, int, int)
          {
            refreshEmptyState();
            refreshSelectedJob();
          });
  connect(jobProxy_, &QAbstractItemModel::modelReset, this,
          [this]()
          {
            refreshEmptyState();
            refreshSelectedJob();
          });
  connect(jobProxy_, &QAbstractItemModel::layoutChanged, this,
          [this]()
          {
            refreshEmptyState();
          });

  connect(bridge_, &WatcherBridge::jobSnapshotReceived,
          tableModel_, &JobTableModel::onJobSnapshot);
  connect(bridge_, &WatcherBridge::jobSnapshotReceived,
          this, &MainWindow::onJobCountChanged);
  connect(bridge_, &WatcherBridge::jobSnapshotReceived,
          userTaskModel_, &UserTaskModel::onJobSnapshot);
  connect(bridge_, &WatcherBridge::metadataReceived,
          tableModel_, &JobTableModel::onMetadata);
  connect(bridge_, &WatcherBridge::metadataReceived,
          userTaskModel_, &UserTaskModel::onMetadata);
  connect(bridge_, &WatcherBridge::lifecycleEventReceived,
          tableModel_, &JobTableModel::onLifecycleEvent);
  connect(bridge_, &WatcherBridge::lifecycleEventReceived,
          userTaskModel_, &UserTaskModel::onLifecycleEvent);
  connect(bridge_, &WatcherBridge::watcherError, this,
          [this](const QString& error)
          {
            statusBar()->showMessage(QStringLiteral("警告: %1").arg(error), 5000);
            updateButtonStates();
            updatePrinterLabel();
          });
  connect(bridge_, &WatcherBridge::printerSkipped,
          this, &MainWindow::onPrinterSkipped);
  connect(bridge_, &WatcherBridge::sessionFinished, this,
          [this]()
          {
            updateButtonStates();
            updatePrinterLabel();
          });

  // ── User task / statistics wiring ───────────────────────────────────

  connect(userTaskModel_, &QAbstractItemModel::dataChanged,
          this, &MainWindow::refreshTaskPanels);
  connect(userTaskModel_, &QAbstractItemModel::rowsInserted,
          this, &MainWindow::refreshTaskPanels);
  connect(userTaskModel_, &QAbstractItemModel::modelReset,
          this, &MainWindow::refreshTaskPanels);
  connect(tableModel_, &QAbstractItemModel::dataChanged,
          this, &MainWindow::refreshTaskPanels);
  connect(tableModel_, &QAbstractItemModel::rowsInserted,
          this, &MainWindow::refreshTaskPanels);
  connect(userTaskPanel_, &UserTaskPanel::taskCompleted,
          this, &MainWindow::refreshTaskPanels);

  // ── Initialise ──────────────────────────────────────────────────────

  updateButtonStates();
  updatePrinterLabel();
  refreshSummaryCards();
  refreshEmptyState();

  QTimer::singleShot(100, this, &MainWindow::autoStart);
}

MainWindow::~MainWindow()
{
  bridge_->stopWatching();
}

void MainWindow::onStartClicked()
{
  if (bridge_->isRunning())
  {
    return;
  }

  tableModel_->clear();
  userTaskModel_->clear();
  detailPanel_->clear();
  timeline_->clear();
  refreshTaskPanels();

  const QString documentsPath =
    QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  const QString timestamp =
    QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
  lastOutputDir_ =
    QDir(documentsPath).filePath(QStringLiteral("WPCE/capture-%1").arg(timestamp));

  outputDirLabel_->setText(QDir::toNativeSeparators(lastOutputDir_));
  outputDirLabel_->setToolTip(QDir::toNativeSeparators(lastOutputDir_));

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
  updateButtonStates();
  updatePrinterLabel();
  statusBar()->showMessage(QStringLiteral("已停止"));
}

void MainWindow::onOpenOutputClicked()
{
  if (lastOutputDir_.isEmpty())
  {
    return;
  }

  QDesktopServices::openUrl(QUrl::fromLocalFile(lastOutputDir_));
}

void MainWindow::onTableSelectionChanged()
{
  refreshSelectedJob();
}

void MainWindow::onPrinterSkipped(const QString& printerName)
{
  statusBar()->showMessage(QStringLiteral("已跳过打印机: %1").arg(printerName),
                           3000);
}

void MainWindow::onJobCountChanged()
{
  QTimer::singleShot(0, this,
                     [this]()
                     {
                       refreshSummaryCards();
                       statusBar()->showMessage(
                         QStringLiteral("已同步 %1 个打印任务")
                           .arg(tableModel_->rowCount()));
                     });
}

void MainWindow::autoStart()
{
  onStartClicked();
}

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

  const int printerCount = running ? bridge_->watchedPrinters().size() : 0;
  printerCountLabel_->setText(QString::number(printerCount));
}

void MainWindow::refreshSummaryCards()
{
  const int total = tableModel_->rowCount();
  const int visible = jobProxy_->rowCount();

  jobCountLabel_->setText(QString::number(total));
  resultCountLabel_->setText(QStringLiteral("显示 %1 / %2").arg(visible).arg(total));

  const bool filtersActive =
    !searchEdit_->text().trimmed().isEmpty() || phaseFilter_->currentIndex() != 0;
  clearFilterBtn_->setEnabled(filtersActive);
}

void MainWindow::refreshEmptyState()
{
  refreshSummaryCards();

  const bool hasVisibleRows = jobProxy_->rowCount() > 0;
  tableStack_->setCurrentWidget(hasVisibleRows ? static_cast<QWidget*>(tableView_)
                                               : static_cast<QWidget*>(emptyStateLabel_));

  if (hasVisibleRows)
  {
    return;
  }

  emptyStateLabel_->setText(tableModel_->rowCount() == 0
                              ? QStringLiteral("暂无打印作业")
                              : QStringLiteral("没有匹配的作业"));
}

void MainWindow::refreshSelectedJob()
{
  const QModelIndex proxyIndex = tableView_->currentIndex();
  if (!proxyIndex.isValid())
  {
    detailPanel_->clear();
    timeline_->clear();
    return;
  }

  const QModelIndex sourceIndex = jobProxy_->mapToSource(proxyIndex);
  if (!sourceIndex.isValid())
  {
    detailPanel_->clear();
    timeline_->clear();
    return;
  }

  const auto rowData = tableModel_->jobAtRow(sourceIndex.row());
  if (!rowData.has_value())
  {
    detailPanel_->clear();
    timeline_->clear();
    return;
  }

  detailPanel_->showJob(rowData->job, rowData->metadata);
  timeline_->loadEvents(rowData->lifecycleEvents);
}

void MainWindow::selectFirstVisibleRow()
{
  if (tableView_->currentIndex().isValid() || jobProxy_->rowCount() == 0)
  {
    return;
  }

  const QModelIndex index = jobProxy_->index(0, 0);
  tableView_->selectionModel()->setCurrentIndex(
    index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
}

void MainWindow::refreshTaskPanels()
{
  statisticsPanel_->refresh(userTaskModel_);
}

}  // namespace wpce::ui
