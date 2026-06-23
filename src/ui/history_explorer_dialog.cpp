#include "ui/history_explorer_dialog.h"
#include "bridge/settlement_bridge.h"

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QStandardPaths>

namespace wpce::ui
{

HistoryExplorerDialog::HistoryExplorerDialog(const QString& stateDir,
                                               QWidget* parent)
  : QDialog(parent), stateDir_(stateDir)
{
  setWindowTitle(QStringLiteral("结算历史"));
  setMinimumSize(800, 500);
  resize(900, 600);
  buildUi();
  loadOrders();
}

void HistoryExplorerDialog::buildUi()
{
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(16, 16, 16, 16);
  root->setSpacing(10);

  auto* headerRow = new QHBoxLayout();
  auto* title = new QLabel(QStringLiteral("已结算订单历史"));
  title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: bold; color: #0f172a;"));
  headerRow->addWidget(title);
  headerRow->addStretch(1);

  filterEdit_ = new QLineEdit();
  filterEdit_->setPlaceholderText(QStringLiteral("筛选: 订单号/用户名..."));
  filterEdit_->setMinimumWidth(200);
  connect(filterEdit_, &QLineEdit::textChanged, this, &HistoryExplorerDialog::onFilterChanged);
  headerRow->addWidget(filterEdit_);

  auto* refreshBtn = new QPushButton(QStringLiteral("刷新"));
  connect(refreshBtn, &QPushButton::clicked, this, [this]() { loadOrders(); });
  headerRow->addWidget(refreshBtn);
  root->addLayout(headerRow);

  // Order list table.
  orderTable_ = new QTableWidget(0, 6);
  orderTable_->setHorizontalHeaderLabels({
    QStringLiteral("订单号"), QStringLiteral("批次号"),
    QStringLiteral("用户"), QStringLiteral("结算时间"),
    QStringLiteral("作业数"), QStringLiteral("金额") });
  orderTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  orderTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  orderTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  orderTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
  orderTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
  orderTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
  orderTable_->verticalHeader()->hide();
  orderTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
  orderTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  orderTable_->setAlternatingRowColors(true);
  connect(orderTable_, &QTableWidget::itemSelectionChanged,
          this, &HistoryExplorerDialog::onSelectionChanged);
  root->addWidget(orderTable_, 1);

  // Detail table.
  auto* detailLabel = new QLabel(QStringLiteral("作业明细"));
  detailLabel->setStyleSheet(QStringLiteral("font-size: 13px; font-weight: bold; color: #334155;"));
  root->addWidget(detailLabel);

  detailTable_ = new QTableWidget(0, 5);
  detailTable_->setHorizontalHeaderLabels({
    QStringLiteral("作业ID"), QStringLiteral("页数"),
    QStringLiteral("份数"), QStringLiteral("匹配规则"),
    QStringLiteral("金额") });
  detailTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  detailTable_->verticalHeader()->hide();
  detailTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  detailTable_->setAlternatingRowColors(true);
  root->addWidget(detailTable_, 1);
}

void HistoryExplorerDialog::loadOrders()
{
  orders_.clear();
  orderTable_->setRowCount(0);
  detailTable_->setRowCount(0);

  QDir dir(stateDir_ + QDir::separator() + QStringLiteral("settlement_orders"));
  if (!dir.exists()) return;

  const QStringList files = dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
  for (const QString& file : files)
  {
    const QString fullPath = dir.absoluteFilePath(file);
    auto order = wpce::bridge::SettlementBridge::loadFromFile(
      std::filesystem::path(fullPath.toStdWString()));
    if (!order.has_value()) continue;

    OrderRow row;
    row.orderId = QString::fromStdString(order->order_id);
    row.batchId = QString::fromStdString(order->batch_id);
    row.userId = QString::fromStdString(order->user_id);
    row.settledAt = QString::fromStdString(order->settled_at_utc);
    row.totalPrice = order->total_price;
    row.jobCount = static_cast<int>(order->job_ids.size());
    row.filePath = fullPath;
    orders_.push_back(std::move(row));
  }

  // Populate table.
  onFilterChanged();
}

void HistoryExplorerDialog::onFilterChanged()
{
  const QString filter = filterEdit_->text().trimmed().toLower();
  orderTable_->setRowCount(0);

  for (const auto& row : orders_)
  {
    if (!filter.isEmpty())
    {
      bool match = row.orderId.toLower().contains(filter) ||
                   row.userId.toLower().contains(filter) ||
                   row.batchId.toLower().contains(filter);
      if (!match) continue;
    }

    int r = orderTable_->rowCount();
    orderTable_->insertRow(r);
    orderTable_->setItem(r, 0, new QTableWidgetItem(row.orderId));
    orderTable_->setItem(r, 1, new QTableWidgetItem(row.batchId));
    orderTable_->setItem(r, 2, new QTableWidgetItem(row.userId));
    orderTable_->setItem(r, 3, new QTableWidgetItem(row.settledAt));
    orderTable_->setItem(r, 4, new QTableWidgetItem(QString::number(row.jobCount)));
    auto* priceItem = new QTableWidgetItem(QString::number(row.totalPrice, 'f', 2));
    priceItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    orderTable_->setItem(r, 5, priceItem);
  }
}

void HistoryExplorerDialog::onSelectionChanged()
{
  detailTable_->setRowCount(0);
  int row = orderTable_->currentRow();
  if (row < 0 || row >= static_cast<int>(orders_.size())) return;

  const auto& orderRow = orders_[static_cast<std::size_t>(row)];
  auto order = wpce::bridge::SettlementBridge::loadFromFile(
    std::filesystem::path(orderRow.filePath.toStdWString()));
  if (!order.has_value()) return;

  for (const auto& entry : order->pricing_entries)
  {
    int r = detailTable_->rowCount();
    detailTable_->insertRow(r);
    detailTable_->setItem(r, 0, new QTableWidgetItem(QString::number(entry.job_id)));
    detailTable_->setItem(r, 1, new QTableWidgetItem(QString::number(entry.total_pages)));
    detailTable_->setItem(r, 2, new QTableWidgetItem(QString::number(entry.copies)));
    detailTable_->setItem(r, 3, new QTableWidgetItem(QString::fromStdString(entry.matched_rule_id)));
    auto* priceItem = new QTableWidgetItem(QString::number(entry.total_price, 'f', 2));
    priceItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    detailTable_->setItem(r, 4, priceItem);
  }
}

}  // namespace wpce::ui
