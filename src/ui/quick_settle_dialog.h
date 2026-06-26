#pragma once

#include "ui/billing_batch_model.h"
#include "ui/pricing_config.h"

#include <QAction>
#include <QDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace wpce::ui
{

class BillingService;

/// Compact floating settlement window — card flipper style.
/// Operator navigates left/right between batches (Active + Suspended).
/// Context-sensitive action button changes based on current batch state:
///   Active    → "挂单" + "结算"
///   Suspended → "恢复" + "结算"
///   Settled   → both disabled
///
/// Size: 460×600 default, draggable, expandable to 900×800.
class QuickSettleDialog : public QDialog
{
  Q_OBJECT

 public:
  explicit QuickSettleDialog(BillingBatchModel* model,
                             BillingService* service,
                             const PricingConfig* pricing,
                             int initialWidth = 460,
                             int initialHeight = 600,
                             int initialCollapsedHeight = 0,
                             bool initialCollapsed = false,
                             QWidget* parent = nullptr);
  ~QuickSettleDialog() override;

  void setCurrentBatchIndex(int index);

 public slots:
  void refresh();
  /// Called by MainWindow when a new batch is created — auto-switches to it
  /// if the dialog is in a settled batch or the inactivity timer has fired.
  void onBatchCreated(const QString& batchId);

  /// Explicit fallback after batch-structure changes.  It only jumps after
  /// the inactivity window has elapsed; new active batches still use
  /// onBatchCreated() for immediate focus.
  /// Called by MainWindow after batch-structure changes.
  void ensurePriorityView();

 signals:
  void settleRequested(const QString& batchId);
  void hangUpRequested(const QString& batchId);
  void resumeRequested(const QString& batchId);
  void batchFocusChanged(const QString& batchId);
  void geometryChanged(int width, int height, int collapsedHeight, bool collapsed);

 protected:
  void closeEvent(QCloseEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void hideEvent(QHideEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;
#ifdef _WIN32
  bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
#endif

 private:
  void buildUi();
  void rebuildUi();
  void switchToPrev();
  void switchToNext();
  void switchToPrevRelevant();
  void switchToNextRelevant();
  void onSettleClicked();
  void onActionClicked();
  void updatePinnedState();
  void updateActionButtons();
  void updateNavButtonStates();
  void updatePositionIndicator();

  /// Return the model row index that should be shown by priority:
  ///   1. Current active-batch (most recently created active)
  ///   2. First suspended batch
  ///   3. Last settled batch
  /// Returns -1 if the model is empty.
  int priorityRow() const;

  /// Reset the inactivity timer (called on any user interaction).
  void resetInactivityTimer();

  /// Toggle collapsed/expanded state.
  void toggleCollapsed();

  /// Called when the operator edits a color-pages cell in column 4.
  void onPageCellChanged(int row, int col);

  /// Create system-tray icon for minimize-to-tray restore path.
  void createTrayIcon();
  void minimizeToTray();
  void showFromTray();
  void onTrayActivated(QSystemTrayIcon::ActivationReason reason);

  /// Debounced refresh: coalesces multiple rapid refresh() calls.
  QTimer refreshDebounce_;
  static constexpr int kRefreshDebounceMs = 50;
  bool rebuildingSuppressed_ = false;

  BillingBatchModel* model_ = nullptr;
  BillingService* service_ = nullptr;
  const PricingConfig* pricingConfig_ = nullptr;
  int currentIndex_ = -1;
  bool pinned_ = true;

  // Navigation button click debounce (distinguish single vs double click).
  qint64 lastNavPressMs_ = 0;
  bool navIsDoubleClick_ = false;

  // Inactivity auto-refresh: after 10 s without interaction, jump to priority row.
  QTimer inactivityTimer_;
  static constexpr int kInactivityMs = 10000;

  // Collapsed state: show only title bar + summary + action bar.
  bool collapsed_ = false;
  QPushButton* collapseBtn_ = nullptr;
  QWidget* contentArea_ = nullptr;       // The main content widget
  QFrame* firstAnchorFrame_ = nullptr;   // Hidden when collapsed
  QFrame* lastAnchorFrame_ = nullptr;    // Hidden when collapsed
  QSize expandedSize_;                   // Remembered expanded geometry
  int collapsedHeight_ = 0;              // Remembered collapsed height

  // UI widgets — title bar
  QWidget* titleBar_ = nullptr;
  QLabel* titleLabel_ = nullptr;
  QLabel* stateBadgeLabel_ = nullptr;
  QLabel* positionLabel_ = nullptr;
  QPushButton* pinBtn_ = nullptr;
  QPushButton* minimizeBtn_ = nullptr;
  QPushButton* closeBtn_ = nullptr;

  // Content
  QLabel* firstDocLabel_ = nullptr;
  QLabel* firstTimeLabel_ = nullptr;
  QLabel* firstPagesLabel_ = nullptr;

  QTableWidget* jobTable_ = nullptr;

  QLabel* lastDocLabel_ = nullptr;
  QLabel* lastTimeLabel_ = nullptr;
  QLabel* lastPagesLabel_ = nullptr;

  QLabel* summaryLabel_ = nullptr;
  QLabel* colorPendingLabel_ = nullptr;
  QWidget* collapsedColorArea_ = nullptr;

  // System tray restore path
  QSystemTrayIcon* trayIcon_ = nullptr;
  QMenu* trayMenu_ = nullptr;
  bool minimizedToTray_ = false;
  bool firstShow_ = true;
  bool inToggle_ = false;  // suppress resizeEvent during toggleCollapsed

  // Action bar
  QPushButton* prevBtn_ = nullptr;
  QPushButton* actionBtn_ = nullptr;
  QPushButton* nextBtn_ = nullptr;
  QPushButton* settleBtn_ = nullptr;
};

}  // namespace wpce::ui
