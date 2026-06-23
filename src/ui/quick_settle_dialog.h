#pragma once

#include "ui/billing_batch_model.h"
#include "ui/pricing_config.h"

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace wpce::ui
{

class BillingService;

/// Compact floating settlement window for rapid operator workflow.
/// Shows a single batch's jobs with first/last document anchors,
/// scrollable job list, summary bar, and prev/hang-up/next/settle controls.
///
/// Size: 460×600 default, draggable, expandable to 900×800.
/// Column visibility is width-driven:
///   460-580: # / doc / pages / duplex / price
///   581-680: + copies
///   681-800: + printer
///   801-900: + paper + full doc name + time
class QuickSettleDialog : public QDialog
{
  Q_OBJECT

 public:
  explicit QuickSettleDialog(BillingBatchModel* model,
                             BillingService* service,
                             const PricingConfig* pricing,
                             QWidget* parent = nullptr);

  void setCurrentBatchIndex(int index);

 signals:
  void settleRequested(const QString& batchId);
  void hangUpRequested(const QString& batchId);
  void resumeRequested(const QString& batchId);

 protected:
  void closeEvent(QCloseEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;

 private:
  void buildUi();
  void rebuildUi();
  void updateColumnVisibility();
  void switchToPrev();
  void switchToNext();
  void onSettleClicked();
  void onHangUpClicked();
  void onResumeClicked();
  void updatePinnedState();
  void updateHangUpState();
  void updateResumeMenu();

  BillingBatchModel* model_ = nullptr;
  BillingService* service_ = nullptr;
  const PricingConfig* pricingConfig_ = nullptr;
  int currentIndex_ = -1;
  bool pinned_ = true;

  // Drag support for frameless window.
  QPoint dragStartPos_;
  bool dragging_ = false;

  // UI widgets
  QWidget* titleBar_ = nullptr;
  QLabel* titleLabel_ = nullptr;
  QLabel* stateBadgeLabel_ = nullptr;
  QPushButton* pinBtn_ = nullptr;
  QPushButton* closeBtn_ = nullptr;

  QLabel* firstDocLabel_ = nullptr;
  QLabel* firstTimeLabel_ = nullptr;
  QLabel* firstPagesLabel_ = nullptr;

  QTableWidget* jobTable_ = nullptr;

  QLabel* lastDocLabel_ = nullptr;
  QLabel* lastTimeLabel_ = nullptr;
  QLabel* lastPagesLabel_ = nullptr;

  QLabel* summaryLabel_ = nullptr;

  QPushButton* prevBtn_ = nullptr;
  QPushButton* hangUpBtn_ = nullptr;
  QPushButton* resumeBtn_ = nullptr;
  QPushButton* nextBtn_ = nullptr;
  QPushButton* settleBtn_ = nullptr;
};

}  // namespace wpce::ui
