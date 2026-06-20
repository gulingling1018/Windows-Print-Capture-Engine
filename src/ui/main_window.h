#pragma once

#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QTableView>
#include <QTimer>

class QComboBox;
class QLineEdit;
class QSortFilterProxyModel;
class QStackedWidget;

namespace wpce::ui
{

class WatcherBridge;
class JobTableModel;
class JobDetailPanel;
class LifecycleTimeline;
class StatisticsPanel;
class UserTaskModel;
class UserTaskPanel;

class MainWindow : public QMainWindow
{
  Q_OBJECT

 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

 private slots:
  void onStartClicked();
  void onStopClicked();
  void onOpenOutputClicked();
  void onTableSelectionChanged();
  void onPrinterSkipped(const QString& printerName);
  void onJobCountChanged();

 private:
  void autoStart();
  void updateButtonStates();
  void updatePrinterLabel();
  void refreshSummaryCards();
  void refreshEmptyState();
  void refreshSelectedJob();
  void selectFirstVisibleRow();
  void refreshTaskPanels();

  WatcherBridge* bridge_ = nullptr;

  QLabel* sessionStateLabel_ = nullptr;
  QLabel* printerCountLabel_ = nullptr;
  QLabel* jobCountLabel_ = nullptr;
  QLabel* outputDirLabel_ = nullptr;
  QLabel* resultCountLabel_ = nullptr;
  QLabel* emptyStateLabel_ = nullptr;

  QPushButton* startBtn_ = nullptr;
  QPushButton* stopBtn_ = nullptr;
  QPushButton* openOutputBtn_ = nullptr;
  QPushButton* clearFilterBtn_ = nullptr;

  QLineEdit* searchEdit_ = nullptr;
  QComboBox* phaseFilter_ = nullptr;

  QTableView* tableView_ = nullptr;
  QStackedWidget* tableStack_ = nullptr;
  JobTableModel* tableModel_ = nullptr;
  QSortFilterProxyModel* jobProxy_ = nullptr;
  JobDetailPanel* detailPanel_ = nullptr;
  LifecycleTimeline* timeline_ = nullptr;

  StatisticsPanel* statisticsPanel_ = nullptr;
  UserTaskModel* userTaskModel_ = nullptr;
  UserTaskPanel* userTaskPanel_ = nullptr;

  QString lastOutputDir_;
};

}  // namespace wpce::ui
