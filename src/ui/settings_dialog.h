#pragma once

#include "common/app_config.h"

#include <QCheckBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QString>
#include <QTabWidget>
#include <QTableWidget>

namespace wpce::ui
{

/// Modal dialog for editing application configuration at runtime.
/// Changes are written back to disk immediately on "Save".
class SettingsDialog : public QDialog
{
  Q_OBJECT

 public:
  explicit SettingsDialog(common::AppConfig& config, QWidget* parent = nullptr);

 private slots:
  void onSave();
  void onRestoreDefaults();

 private:
  void buildMerchantTab();
  void buildPricingTab();
  void buildStorageTab();
  void loadFromConfig();
  void storeToConfig();

  common::AppConfig& config_;

  QTabWidget* tabs_ = nullptr;

  // ── Merchant tab ──
  QLineEdit* merchantName_ = nullptr;
  QLineEdit* merchantPhone_ = nullptr;
  QLineEdit* merchantAddress_ = nullptr;

  // ── Pricing tab ──
  QDoubleSpinBox* simplexPrice_ = nullptr;
  QDoubleSpinBox* duplexPrice_ = nullptr;
  QDoubleSpinBox* colorSurcharge_ = nullptr;
  QDoubleSpinBox* largePaperSurcharge_ = nullptr;
  QDoubleSpinBox* highQualitySurcharge_ = nullptr;
  QTableWidget* volumeDiscountTable_ = nullptr;

  // ── Storage tab ──
  QLineEdit* outputRoot_ = nullptr;
  QPushButton* browseOutputBtn_ = nullptr;
  QCheckBox* useTimestampSubdir_ = nullptr;
  QSpinBox* queueCapacity_ = nullptr;
  QSpinBox* retentionDays_ = nullptr;
  QSpinBox* stallTimeout_ = nullptr;
  QCheckBox* perfLogEnabled_ = nullptr;
};

}  // namespace wpce::ui
