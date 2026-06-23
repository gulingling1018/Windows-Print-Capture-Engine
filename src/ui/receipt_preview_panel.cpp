#include "ui/receipt_preview_panel.h"

#include "ui/user_color_palette.h"

#include <QApplication>
#include <QFont>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace wpce::ui
{

// ── ReceiptCard ──────────────────────────────────────────────────────

ReceiptCard::ReceiptCard(const QString& batchId, const QString& displayName,
                         const QString& firstDoc, const QString& lastDoc,
                         int totalPages, double totalPrice,
                         const QColor& /*userColor*/, QWidget* parent,
                         bool completed, bool hasPending, bool suspended)
    : QFrame(parent), batchId_(batchId), settled_(completed)
{
  setObjectName(QStringLiteral("ReceiptCard"));
  setFixedSize(240, 120);
  setFrameShape(QFrame::NoFrame);
  setCursor(completed ? Qt::ArrowCursor : Qt::PointingHandCursor);

  const bool dimmed = completed;

  // Top accent border color based on state.
  QString accentColor;
  if (dimmed)
    accentColor = QStringLiteral("#d1d5db");
  else if (suspended)
    accentColor = QStringLiteral("#f59e0b");
  else
    accentColor = QStringLiteral("#2563eb");

  QColor border = dimmed ? QColor(0xe2, 0xe8, 0xf0) : QColor(0xe2, 0xe8, 0xf0);
  if (hasPending && !dimmed)
    border = QColor(0xf5, 0x9e, 0x0b);

  const QColor bg = dimmed ? QColor(0xf9, 0xfa, 0xfb) : QColor(0xff, 0xff, 0xff);

  const QString sheet = QStringLiteral(
    "QFrame#ReceiptCard {"
    "  background: %1;"
    "  border: 1px solid %2;"
    "  border-top: 3px solid %3;"
    "  border-radius: 10px;"
    "}"
    "QFrame#ReceiptCard:hover {"
    "  border-color: %4;"
    "  background: %5;"
    "}")
    .arg(bg.name())
    .arg(border.name())
    .arg(accentColor)
    .arg(dimmed ? border.name() : QStringLiteral("#94a3b8"))
    .arg(dimmed ? bg.name() : QStringLiteral("#f8fafc"));
  setStyleSheet(sheet);

  if (!dimmed)
  {
    auto* shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(8);
    shadow->setOffset(0, 2);
    shadow->setColor(QColor(0, 0, 0, 25));
    setGraphicsEffect(shadow);
  }

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(12, 8, 12, 8);
  root->setSpacing(3);

  // ── Top row: status indicator + display name ─────────────────────
  auto* topRow = new QHBoxLayout();
  topRow->setContentsMargins(0, 0, 0, 0);
  topRow->setSpacing(6);

  if (!dimmed)
  {
    auto* statusDot = new QLabel();
    if (suspended)
    {
      statusDot->setText(QStringLiteral("⏸"));
      statusDot->setStyleSheet(QStringLiteral("color: #f59e0b; font-size: 10px;"));
      statusDot->setToolTip(QStringLiteral("已挂单"));
    }
    else
    {
      statusDot->setText(QStringLiteral("●"));
      statusDot->setStyleSheet(QStringLiteral("color: #2563eb; font-size: 9px;"));
      statusDot->setToolTip(QStringLiteral("监听中"));
    }
    topRow->addWidget(statusDot);
  }

  auto* nameLabel = new QLabel(displayName);
  nameLabel->setObjectName(QStringLiteral("ReceiptName"));
  QFont nameFont;
  nameFont.setPointSize(10);
  nameFont.setWeight(QFont::Bold);
  nameLabel->setFont(nameFont);
  nameLabel->setStyleSheet(dimmed ? QStringLiteral("color: #9ca3af;")
                                  : QStringLiteral("color: #0f172a;"));
  topRow->addWidget(nameLabel, 1);
  root->addLayout(topRow);

  // ── Document range ────────────────────────────────────────────────
  QString docHint;
  if (!firstDoc.isEmpty() && !lastDoc.isEmpty() && firstDoc != lastDoc)
    docHint = QStringLiteral("%1 ~ %2").arg(firstDoc.left(12)).arg(lastDoc.left(12));
  else if (!firstDoc.isEmpty())
    docHint = firstDoc.left(24);
  else
    docHint = QStringLiteral("—");

  auto* docLabel = new QLabel(docHint);
  docLabel->setObjectName(QStringLiteral("ReceiptDocs"));
  QFont docFont;
  docFont.setPointSize(8);
  docLabel->setFont(docFont);
  docLabel->setStyleSheet(dimmed ? QStringLiteral("color: #d1d5db;")
                                 : QStringLiteral("color: #64748b;"));
  docLabel->setWordWrap(false);
  root->addWidget(docLabel);

  // ── Divider ───────────────────────────────────────────────────────
  auto* divider = new QFrame();
  divider->setFrameShape(QFrame::HLine);
  divider->setFixedHeight(1);
  divider->setStyleSheet(QStringLiteral("background: %1; border: none;")
    .arg(dimmed ? QStringLiteral("#f3f4f6") : QStringLiteral("#e2e8f0")));
  root->addWidget(divider);

  // ── Stats row: pages + price ──────────────────────────────────────
  auto* statsRow = new QHBoxLayout();
  statsRow->setContentsMargins(0, 2, 0, 2);
  statsRow->setSpacing(10);

  const QString statsColor = dimmed ? QStringLiteral("color: #d1d5db; font-size: 11px;")
                                    : QStringLiteral("color: #334155; font-size: 11px;");
  const QString priceColor = dimmed ? QStringLiteral("color: #9ca3af; font-size: 13px; font-weight: bold;")
                                    : QStringLiteral("color: #dc2626; font-size: 13px; font-weight: bold;");

  auto* pagesLabel = new QLabel(QStringLiteral("%1 页").arg(totalPages));
  pagesLabel->setStyleSheet(statsColor);

  auto* priceLabel = new QLabel(QStringLiteral("¥%1").arg(totalPrice, 0, 'f', 2));
  priceLabel->setStyleSheet(priceColor);

  statsRow->addWidget(pagesLabel);
  statsRow->addStretch();
  statsRow->addWidget(priceLabel);
  root->addLayout(statsRow);

  // ── Bottom: settle button / completed badge ───────────────────────
  auto* btnRow = new QHBoxLayout();
  btnRow->setContentsMargins(0, 0, 0, 0);

  if (dimmed)
  {
    auto* badge = new QLabel(QStringLiteral("✓ 已结算"));
    badge->setAlignment(Qt::AlignCenter);
    badge->setStyleSheet(QStringLiteral(
      "background: #dcfce7; color: #166534;"
      "  border-radius: 4px; padding: 3px 10px;"
      "  font-size: 10px; font-weight: 600;"));
    btnRow->addStretch();
    btnRow->addWidget(badge);
  }
  else
  {
    if (hasPending)
    {
      auto* pendingTag = new QLabel(QStringLiteral("⚠ 待确认"));
      pendingTag->setStyleSheet(QStringLiteral(
        "color: #92400e; font-size: 9px; font-weight: 600;"
        "background: #fffbeb; border-radius: 3px; padding: 1px 5px;"));
      btnRow->addWidget(pendingTag);
    }
    btnRow->addStretch();

    auto* settleBtn = new QPushButton(QStringLiteral("结算"));
    settleBtn->setObjectName(QStringLiteral("CardSettleBtn"));
    settleBtn->setCursor(Qt::PointingHandCursor);
    settleBtn->setStyleSheet(QStringLiteral(
      "QPushButton#CardSettleBtn {"
      "  background: #2563eb; color: white;"
      "  border: none; border-radius: 5px;"
      "  padding: 4px 14px; font-weight: 600; font-size: 11px;"
      "}"
      "QPushButton#CardSettleBtn:hover { background: #1d4ed8; }"
      "QPushButton#CardSettleBtn:pressed { background: #1e40af; }"));

    connect(settleBtn, &QPushButton::clicked, this, [this]() {
      emit settleClicked(batchId_);
    });

    btnRow->addWidget(settleBtn);
  }
  root->addLayout(btnRow);
}

void ReceiptCard::mousePressEvent(QMouseEvent* event)
{
  if (settled_)
  {
    QFrame::mousePressEvent(event);
    return;
  }
  // Only emit cardClicked for clicks NOT on the settle button.
  QWidget* child = childAt(event->pos());
  if (child != nullptr && child->objectName() == QStringLiteral("CardSettleBtn"))
  {
    QFrame::mousePressEvent(event);
    return;
  }
  emit cardClicked(batchId_);
}

// ── ReceiptPreviewPanel ──────────────────────────────────────────────

ReceiptPreviewPanel::ReceiptPreviewPanel(QWidget* parent) : QWidget(parent)
{
  setObjectName(QStringLiteral("ReceiptPreviewPanel"));
  setMinimumHeight(148);
  setMaximumHeight(168);
  setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(16, 8, 16, 8);
  root->setSpacing(6);

  // Header.
  auto* headerRow = new QHBoxLayout();
  headerRow->setContentsMargins(0, 0, 0, 0);

  auto* title = new QLabel(QStringLiteral("用户订单"));
  title->setObjectName(QStringLiteral("PanelHeader"));
  QFont tf;
  tf.setPointSize(11);
  tf.setWeight(QFont::DemiBold);
  title->setFont(tf);
  title->setStyleSheet(QStringLiteral("color: #0f172a;"));

  headerRow->addWidget(title);
  headerRow->addStretch();
  root->addLayout(headerRow);

  // Scroll area for cards.
  scrollArea_ = new QScrollArea(this);
  scrollArea_->setObjectName(QStringLiteral("ReceiptScroll"));
  scrollArea_->setWidgetResizable(true);
  scrollArea_->setFrameShape(QFrame::NoFrame);
  scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  scrollArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scrollArea_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  scrollArea_->setFixedHeight(128);

  cardsContainer_ = new QWidget();
  cardsContainer_->setObjectName(QStringLiteral("ReceiptCardsContainer"));
  cardsContainer_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
  cardsContainer_->setFixedHeight(128);

  cardsLayout_ = new QHBoxLayout(cardsContainer_);
  cardsLayout_->setContentsMargins(4, 4, 4, 4);
  cardsLayout_->setSpacing(12);
  cardsLayout_->addStretch();

  emptyLabel_ = new QLabel(QStringLiteral("暂无待结算订单"));
  emptyLabel_->setAlignment(Qt::AlignCenter);
  emptyLabel_->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 12px;"));
  cardsLayout_->insertWidget(0, emptyLabel_);

  scrollArea_->setWidget(cardsContainer_);
  root->addWidget(scrollArea_);
}

void ReceiptPreviewPanel::clearCards()
{
  QLayoutItem* item = nullptr;
  while ((item = cardsLayout_->takeAt(0)) != nullptr)
  {
    if (item->widget() != nullptr)
    {
      item->widget()->deleteLater();
    }
    delete item;
  }
  cardsLayout_->addStretch();

  emptyLabel_ = new QLabel(QStringLiteral("暂无待结算订单"));
  emptyLabel_->setAlignment(Qt::AlignCenter);
  emptyLabel_->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 12px;"));
  cardsLayout_->insertWidget(0, emptyLabel_);
}

void ReceiptPreviewPanel::addCard(const QString& batchId, const QString& displayName,
                                  const QString& firstDoc, const QString& lastDoc,
                                  int totalPages, double totalPrice,
                                  const QColor& userColor, bool completed,
                                  bool hasPending, bool suspended)
{
  // Remove empty label if present.
  if (emptyLabel_ != nullptr)
  {
    cardsLayout_->removeWidget(emptyLabel_);
    emptyLabel_->deleteLater();
    emptyLabel_ = nullptr;
  }

  auto* card = new ReceiptCard(batchId, displayName, firstDoc, lastDoc,
                               totalPages, totalPrice, userColor,
                               cardsContainer_, completed, hasPending, suspended);

  if (!completed)
  {
    connect(card, &ReceiptCard::settleClicked,
            this, &ReceiptPreviewPanel::settleRequested);
    connect(card, &ReceiptCard::cardClicked,
            this, &ReceiptPreviewPanel::cardClicked);
  }

  // Insert before the stretch.
  cardsLayout_->insertWidget(cardsLayout_->count() - 1, card);
}

bool ReceiptPreviewPanel::hasCards() const
{
  return emptyLabel_ == nullptr;
}

}  // namespace wpce::ui
