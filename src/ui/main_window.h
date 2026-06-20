#pragma once

#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QTableView>
#include <QTimer>

namespace wpce::ui
{

class WatcherBridge;
class JobTableModel;
class JobDetailPanel;
class LifecycleTimeline;

/// Main application window for the WPCE Print Job Monitor.
///
/// Watches ALL local printers by default.  Layout:
///   [Toolbar: ▶ Start | ■ Stop | watched printer count]
///   [Job Table .................] [Detail Panel]
///                                [Timeline  ]
///   [Status Bar                          ]
class MainWindow : public QMainWindow
{
  Q_OBJECT

 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

 private slots:
  void onStartClicked();
  void onStopClicked();
  void onTableSelectionChanged();
  void onPrinterSkipped(const QString& printerName);
  void onJobCountChanged();

 private:
  void autoStart();
  void updateButtonStates();
  void updatePrinterLabel();

  // ── Bridge ───────────────────────────────────────────────────────────
  WatcherBridge* bridge_ = nullptr;

  // ── Toolbar widgets ──────────────────────────────────────────────────
  QLabel* printerCountLabel_ = nullptr;
  QPushButton* startBtn_ = nullptr;
  QPushButton* stopBtn_ = nullptr;

  // ── Central area ─────────────────────────────────────────────────────
  QTableView* tableView_ = nullptr;
  JobTableModel* tableModel_ = nullptr;
  JobDetailPanel* detailPanel_ = nullptr;
  LifecycleTimeline* timeline_ = nullptr;

  // ── Statistics ───────────────────────────────────────────────────────
  int observedJobCount_ = 0;
};

}  // namespace wpce::ui
