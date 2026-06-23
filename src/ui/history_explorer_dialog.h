#pragma once

#include "core/data_contracts.h"

#include <QDate>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <vector>

namespace wpce::ui
{

class HistoryExplorerDialog : public QDialog
{
  Q_OBJECT

 public:
  explicit HistoryExplorerDialog(const QString& historyRoot,
                                  QWidget* parent = nullptr);

  void loadOrders();

 private:
  void buildUi();
  void navigateDate(int delta);
  void goToToday();
  void loadOrdersForCurrentDate();
  void onFilterChanged();
  void onSelectionChanged();
  void updateDateDisplay();

  QString historyRoot_;
  QDate currentDate_;
  std::vector<std::string> availableDates_;

  // UI elements
  QPushButton* prevDateBtn_ = nullptr;
  QPushButton* nextDateBtn_ = nullptr;
  QPushButton* todayBtn_ = nullptr;
  QLabel* dateLabel_ = nullptr;
  QLabel* summaryLabel_ = nullptr;
  QLineEdit* searchEdit_ = nullptr;
  QTableWidget* orderTable_ = nullptr;
  QTableWidget* detailTable_ = nullptr;

  struct OrderRow
  {
    QString orderId;
    QString batchId;
    QString userId;
    QString settledAt;
    QString firstDoc;
    QString lastDoc;
    double totalPrice = 0.0;
    int jobCount = 0;
    core::SettlementOrder order;
  };
  std::vector<OrderRow> orders_;
};

}  // namespace wpce::ui
