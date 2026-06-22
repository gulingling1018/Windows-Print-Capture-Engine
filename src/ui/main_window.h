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

class QLineEdit;
class QResizeEvent;
class QStackedWidget;

namespace wpce::ui
{

class WatcherBridge;
class UserGroupModel;
class JobDetailPanel;
class ReceiptPreviewPanel;
class UserTaskModel;

class MainWindow : public QMainWindow
{
  Q_OBJECT

 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

 protected:
  void resizeEvent(QResizeEvent* event) override;

 private slots:
  void onStartClicked();
  void onStopClicked();
  void onOpenOutputClicked();
  void onSettingsClicked();
  void onCompleteClicked();
  void onTreeSelectionChanged();
  void onPrinterSkipped(const QString& printerName);
  void onWatcherHeartbeat(const QString& printerName);
  void onUserCompleted(const QString& userId);
  void checkStalePrinters();

 private:
  void autoStart();
  void updateButtonStates();
  void updatePrinterLabel();
  void refreshSummaryCards();
  void refreshEmptyState();
  void refreshSelectedDetail();
  void refreshGroupRowSpans();
  void selectFirstIncompleteUser();
  void refreshReceiptPanel();
  void applySearchFilter();
  void scheduleWorkspaceRebalance();
  void rebalanceWorkspaceSplitters();
  void loadOrCreateConfig();
  void applyConfig();
  QString resolveOutputRoot() const;

  common::AppConfig config_;

  WatcherBridge* bridge_ = nullptr;

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

  QLineEdit* searchEdit_ = nullptr;

  QTreeView* treeView_ = nullptr;
  QStackedWidget* tableStack_ = nullptr;
  UserGroupModel* groupModel_ = nullptr;
  JobDetailPanel* detailPanel_ = nullptr;
  QSplitter* workspaceSplitter_ = nullptr;
  QSplitter* verticalSplitter_ = nullptr;
  bool splitterRebalancePending_ = false;

  UserTaskModel* userTaskModel_ = nullptr;
  ReceiptPreviewPanel* receiptPanel_ = nullptr;

  QHash<QString, QDateTime> printerHeartbeats_;
  QTimer* heartbeatCheckTimer_ = nullptr;

  QString lastOutputDir_;
};

}  // namespace wpce::ui
