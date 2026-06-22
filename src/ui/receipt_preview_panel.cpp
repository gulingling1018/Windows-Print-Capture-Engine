#include "ui/receipt_preview_panel.h"

#include "ui/user_color_palette.h"

#include <QApplication>
#include <QFont>
#include <QFrame>
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

ReceiptCard::ReceiptCard(const QString& taskId, const QString& userId,
                         const QString& firstDoc, const QString& lastDoc,
                         int totalPages, double totalPrice,
                         const QColor& userColor, QWidget* parent,
                         bool completed)
    : QFrame(parent), userId_(userId)
{
  setObjectName(QStringLiteral("ReceiptCard"));
  setFixedSize(220, 130);
  setFrameShape(QFrame::NoFrame);

  const bool dimmed = completed;

  // Rounded border via stylesheet (lightweight).
  const QColor border = dimmed ? QColor(0xc0, 0xc0, 0xc0) : userColor.darker(120);
  const QColor bg = dimmed ? QColor(0xf8, 0xf8, 0xf8) : QColor(0xff, 0xff, 0xff);
  const QString sheet = QStringLiteral(
    "QFrame#ReceiptCard {"
    "  background: %1;"
    "  border: 2px solid %2;"
    "  border-radius: 10px;"
    "}").arg(bg.name()).arg(border.name());
  setStyleSheet(sheet);

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(12, 10, 12, 10);
  root->setSpacing(4);

  // ── Top row: task id + user initial avatar ───────────────────────
  auto* topRow = new QHBoxLayout();
  topRow->setContentsMargins(0, 0, 0, 0);
  topRow->setSpacing(8);

  // Avatar circle with user initial or checkmark.
  auto* avatar = new QLabel(dimmed ? QStringLiteral("✓") : (userId.isEmpty() ? QStringLiteral("?") : userId.at(0).toUpper()));
  avatar->setFixedSize(26, 26);
  avatar->setAlignment(Qt::AlignCenter);
  avatar->setStyleSheet(dimmed
    ? QStringLiteral("background: #16a34a; color: white; border-radius: 13px; font-weight: bold; font-size: 13px;")
    : QStringLiteral("background: %1; color: white; border-radius: 13px; font-weight: bold; font-size: 11px;").arg(userColor.name()));

  auto* idLabel = new QLabel(QStringLiteral("#%1").arg(taskId));
  idLabel->setObjectName(QStringLiteral("ReceiptId"));
  QFont idFont;
  idFont.setPointSize(8);
  idFont.setWeight(QFont::DemiBold);
  idLabel->setFont(idFont);
  idLabel->setStyleSheet(dimmed ? QStringLiteral("color: #cbd5e1;") : QStringLiteral("color: #94a3b8;"));

  topRow->addWidget(avatar);
  topRow->addWidget(idLabel, 1);
  root->addLayout(topRow);

  // ── User name ─────────────────────────────────────────────────────
  auto* nameLabel = new QLabel(dimmed ? QStringLiteral("%1 ✓").arg(userId) : userId);
  nameLabel->setObjectName(QStringLiteral("ReceiptUser"));
  QFont nameFont;
  nameFont.setPointSize(12);
  nameFont.setWeight(QFont::DemiBold);
  nameLabel->setFont(nameFont);
  nameLabel->setStyleSheet(dimmed ? QStringLiteral("color: #94a3b8;") : QStringLiteral("color: #0f172a;"));
  nameLabel->setWordWrap(false);
  root->addWidget(nameLabel);

  // ── First ~ Last document names ───────────────────────────────────
  QString docHint;
  if (!firstDoc.isEmpty() && !lastDoc.isEmpty() && firstDoc != lastDoc)
    docHint = QStringLiteral("《%1》~《%2》").arg(firstDoc).arg(lastDoc);
  else if (!firstDoc.isEmpty())
    docHint = QStringLiteral("《%1》").arg(firstDoc);
  else
    docHint = QStringLiteral("—");

  auto* docLabel = new QLabel(docHint);
  docLabel->setObjectName(QStringLiteral("ReceiptDocs"));
  QFont docFont;
  docFont.setPointSize(8);
  docLabel->setFont(docFont);
  docLabel->setStyleSheet(dimmed ? QStringLiteral("color: #cbd5e1;") : QStringLiteral("color: #64748b;"));
  docLabel->setWordWrap(false);
  root->addWidget(docLabel);

  // ── Divider ───────────────────────────────────────────────────────
  auto* divider = new QFrame();
  divider->setFrameShape(QFrame::HLine);
  divider->setStyleSheet(QStringLiteral("color: %1;").arg(dimmed ? "#e5e5e5" : "#e2e8f0"));
  root->addWidget(divider);

  // ── Stats row: pages + price ──────────────────────────────────────
  auto* statsRow = new QHBoxLayout();
  statsRow->setContentsMargins(0, 0, 0, 0);
  statsRow->setSpacing(10);

  const QString statsColor = dimmed ? QStringLiteral("color: #cbd5e1; font-size: 10px;")
                                    : QStringLiteral("color: #334155; font-size: 10px;");
  const QString priceColor = dimmed ? QStringLiteral("color: #9ca3af; font-size: 11px; font-weight: bold;")
                                    : QStringLiteral("color: #dc2626; font-size: 11px; font-weight: bold;");

  auto* pagesLabel = new QLabel(QStringLiteral("📄 %1页").arg(totalPages));
  pagesLabel->setStyleSheet(statsColor);

  auto* priceLabel = new QLabel(QStringLiteral("💰 ¥%1").arg(totalPrice, 0, 'f', 2));
  priceLabel->setStyleSheet(priceColor);

  statsRow->addWidget(pagesLabel);
  statsRow->addStretch();
  statsRow->addWidget(priceLabel);
  root->addLayout(statsRow);

  // ── Settle button / completed badge ───────────────────────────────
  auto* btnRow = new QHBoxLayout();
  btnRow->setContentsMargins(0, 0, 0, 0);

  if (dimmed)
  {
    auto* badge = new QLabel(QStringLiteral("已结算"));
    badge->setAlignment(Qt::AlignCenter);
    badge->setStyleSheet(QStringLiteral(
      "background: #16a34a; color: white;"
      "  border-radius: 5px; padding: 3px 12px;"
      "  font-size: 10px; font-weight: 600;"));
    btnRow->addStretch();
    btnRow->addWidget(badge);
  }
  else
  {
    auto* settleBtn = new QPushButton(QStringLiteral("结算"));
    settleBtn->setCursor(Qt::PointingHandCursor);
    settleBtn->setStyleSheet(QStringLiteral(
      "QPushButton {"
      "  background: %1; color: white;"
      "  border: none; border-radius: 6px;"
      "  padding: 5px 14px; font-weight: 600; font-size: 11px;"
      "}"
      "QPushButton:hover { background: %2; }"
      "QPushButton:pressed { background: %3; }"
    ).arg(userColor.name())
     .arg(userColor.lighter(110).name())
     .arg(userColor.darker(110).name()));

    connect(settleBtn, &QPushButton::clicked, this, [this]() {
      emit settleClicked(userId_);
    });

    btnRow->addStretch();
    btnRow->addWidget(settleBtn);
  }
  root->addLayout(btnRow);
}

