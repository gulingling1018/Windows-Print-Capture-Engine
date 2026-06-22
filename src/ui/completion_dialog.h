/// @file completion_dialog.h
/// Per-user settlement dialog showing job-level price breakdown.

#pragma once

#include "common/app_config.h"
#include "ui/pricing_config.h"

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QTableWidget>

#include <vector>

namespace wpce::ui
{

struct JobCostLine
{
  QString documentName;
  int pages = 0;
  QString colorMode;
  QString duplexMode;
  QString paperSize;
  double price = 0.0;
};

class CompletionDialog : public QDialog
{
  Q_OBJECT

 public:
  CompletionDialog(const QString& userId,
                   const std::vector<JobCostLine>& lines,
                   const PricingConfig& pricing,
                   QWidget* parent = nullptr);

  /// Returns true if the operator confirmed settlement.
  bool confirmed() const { return confirmed_; }

 private:
  void buildUi(const QString& userId,
               const std::vector<JobCostLine>& lines,
               const PricingConfig& pricing);

  bool confirmed_ = false;
  QTableWidget* table_ = nullptr;
  QLabel* totalLabel_ = nullptr;
  QPushButton* confirmBtn_ = nullptr;
  QPushButton* cancelBtn_ = nullptr;
};

}  // namespace wpce::ui
