/// @file completion_dialog.cpp
/// Completion dialog implementation.

#include "ui/completion_dialog.h"

#include <QDialogButtonBox>
#include <QFrame>
#include <QHeaderView>
#include <QLabel>
#include <QVBoxLayout>

#include <iomanip>
#include <sstream>

namespace wpce::ui
{

CompletionDialog::CompletionDialog(const QString& userId,
                                     const std::vector<JobCostLine>& lines,
                                     const PricingConfig& pricing,
                                     QWidget* parent)
  : QDialog(parent)
{
  Q_UNUSED(pricing);
  buildUi(userId, lines, pricing);
}

void CompletionDialog::buildUi(const QString& userId,
                                const std::vector<JobCostLine>& lines,
                                const PricingConfig& /*pricing*/)
{
  setWindowTitle(QStringLiteral("结算 — %1").arg(userId));
  setMinimumSize(520, 360);
  resize(560, 420);

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(20, 20, 20, 20);
  root->setSpacing(14);

  // ── Title ──────────────────────────────────────────────────────────
  auto* title = new QLabel(QStringLiteral("结算 — %1").arg(userId));
  QFont titleFont;
  titleFont.setPointSize(14);
  titleFont.setWeight(QFont::DemiBold);
  title->setFont(titleFont);
  root->addWidget(title);

  auto* divider = new QFrame();
  divider->setFrameShape(QFrame::HLine);
  divider->setStyleSheet(QStringLiteral("background: #e2e8f0; max-height: 1px;"));
  root->addWidget(divider);

  // ── Job detail table ───────────────────────────────────────────────
  auto* detailLabel = new QLabel(QStringLiteral("作业明细:"));
  QFont sectionFont;
  sectionFont.setWeight(QFont::DemiBold);
  sectionFont.setPointSize(10);
  detailLabel->setFont(sectionFont);
  root->addWidget(detailLabel);

  table_ = new QTableWidget(static_cast<int>(lines.size()), 5, this);
  table_->setHorizontalHeaderLabels({
    QStringLiteral("文档"), QStringLiteral("页数"),
    QStringLiteral("颜色"), QStringLiteral("双面"),
    QStringLiteral("金额 (¥)") });
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setAlternatingRowColors(true);
  table_->setShowGrid(false);
  table_->verticalHeader()->hide();
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
  table_->setStyleSheet(QStringLiteral(
    "QTableWidget { background: white; border: 1px solid #e2e8f0; border-radius: 6px; }"
    "QTableWidget::item { padding: 6px 10px; }"
    "QHeaderView::section { background: #f1f5f9; color: #475569; font-weight: bold;"
    "  padding: 7px 10px; border-bottom: 2px solid #cbd5e1; }"));

  double grandTotal = 0.0;
  int grandPages = 0;

  for (std::size_t i = 0; i < lines.size(); ++i)
  {
    const auto& line = lines[i];

    auto* docItem = new QTableWidgetItem(line.documentName);
    docItem->setData(Qt::UserRole, line.pages);

    auto* pagesItem = new QTableWidgetItem(QString::number(line.pages));
    pagesItem->setTextAlignment(Qt::AlignCenter);

    auto* colorItem = new QTableWidgetItem(line.colorMode);
    colorItem->setTextAlignment(Qt::AlignCenter);

    auto* duplexItem = new QTableWidgetItem(line.duplexMode);
    duplexItem->setTextAlignment(Qt::AlignCenter);

    std::ostringstream priceStr;
    priceStr << std::fixed << std::setprecision(2) << line.price;
    auto* priceItem = new QTableWidgetItem(QString::fromStdString(priceStr.str()));
    priceItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    QFont monoFont(QStringLiteral("Cascadia Mono"));
    monoFont.setStyleHint(QFont::Monospace);
    priceItem->setFont(monoFont);
    if (line.price > 0)
      priceItem->setForeground(QColor(0x06, 0x76, 0x47));

    table_->setItem(static_cast<int>(i), 0, docItem);
    table_->setItem(static_cast<int>(i), 1, pagesItem);
    table_->setItem(static_cast<int>(i), 2, colorItem);
    table_->setItem(static_cast<int>(i), 3, duplexItem);
    table_->setItem(static_cast<int>(i), 4, priceItem);

    grandTotal += line.price;
    grandPages += line.pages;
  }

  root->addWidget(table_, 1);

  // ── Totals row ─────────────────────────────────────────────────────
  auto* totalFrame = new QFrame();
  totalFrame->setStyleSheet(QStringLiteral(
    "QFrame#TotalFrame { background: #f0fdf4; border: 1px solid #bbf7d0;"
    "  border-radius: 8px; padding: 12px; }"));
  totalFrame->setObjectName(QStringLiteral("TotalFrame"));
  auto* totalLayout = new QVBoxLayout(totalFrame);
  totalLayout->setContentsMargins(14, 10, 14, 10);
  totalLayout->setSpacing(4);

  std::ostringstream totalStr;
  totalStr << std::fixed << std::setprecision(2) << grandTotal;
  totalLabel_ = new QLabel(
    QStringLiteral("合计: %1 页    金额: ¥%2")
      .arg(grandPages)
      .arg(QString::fromStdString(totalStr.str())));
  QFont totalFont;
  totalFont.setPointSize(13);
  totalFont.setWeight(QFont::DemiBold);
  totalLabel_->setFont(totalFont);
  totalLabel_->setStyleSheet(QStringLiteral("color: #166534;"));
  totalLayout->addWidget(totalLabel_);

  root->addWidget(totalFrame);

  // ── Buttons ────────────────────────────────────────────────────────
  auto* buttonLayout = new QHBoxLayout();
  buttonLayout->addStretch();

  cancelBtn_ = new QPushButton(QStringLiteral("取消"));
  cancelBtn_->setMinimumHeight(36);
  cancelBtn_->setStyleSheet(QStringLiteral(
    "QPushButton { background: white; border: 1px solid #cbd5e1; border-radius: 6px;"
    "  padding: 6px 24px; color: #475569; font-weight: 600; }"
    "QPushButton:hover { background: #f8fafc; }"));

  confirmBtn_ = new QPushButton(QStringLiteral("确认结算"));
  confirmBtn_->setMinimumHeight(36);
  confirmBtn_->setStyleSheet(QStringLiteral(
    "QPushButton { background: #2563eb; border: none; border-radius: 6px;"
    "  padding: 6px 32px; color: white; font-weight: 700; }"
    "QPushButton:hover { background: #1d4ed8; }"));

  buttonLayout->addWidget(cancelBtn_);
  buttonLayout->addWidget(confirmBtn_);
  root->addLayout(buttonLayout);

  connect(confirmBtn_, &QPushButton::clicked, this, [this]() {
    confirmed_ = true;
    accept();
  });
  connect(cancelBtn_, &QPushButton::clicked, this, &QDialog::reject);
}

}  // namespace wpce::ui