// ── ReceiptPreviewPanel ──────────────────────────────────────────────

ReceiptPreviewPanel::ReceiptPreviewPanel(QWidget* parent) : QWidget(parent)
{
  setObjectName(QStringLiteral("ReceiptPreviewPanel"));
  setMinimumHeight(154);
  setMaximumHeight(180);
  setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(16, 8, 16, 8);
  root->setSpacing(6);

  // Header.
  auto* headerRow = new QHBoxLayout();
  headerRow->setContentsMargins(0, 0, 0, 0);

  auto* title = new QLabel(QStringLiteral("用户订单预览"));
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
  scrollArea_->setFixedHeight(136);

  cardsContainer_ = new QWidget();
  cardsContainer_->setObjectName(QStringLiteral("ReceiptCardsContainer"));
  cardsContainer_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
  cardsContainer_->setFixedHeight(136);

  cardsLayout_ = new QHBoxLayout(cardsContainer_);
  cardsLayout_->setContentsMargins(0, 0, 0, 0);
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

void ReceiptPreviewPanel::addCard(const QString& taskId, const QString& userId,
                                  const QString& firstDoc, const QString& lastDoc,
                                  int totalPages, double totalPrice,
                                  const QColor& userColor, bool completed)
{
  // Remove empty label if present.
  if (emptyLabel_ != nullptr)
  {
    cardsLayout_->removeWidget(emptyLabel_);
    emptyLabel_->deleteLater();
    emptyLabel_ = nullptr;
  }

  auto* card = new ReceiptCard(taskId, userId, firstDoc, lastDoc,
                               totalPages, totalPrice, userColor,
                               cardsContainer_, completed);

  if (!completed)
  {
    connect(card, &ReceiptCard::settleClicked,
            this, &ReceiptPreviewPanel::settleRequested);
  }

  // Insert before the stretch.
  cardsLayout_->insertWidget(cardsLayout_->count() - 1, card);
}

bool ReceiptPreviewPanel::hasCards() const
{
  return emptyLabel_ == nullptr;
}

}  // namespace wpce::ui
