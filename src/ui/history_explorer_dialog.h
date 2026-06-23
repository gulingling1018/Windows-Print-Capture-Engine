#pragma once

#include "core/data_contracts.h"

#include <QDialog>
#include <QLineEdit>
#include <QTableWidget>
#include <QVBoxLayout>

namespace wpce::ui
{

/// History explorer dialog — lists all settled SettlementOrder files
/// from <output_root>/_state/settlement_orders/ and allows the operator
/// to inspect past settlements for audit / re-print.
class HistoryExplorerDialog : public QDialog
{
  Q_OBJECT

 public:
  explicit HistoryExplorerDialog(const QString& stateDir,
                                  QWidget* parent = nullptr);

  /// Reload orders from disk (call before show()).
  void loadOrders();

 private:
  void buildUi();
  void onSelectionChanged();
  void onFilterChanged();

  QString stateDir_;
  QTableWidget* orderTable_ = nullptr;
  QTableWidget* detailTable_ = nullptr;
  QLineEdit* filterEdit_ = nullptr;

  struct OrderRow
  {
    QString orderId;
    QString batchId;
    QString userId;
    QString settledAt;
    double totalPrice = 0.0;
    int jobCount = 0;
    QString filePath;
  };
  std::vector<OrderRow> orders_;
};

}  // namespace wpce::ui
