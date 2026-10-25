#pragma once

#include "common/app_config.h"

#include <QDateTime>
#include <QHash>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QTreeView>

class QCloseEvent;
class QLineEdit;
class QResizeEvent;
class QStackedWidget;

namespace wpce::ui
{

class WatcherBridge;
class BillingBatchModel;
class BillingService;
class HistoryExplorerDialog;
class JobDetailPanel;
class ReceiptPreviewPanel;
class QuickSettleDialog;

class MainWindow : public QMainWindow
{
  Q_OBJECT

 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

  [[nodiscard]] bool isSilentStartup() const;

 protected:
  void resizeEvent(QResizeEvent* event) override;
  void closeEvent(QCloseEvent* event) override;

 private slots:
  void onStartClicked();
  void onStopClicked();
  void onOpenOutputClicked();
  void onSettingsClicked();
  void onCompleteClicked();
  void onTreeSelectionChanged();
  void onPrinterSkipped(const QString& printerName);
  void onWatcherHeartbeat(const QString& printerName);
  void onBatchSettled(const QString& orderId, const QString& batchId);
  void checkStalePrinters();

 private:
  void autoStart();
  void updateButtonStates();
  void updatePrinterLabel();
  void refreshSummaryCards();
  void refreshEmptyState();
  void refreshSelectedDetail();
  void refreshGroupRowSpans();
  void selectNextRelevantBatch();
  void selectLastBatch();
  void showQuickSettleDialog();
  void refreshReceiptPanel();
  void applySearchFilter();
  void scheduleWorkspaceRebalance();
  void rebalanceWorkspaceSplitters();
  void loadOrCreateConfig();
  void applyConfig();
  void detectPrintersIfNeeded();
  void detectPrintersAsync();
  QString resolveStorageRoot() const;
  QString resolveOutputRoot() const;
  QString resolveStateDir() const;
  QString resolveOrderBucketPath() const;
  QString resolveHistoryRoot() const;
  QString resolveUserDir(const QString& displayName) const;
  void ensureUserDirectories(const QString& displayName);
  void migrateLegacyDailyStateIfNeeded();
  void cleanupExpiredCapturedData();

  common::AppConfig config_;

  WatcherBridge* bridge_ = nullptr;
  BillingService* billingService_ = nullptr;
  BillingBatchModel* batchModel_ = nullptr;

  QLabel* sessionStateLabel_ = nullptr;
  QLabel* printerCountLabel_ = nullptr;
  QLabel* userCountLabel_ = nullptr;
  QLabel* outputDirLabel_ = nullptr;
  QLabel* resultCountLabel_ = nullptr;
  QLabel* emptyStateLabel_ = nullptr;

  QPushButton* startBtn_ = nullptr;
  QPushButton* stopBtn_ = nullptr;
  QPushButton* openOutputBtn_ = nullptr;
  QPushButton* settingsBtn_ = nullptr;
  QPushButton* clearFilterBtn_ = nullptr;
  QPushButton* expandAllBtn_ = nullptr;
  QPushButton* collapseAllBtn_ = nullptr;
  QPushButton* completeUserBtn_ = nullptr;
  QPushButton* quickSettleBtn_ = nullptr;

  QLineEdit* searchEdit_ = nullptr;

  QTreeView* treeView_ = nullptr;
  QStackedWidget* tableStack_ = nullptr;
  JobDetailPanel* detailPanel_ = nullptr;
  QSplitter* workspaceSplitter_ = nullptr;
  QSplitter* verticalSplitter_ = nullptr;
  bool splitterRebalancePending_ = false;

  ReceiptPreviewPanel* receiptPanel_ = nullptr;
  QuickSettleDialog* quickSettleDialog_ = nullptr;
  HistoryExplorerDialog* historyDialog_ = nullptr;

  QHash<QString, QDateTime> printerHeartbeats_;
  QTimer* heartbeatCheckTimer_ = nullptr;
  QTimer* batchUpdateDebounce_ = nullptr;

  QString lastOutputDir_;
  bool closing_{false};
};

}  // namespace wpce::ui
