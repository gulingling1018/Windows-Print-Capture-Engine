#pragma once

#include <QColor>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QWidget>

namespace wpce::ui
{

/// Single receipt card showing one user's pending order.
class ReceiptCard : public QFrame
{
  Q_OBJECT

 public:
  explicit ReceiptCard(const QString& batchId, const QString& displayName,
                       const QString& firstDoc, const QString& lastDoc,
                       int totalPages, double totalPrice,
                       const QColor& userColor, QWidget* parent = nullptr,
                       bool completed = false, bool hasPending = false,
                       bool suspended = false);

 signals:
  void settleClicked(const QString& batchId);
  void cardClicked(const QString& batchId);

 protected:
  void mousePressEvent(QMouseEvent* event) override;

 private:
  QString batchId_;
  bool settled_ = false;
};

/// Horizontal scrollable panel of receipt cards for active users.
/// Replaces the old StatisticsPanel + UserTaskPanel combo with a
/// glanceable card stream: order-id, user docs, pages, price, settle.
class ReceiptPreviewPanel : public QWidget
{
  Q_OBJECT

 public:
  explicit ReceiptPreviewPanel(QWidget* parent = nullptr);

  /// Remove all cards.
  void clearCards();

  /// Add one receipt card.  Cards are laid out left-to-right.
  /// @param completed  If true, renders as a grayed-out placeholder
  ///                   with a checkmark instead of a settle button.
  void addCard(const QString& batchId, const QString& displayName,
               const QString& firstDoc, const QString& lastDoc,
               int totalPages, double totalPrice,
               const QColor& userColor, bool completed = false,
               bool hasPending = false, bool suspended = false);

  /// True when at least one card is present.
  [[nodiscard]] bool hasCards() const;

 signals:
  void settleRequested(const QString& batchId);
  void cardClicked(const QString& batchId);

 private:
  QWidget* cardsContainer_ = nullptr;
  QHBoxLayout* cardsLayout_ = nullptr;
  QLabel* emptyLabel_ = nullptr;
  QScrollArea* scrollArea_ = nullptr;
};

}  // namespace wpce::ui
