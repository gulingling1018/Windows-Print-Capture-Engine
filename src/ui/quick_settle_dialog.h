#pragma once

#include "ui/billing_batch_model.h"
#include "ui/pricing_config.h"

#include <QDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
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
                             QWidget* parent = nullptr);

  void setCurrentBatchIndex(int index);

 public slots:
  void refresh();

 signals:
  void settleRequested(const QString& batchId);
  void hangUpRequested(const QString& batchId);
  void resumeRequested(const QString& batchId);
  void batchFocusChanged(const QString& batchId);

 protected:
  void closeEvent(QCloseEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

 private:
  void buildUi();
  void rebuildUi();
  void updateColumnVisibility();
  void switchToPrev();
  void switchToNext();
  void onSettleClicked();
  void onActionClicked();
  void updatePinnedState();
  void updateActionButtons();
  void updatePositionIndicator();

  BillingBatchModel* model_ = nullptr;
  BillingService* service_ = nullptr;
  const PricingConfig* pricingConfig_ = nullptr;
  int currentIndex_ = -1;
  bool pinned_ = true;

  // Drag support for frameless window.
  QPoint dragStartPos_;
  bool dragging_ = false;

  // UI widgets — title bar
  QWidget* titleBar_ = nullptr;
  QLabel* titleLabel_ = nullptr;
  QLabel* stateBadgeLabel_ = nullptr;
  QLabel* positionLabel_ = nullptr;
  QPushButton* pinBtn_ = nullptr;
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

  // Action bar
  QPushButton* prevBtn_ = nullptr;
  QPushButton* actionBtn_ = nullptr;
  QPushButton* nextBtn_ = nullptr;
  QPushButton* settleBtn_ = nullptr;
};

}  // namespace wpce::ui
