#include "ui/history_explorer_dialog.h"
#include "bridge/settlement_bridge.h"
#include "common/business_clock.h"
#include "domain/billing_batch_repository.h"

#include <QDateTime>
#include <QDir>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <filesystem>

namespace wpce::ui
{

namespace
{

QDate businessToday()
{
  return QDate::fromString(
      QString::fromStdString(common::current_business_date().yyyy_mm_dd),
      QStringLiteral("yyyy-MM-dd"));
}

}  // namespace

HistoryExplorerDialog::HistoryExplorerDialog(const QString& historyRoot,
                                             QWidget* parent)
  : QDialog(parent), historyRoot_(historyRoot), currentDate_(businessToday())
{
  setWindowTitle(QStringLiteral("结算历史"));
  setMinimumSize(900, 560);
  resize(1000, 650);
  buildUi();
}

void HistoryExplorerDialog::buildUi()
{
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(16, 16, 16, 16);
  root->setSpacing(10);

  // ── Header row ──────────────────────────────────────────────────────
  auto* headerRow = new QHBoxLayout();
  headerRow->setSpacing(10);

  auto* title = new QLabel(QStringLiteral("结算历史"));
  title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: bold; color: #0f172a;"));
  headerRow->addWidget(title);
  headerRow->addStretch(1);

  searchEdit_ = new QLineEdit();
  searchEdit_->setPlaceholderText(QStringLiteral("搜索: 订单号/文档名..."));
  searchEdit_->setClearButtonEnabled(true);
  searchEdit_->setMinimumWidth(200);
  searchEdit_->setMaximumWidth(280);
  connect(searchEdit_, &QLineEdit::textChanged, this, &HistoryExplorerDialog::onFilterChanged);
  headerRow->addWidget(searchEdit_);

  auto* refreshBtn = new QPushButton(QStringLiteral("刷新"));
  refreshBtn->setFixedWidth(60);
  connect(refreshBtn, &QPushButton::clicked, this, [this]() { loadOrders(); });
  headerRow->addWidget(refreshBtn);
  root->addLayout(headerRow);

  // ── Date navigation bar ─────────────────────────────────────────────
  auto* dateBar = new QHBoxLayout();
  dateBar->setSpacing(8);

  prevDateBtn_ = new QPushButton(QStringLiteral("◀"));
  prevDateBtn_->setFixedSize(36, 32);
  prevDateBtn_->setToolTip(QStringLiteral("前一天"));
  prevDateBtn_->setStyleSheet(QStringLiteral(
    "QPushButton { background: white; border: 1px solid #cbd5e1; border-radius: 6px;"
    "  font-size: 14px; color: #475569; }"
    "QPushButton:hover { background: #f1f5f9; }"
    "QPushButton:disabled { color: #d1d5db; }"));
  connect(prevDateBtn_, &QPushButton::clicked, this, [this]() { navigateDate(-1); });

  dateLabel_ = new QLabel();
  dateLabel_->setAlignment(Qt::AlignCenter);
  dateLabel_->setMinimumWidth(140);
  dateLabel_->setStyleSheet(QStringLiteral(
    "font-size: 14px; font-weight: 600; color: #1e293b;"
    "background: #f8fafc; border: 1px solid #e2e8f0; border-radius: 6px;"
    "padding: 4px 12px;"));

  nextDateBtn_ = new QPushButton(QStringLiteral("▶"));
  nextDateBtn_->setFixedSize(36, 32);
  nextDateBtn_->setToolTip(QStringLiteral("后一天"));
  nextDateBtn_->setStyleSheet(prevDateBtn_->styleSheet());
  connect(nextDateBtn_, &QPushButton::clicked, this, [this]() { navigateDate(1); });

  todayBtn_ = new QPushButton(QStringLiteral("今天"));
  todayBtn_->setFixedWidth(56);
  todayBtn_->setStyleSheet(QStringLiteral(
    "QPushButton { background: #2563eb; color: white; border: none; border-radius: 6px;"
    "  font-size: 12px; font-weight: 600; padding: 6px 10px; }"
    "QPushButton:hover { background: #1d4ed8; }"));
  connect(todayBtn_, &QPushButton::clicked, this, [this]() { goToToday(); });

  summaryLabel_ = new QLabel();
  summaryLabel_->setStyleSheet(QStringLiteral(
    "color: #475569; font-size: 12px; font-weight: 500;"
    "background: #f0fdf4; border: 1px solid #bbf7d0; border-radius: 6px;"
    "padding: 4px 10px;"));

  dateBar->addWidget(prevDateBtn_);
  dateBar->addWidget(dateLabel_);
  dateBar->addWidget(nextDateBtn_);
  dateBar->addWidget(todayBtn_);
  dateBar->addStretch(1);
  dateBar->addWidget(summaryLabel_);
  root->addLayout(dateBar);

  // ── Order table ─────────────────────────────────────────────────────
  orderTable_ = new QTableWidget(0, 6);
  orderTable_->setHorizontalHeaderLabels({
    QStringLiteral("订单号"), QStringLiteral("用户"),
    QStringLiteral("首文档"), QStringLiteral("尾文档"),
    QStringLiteral("结算时间"), QStringLiteral("金额") });
  orderTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
  orderTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  orderTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
  orderTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
  orderTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
  orderTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
  orderTable_->setColumnWidth(0, 180);
  orderTable_->verticalHeader()->hide();
  orderTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
  orderTable_->setSelectionMode(QAbstractItemView::SingleSelection);
  orderTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  orderTable_->setAlternatingRowColors(true);
  orderTable_->setStyleSheet(QStringLiteral(
    "QTableWidget { border: 1px solid #e2e8f0; border-radius: 6px; }"
    "QTableWidget::item { padding: 4px 8px; }"
    "QHeaderView::section { background: #f1f5f9; color: #475569; font-weight: bold;"
    "  padding: 6px 8px; border: none; border-bottom: 1px solid #cbd5e1; }"));
  connect(orderTable_, &QTableWidget::itemSelectionChanged,
          this, &HistoryExplorerDialog::onSelectionChanged);
  root->addWidget(orderTable_, 2);

  // ── Detail table ────────────────────────────────────────────────────
  auto* detailLabel = new QLabel(QStringLiteral("作业明细"));
  detailLabel->setStyleSheet(QStringLiteral("font-size: 13px; font-weight: bold; color: #334155;"));
  root->addWidget(detailLabel);

  detailTable_ = new QTableWidget(0, 6);
  detailTable_->setHorizontalHeaderLabels({
    QStringLiteral("作业ID"), QStringLiteral("页数"),
    QStringLiteral("份数"), QStringLiteral("纸张数"),
    QStringLiteral("匹配规则"), QStringLiteral("金额") });
  detailTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  detailTable_->verticalHeader()->hide();
  detailTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  detailTable_->setAlternatingRowColors(true);
  detailTable_->setStyleSheet(orderTable_->styleSheet());
  root->addWidget(detailTable_, 1);

  updateDateDisplay();
}

void HistoryExplorerDialog::loadOrders()
{
  std::filesystem::path historyRoot(historyRoot_.toStdWString());
  availableDates_ = bridge::SettlementBridge::listDateDirs(historyRoot);
  const auto legacyDates = bridge::SettlementBridge::listDateDirs(historyRoot / "history");
  availableDates_.insert(availableDates_.end(), legacyDates.begin(), legacyDates.end());
  std::sort(availableDates_.begin(), availableDates_.end());
  availableDates_.erase(std::unique(availableDates_.begin(), availableDates_.end()),
                        availableDates_.end());
  loadOrdersForCurrentDate();
}

void HistoryExplorerDialog::navigateDate(int delta)
{
  QDate newDate = currentDate_.addDays(delta);
  if (newDate > businessToday()) return;
  currentDate_ = newDate;
  updateDateDisplay();
  loadOrdersForCurrentDate();
}

void HistoryExplorerDialog::goToToday()
{
  currentDate_ = businessToday();
  updateDateDisplay();
  loadOrdersForCurrentDate();
}

void HistoryExplorerDialog::updateDateDisplay()
{
  dateLabel_->setText(currentDate_.toString(QStringLiteral("yyyy-MM-dd")));
  nextDateBtn_->setEnabled(currentDate_ < businessToday());
  todayBtn_->setEnabled(currentDate_ != businessToday());
}

void HistoryExplorerDialog::loadOrdersForCurrentDate()
{
  orders_.clear();
  orderTable_->setRowCount(0);
  detailTable_->setRowCount(0);

  std::filesystem::path historyRoot(historyRoot_.toStdWString());
  std::string dateStr = currentDate_.toString(QStringLiteral("yyyy-MM-dd")).toStdString();

  domain::BillingBatchRepository repository;
  repository.setFilePath(historyRoot / dateStr / "orders.json");
  std::vector<core::SettlementOrder> settlementOrders;
  (void)repository.loadOrders(settlementOrders);
  if (settlementOrders.empty())
  {
    // Read-only fallback for pre-v4 installations. New settlements are never
    // written here.
    settlementOrders = bridge::SettlementBridge::loadOrdersForDate(
        historyRoot / "history", dateStr);
  }

  for (auto& order : settlementOrders)
  {
    OrderRow row;
    row.orderId = QString::fromStdString(order.order_id);
    row.batchId = QString::fromStdString(order.batch_id);
    row.userId = QString::fromStdString(order.user_id);
    row.settledAt = QString::fromStdString(order.settled_at_utc);
    row.totalPrice = order.total_price;
    row.jobCount = static_cast<int>(order.job_ids.size());

    row.firstDoc = QString::fromStdString(order.first_document_name);
    row.lastDoc = QString::fromStdString(order.last_document_name);
    row.order = std::move(order);
    orders_.push_back(std::move(row));
  }

  // Update summary.
  double dayTotal = 0.0;
  for (const auto& r : orders_) dayTotal += r.totalPrice;
  summaryLabel_->setText(QStringLiteral("当日: %1 单 · ¥%2")
    .arg(orders_.size())
    .arg(QString::number(dayTotal, 'f', 2)));

  onFilterChanged();
}

void HistoryExplorerDialog::onFilterChanged()
{
  const QString filter = searchEdit_->text().trimmed().toLower();
  orderTable_->setRowCount(0);

  for (std::size_t i = 0; i < orders_.size(); ++i)
  {
    const auto& row = orders_[i];
    if (!filter.isEmpty())
    {
      bool match = row.orderId.toLower().contains(filter) ||
                   row.userId.toLower().contains(filter) ||
                   row.firstDoc.toLower().contains(filter) ||
                   row.lastDoc.toLower().contains(filter);
      if (!match) continue;
    }

    int r = orderTable_->rowCount();
    orderTable_->insertRow(r);
    orderTable_->setItem(r, 0, new QTableWidgetItem(row.orderId));
    orderTable_->setItem(r, 1, new QTableWidgetItem(row.userId));
    orderTable_->setItem(r, 2, new QTableWidgetItem(row.firstDoc.isEmpty() ? QStringLiteral("-") : row.firstDoc));
    orderTable_->setItem(r, 3, new QTableWidgetItem(row.lastDoc.isEmpty() ? QStringLiteral("-") : row.lastDoc));

    QDateTime dt = QDateTime::fromString(row.settledAt, Qt::ISODate);
    QString timeStr = dt.isValid() ? dt.toString(QStringLiteral("HH:mm:ss")) : row.settledAt;
    orderTable_->setItem(r, 4, new QTableWidgetItem(timeStr));

    auto* priceItem = new QTableWidgetItem(
      QStringLiteral("¥%1").arg(QString::number(row.totalPrice, 'f', 2)));
    priceItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    orderTable_->setItem(r, 5, priceItem);

    // Store the index into orders_ for detail lookup.
    orderTable_->item(r, 0)->setData(Qt::UserRole, static_cast<int>(i));
  }
}

void HistoryExplorerDialog::onSelectionChanged()
{
  detailTable_->setRowCount(0);
  int row = orderTable_->currentRow();
  if (row < 0) return;

  QVariant idxVar = orderTable_->item(row, 0)->data(Qt::UserRole);
  if (!idxVar.isValid()) return;
  int orderIdx = idxVar.toInt();
  if (orderIdx < 0 || orderIdx >= static_cast<int>(orders_.size())) return;

  const auto& order = orders_[static_cast<std::size_t>(orderIdx)].order;

  for (const auto& entry : order.pricing_entries)
  {
    int r = detailTable_->rowCount();
    detailTable_->insertRow(r);
    detailTable_->setItem(r, 0, new QTableWidgetItem(QString::number(entry.job_id)));
    detailTable_->setItem(r, 1, new QTableWidgetItem(QString::number(entry.total_pages)));
    detailTable_->setItem(r, 2, new QTableWidgetItem(QString::number(entry.copies)));
    detailTable_->setItem(r, 3, new QTableWidgetItem(QString::number(entry.effective_sheets)));
    detailTable_->setItem(r, 4, new QTableWidgetItem(QString::fromStdString(entry.matched_rule_id)));
    auto* priceItem = new QTableWidgetItem(
      QStringLiteral("¥%1").arg(QString::number(entry.total_price, 'f', 2)));
    priceItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    detailTable_->setItem(r, 5, priceItem);
  }
}

}  // namespace wpce::ui
