#include "ui/main_window.h"
#include "ui/job_detail_panel.h"
#include "ui/job_table_model.h"
#include "ui/lifecycle_timeline.h"
#include "ui/watcher_bridge.h"

#include "watcher/print_watcher.h"

#ifdef _WIN32
#include <windows.h>
#include <winspool.h>
#endif

#include <QApplication>
#include <QDateTime>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QStandardPaths>
#include <QToolBar>
#include <QVBoxLayout>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace wpce::ui
{

// ── MainWindow ─────────────────────────────────────────────────────────

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
  setWindowTitle(QStringLiteral("WPCE \xe6\x89\x93\xe5\x8d\xb0\xe4\xbb\xbb\xe5\x8a\xa1\xe7\x9b\x91\xe6\x8e\xa7")); // WPCE 打印任务监控
  resize(1100, 680);

  // ── Toolbar ──────────────────────────────────────────────────────────

  auto* toolbar = new QToolBar(QStringLiteral("\xe5\xb7\xa5\xe5\x85\xb7\xe6\xa0\x8f")); // 工具栏
  toolbar->setMovable(false);
  addToolBar(toolbar);

  startBtn_ = new QPushButton(QStringLiteral("\xe2\x96\xb6 \xe5\xbc\x80\xe5\xa7\x8b\xe7\x9b\x91\xe5\x90\xac")); // ▶ 开始监听
  startBtn_->setStyleSheet(QStringLiteral("QPushButton { color: #1a8c4a; font-weight: bold; }"));
  toolbar->addWidget(startBtn_);

  stopBtn_ = new QPushButton(QStringLiteral("\xe2\x96\xa0 \xe5\x81\x9c\xe6\xad\xa2")); // ■ 停止
  stopBtn_->setStyleSheet(QStringLiteral("QPushButton { color: #cc2222; font-weight: bold; }"));
  toolbar->addWidget(stopBtn_);

  toolbar->addSeparator();

  printerCountLabel_ = new QLabel();
  toolbar->addWidget(printerCountLabel_);

  // ── Central area ─────────────────────────────────────────────────────

  tableView_ = new QTableView();
  tableView_->setSelectionBehavior(QAbstractItemView::SelectRows);
  tableView_->setSelectionMode(QAbstractItemView::SingleSelection);
  tableView_->setAlternatingRowColors(true);
  tableView_->setSortingEnabled(false);
  tableView_->horizontalHeader()->setStretchLastSection(true);
  tableView_->verticalHeader()->hide();

  tableModel_ = new JobTableModel(this);
  tableView_->setModel(tableModel_);

  detailPanel_ = new JobDetailPanel();
  timeline_ = new LifecycleTimeline();

  auto* rightWidget = new QWidget();
  auto* rightLayout = new QVBoxLayout(rightWidget);
  rightLayout->setContentsMargins(0, 0, 0, 0);
  rightLayout->addWidget(detailPanel_, 1);
  rightLayout->addWidget(timeline_, 1);

  auto* splitter = new QSplitter(Qt::Horizontal);
  splitter->addWidget(tableView_);
  splitter->addWidget(rightWidget);
  splitter->setStretchFactor(0, 3);
  splitter->setStretchFactor(1, 2);

  setCentralWidget(splitter);

  // ── Status bar ───────────────────────────────────────────────────────

  statusBar()->showMessage(QStringLiteral("\xe5\xb0\xb1\xe7\xbb\xaa")); // 就绪

  // ── Bridge ───────────────────────────────────────────────────────────

  bridge_ = new WatcherBridge(this);

  // ── Connections ──────────────────────────────────────────────────────

  connect(startBtn_, &QPushButton::clicked, this, &MainWindow::onStartClicked);
  connect(stopBtn_, &QPushButton::clicked, this, &MainWindow::onStopClicked);

  connect(tableView_->selectionModel(), &QItemSelectionModel::currentRowChanged,
          this, &MainWindow::onTableSelectionChanged);

  connect(bridge_, &WatcherBridge::jobSnapshotReceived,
          tableModel_, &JobTableModel::onJobSnapshot);
  connect(bridge_, &WatcherBridge::jobSnapshotReceived,
          this, &MainWindow::onJobCountChanged);

  connect(bridge_, &WatcherBridge::metadataReceived,
          tableModel_, &JobTableModel::onMetadata);

  connect(bridge_, &WatcherBridge::lifecycleEventReceived,
          tableModel_, &JobTableModel::onLifecycleEvent);

  connect(bridge_, &WatcherBridge::lifecycleEventReceived,
          timeline_, &LifecycleTimeline::addEvent);

  connect(bridge_, &WatcherBridge::watcherError, this,
          [this](const QString& error)
          {
            statusBar()->showMessage(
              QStringLiteral("\xe2\x9a\xa0 %1").arg(error)); // ⚠ error
          });

  connect(bridge_, &WatcherBridge::printerSkipped,
          this, &MainWindow::onPrinterSkipped);

  // ── Auto-start ───────────────────────────────────────────────────────

  updateButtonStates();
  updatePrinterLabel();

  // Start after the event loop is running.
  QTimer::singleShot(100, this, &MainWindow::autoStart);
}

MainWindow::~MainWindow()
{
  bridge_->stopWatching();
}

// ── Slots ──────────────────────────────────────────────────────────────

void MainWindow::onStartClicked()
{
  observedJobCount_ = 0;
  timeline_->clear();
  detailPanel_->clear();

  // Default output directory: Documents\WPCE\capture-<timestamp>
  const QString docPath = QStandardPaths::writableLocation(
    QStandardPaths::DocumentsLocation);
  const QString ts = QDateTime::currentDateTime().toString(
    QStringLiteral("yyyyMMdd-HHmmss"));
  const QString outputDir = QStringLiteral("%1/WPCE/%2")
    .arg(docPath, ts);

  bridge_->startWatching(outputDir);
  updateButtonStates();
  updatePrinterLabel();

  statusBar()->showMessage(
    QStringLiteral("\xe7\x9b\x91\xe5\x90\xac\xe4\xb8\xad\xe2\x80\xa6 \xe2\x86\x92 %1") // 监听中… → …
    .arg(outputDir));
}

void MainWindow::onStopClicked()
{
  bridge_->stopWatching();
  updateButtonStates();
  updatePrinterLabel();
  statusBar()->showMessage(QStringLiteral("\xe5\xb7\xb2\xe5\x81\x9c\xe6\xad\xa2")); // 已停止
}

void MainWindow::onTableSelectionChanged()
{
  const QModelIndex& idx = tableView_->currentIndex();
  if (!idx.isValid())
  {
    detailPanel_->clear();
    timeline_->clear();
    return;
  }

  const int row = idx.row();
  const JobRowData* rowData = tableModel_->jobAtRow(row);
  if (rowData == nullptr)
  {
    detailPanel_->clear();
    timeline_->clear();
    return;
  }

  detailPanel_->showJob(rowData->job, rowData->metadata);
}

void MainWindow::onPrinterSkipped(const QString& printerName)
{
  statusBar()->showMessage(
    QStringLiteral("\xe8\xb7\xb3\xe8\xbf\x87: %1").arg(printerName), // 跳过: ...
    3000);
}

void MainWindow::onJobCountChanged()
{
  ++observedJobCount_;
  statusBar()->showMessage(
    QStringLiteral("\xe5\xb7\xb2\xe8\xa7\x82\xe5\xaf\x9f %1 \xe4\xb8\xaa\xe4\xbb\xbb\xe5\x8a\xa1") // 已观察 N 个任务
    .arg(observedJobCount_));
}

// ── Helpers ─────────────────────────────────────────────────────────────

void MainWindow::autoStart()
{
  onStartClicked();
}

void MainWindow::updateButtonStates()
{
  const bool running = bridge_->isRunning();
  startBtn_->setEnabled(!running);
  stopBtn_->setEnabled(running);
}

void MainWindow::updatePrinterLabel()
{
  if (!bridge_->isRunning())
  {
    printerCountLabel_->setText(
      QStringLiteral("\xe6\x9c\xaa\xe5\xbc\x80\xe5\xa7\x8b")); // 未开始
    return;
  }

  const QStringList printers = bridge_->watchedPrinters();
  printerCountLabel_->setText(
    QStringLiteral("\xe6\xad\xa3\xe5\x9c\xa8\xe7\x9b\x91\xe5\x90\xac %1 \xe5\x8f\xb0\xe6\x89\x93\xe5\x8d\xb0\xe6\x9c\xba") // 正在监听 N 台打印机
    .arg(printers.size()));
}

}  // namespace wpce::ui
