#include "ui/settings_dialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace wpce::ui
{

SettingsDialog::SettingsDialog(common::AppConfig& config, QWidget* parent)
    : QDialog(parent), config_(config)
{
  setWindowTitle(QStringLiteral("WPCE 设置"));
  resize(560, 480);
  setMinimumSize(480, 380);

  auto* root = new QVBoxLayout(this);

  tabs_ = new QTabWidget();

  buildMerchantTab();
  buildPricingTab();
  buildStorageTab();

  root->addWidget(tabs_);

  // ── Button row ────────────────────────────────────────────────────────
  auto* buttonRow = new QHBoxLayout();

  auto* restoreBtn = new QPushButton(QStringLiteral("恢复默认"));
  restoreBtn->setCursor(Qt::PointingHandCursor);
  connect(restoreBtn, &QPushButton::clicked, this, &SettingsDialog::onRestoreDefaults);

  auto* box = new QDialogButtonBox();
  auto* saveBtn = box->addButton(QStringLiteral("保存"), QDialogButtonBox::AcceptRole);
  saveBtn->setCursor(Qt::PointingHandCursor);
  auto* cancelBtn = box->addButton(QStringLiteral("取消"), QDialogButtonBox::RejectRole);
  cancelBtn->setCursor(Qt::PointingHandCursor);

  connect(box, &QDialogButtonBox::accepted, this, &SettingsDialog::onSave);
  connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);

  buttonRow->addWidget(restoreBtn);
  buttonRow->addStretch(1);
  buttonRow->addWidget(box);
  root->addLayout(buttonRow);

  loadFromConfig();
}

// ── Merchant tab ───────────────────────────────────────────────────────────

void SettingsDialog::buildMerchantTab()
{
  auto* page = new QWidget();
  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(14);

  auto* form = new QFormLayout();
  form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
  form->setSpacing(10);

  merchantName_ = new QLineEdit();
  merchantName_->setPlaceholderText(QStringLiteral("输入商户/打印店名称"));
  form->addRow(QStringLiteral("商户名称："), merchantName_);

  merchantPhone_ = new QLineEdit();
  merchantPhone_->setPlaceholderText(QStringLiteral("联系电话"));
  form->addRow(QStringLiteral("联系电话："), merchantPhone_);

  merchantAddress_ = new QLineEdit();
  merchantAddress_->setPlaceholderText(QStringLiteral("地址"));
  form->addRow(QStringLiteral("地址："), merchantAddress_);

  layout->addLayout(form);
  layout->addStretch(1);
  tabs_->addTab(page, QStringLiteral("商户信息"));
}

// ── Pricing tab ────────────────────────────────────────────────────────────

void SettingsDialog::buildPricingTab()
{
  auto* page = new QWidget();
  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(14);

  // ── Base prices ──────────────────────────────────────────────────────
  auto* baseGroup = new QGroupBox(QStringLiteral("基础价格 (元)"));
  auto* baseForm = new QFormLayout(baseGroup);
  baseForm->setSpacing(8);

  simplexPrice_ = new QDoubleSpinBox();
  simplexPrice_->setRange(0.00, 999.99);
  simplexPrice_->setDecimals(2);
  simplexPrice_->setSingleStep(0.05);
  simplexPrice_->setSuffix(QStringLiteral(" 元/页"));
  baseForm->addRow(QStringLiteral("单面打印："), simplexPrice_);

  duplexPrice_ = new QDoubleSpinBox();
  duplexPrice_->setRange(0.00, 999.99);
  duplexPrice_->setDecimals(2);
  duplexPrice_->setSingleStep(0.05);
  duplexPrice_->setSuffix(QStringLiteral(" 元/张"));
  baseForm->addRow(QStringLiteral("双面打印："), duplexPrice_);

  layout->addWidget(baseGroup);

  // ── Surcharges ───────────────────────────────────────────────────────
  auto* surchGroup = new QGroupBox(QStringLiteral("附加费 (元/页)"));
  auto* surchForm = new QFormLayout(surchGroup);
  surchForm->setSpacing(8);

  colorSurcharge_ = new QDoubleSpinBox();
  colorSurcharge_->setRange(0.00, 999.99);
  colorSurcharge_->setDecimals(2);
  colorSurcharge_->setSingleStep(0.10);
  colorSurcharge_->setSuffix(QStringLiteral(" 元/页"));
  surchForm->addRow(QStringLiteral("彩色附加："), colorSurcharge_);

  largePaperSurcharge_ = new QDoubleSpinBox();
  largePaperSurcharge_->setRange(0.00, 999.99);
  largePaperSurcharge_->setDecimals(2);
  largePaperSurcharge_->setSingleStep(0.10);
  largePaperSurcharge_->setSuffix(QStringLiteral(" 元/页"));
  surchForm->addRow(QStringLiteral("大幅面附加(A3+)："), largePaperSurcharge_);

  highQualitySurcharge_ = new QDoubleSpinBox();
  highQualitySurcharge_->setRange(0.00, 999.99);
  highQualitySurcharge_->setDecimals(2);
  highQualitySurcharge_->setSingleStep(0.05);
  highQualitySurcharge_->setSuffix(QStringLiteral(" 元/页"));
  surchForm->addRow(QStringLiteral("高质量附加："), highQualitySurcharge_);

  layout->addWidget(surchGroup);

  // ── Volume discounts ─────────────────────────────────────────────────
  auto* volGroup = new QGroupBox(QStringLiteral("批量折扣"));
  auto* volLayout = new QVBoxLayout(volGroup);

  volumeDiscountTable_ = new QTableWidget(0, 2);
  volumeDiscountTable_->setHorizontalHeaderLabels({QStringLiteral("最低页数"), QStringLiteral("折扣率")});
  volumeDiscountTable_->horizontalHeader()->setStretchLastSection(true);
  volumeDiscountTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
  volumeDiscountTable_->setColumnWidth(0, 130);
  volumeDiscountTable_->verticalHeader()->hide();
  volumeDiscountTable_->setAlternatingRowColors(true);

  auto* volBtnRow = new QHBoxLayout();
  auto* addTierBtn = new QPushButton(QStringLiteral("+ 添加阶梯"));
  addTierBtn->setCursor(Qt::PointingHandCursor);
  auto* removeTierBtn = new QPushButton(QStringLiteral("- 移除选中"));
  removeTierBtn->setCursor(Qt::PointingHandCursor);

  connect(addTierBtn, &QPushButton::clicked, this,
          [this]()
          {
            const int row = volumeDiscountTable_->rowCount();
            volumeDiscountTable_->insertRow(row);
            auto* pagesItem = new QTableWidgetItem(QStringLiteral("100"));
            pagesItem->setData(Qt::EditRole, 100);
            volumeDiscountTable_->setItem(row, 0, pagesItem);
            auto* discItem = new QTableWidgetItem(QStringLiteral("0.95"));
            discItem->setData(Qt::EditRole, 0.95);
            volumeDiscountTable_->setItem(row, 1, discItem);
          });

  connect(removeTierBtn, &QPushButton::clicked, this,
          [this]()
          {
            const int row = volumeDiscountTable_->currentRow();
            if (row >= 0)
            {
              volumeDiscountTable_->removeRow(row);
            }
          });

  volBtnRow->addWidget(addTierBtn);
  volBtnRow->addWidget(removeTierBtn);
  volBtnRow->addStretch(1);

  volLayout->addWidget(volumeDiscountTable_);
  volLayout->addLayout(volBtnRow);
  layout->addWidget(volGroup);
  layout->addStretch(1);

  tabs_->addTab(page, QStringLiteral("价格方案"));
}

// ── Storage tab ─────────────────────────────────────────────────────────────

void SettingsDialog::buildStorageTab()
{
  auto* page = new QWidget();
  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(14);

  // ── Output path ──────────────────────────────────────────────────────
  auto* pathGroup = new QGroupBox(QStringLiteral("数据存储"));
  auto* pathForm = new QFormLayout(pathGroup);
  pathForm->setSpacing(8);

  auto* pathRow = new QHBoxLayout();
  outputRoot_ = new QLineEdit();
  outputRoot_->setPlaceholderText(QStringLiteral("例如: ./data 或 D:\\WPCE\\Data"));
  browseOutputBtn_ = new QPushButton(QStringLiteral("浏览..."));
  browseOutputBtn_->setCursor(Qt::PointingHandCursor);
  connect(browseOutputBtn_, &QPushButton::clicked, this,
          [this]()
          {
            const QString dir = QFileDialog::getExistingDirectory(
              this, QStringLiteral("选择数据存储目录"), outputRoot_->text());
            if (!dir.isEmpty())
            {
              outputRoot_->setText(dir);
            }
          });
  pathRow->addWidget(outputRoot_, 1);
  pathRow->addWidget(browseOutputBtn_);
  pathForm->addRow(QStringLiteral("存储根目录："), pathRow);

  useTimestampSubdir_ = new QCheckBox(QStringLiteral("按时间戳创建子目录 (capture-20260620-143052)"));
  pathForm->addRow(QString(), useTimestampSubdir_);

  queueCapacity_ = new QSpinBox();
  queueCapacity_->setRange(16, 4096);
  queueCapacity_->setSingleStep(64);
  queueCapacity_->setSuffix(QStringLiteral(" 条"));
  pathForm->addRow(QStringLiteral("写入队列容量："), queueCapacity_);

  retentionDays_ = new QSpinBox();
  retentionDays_->setRange(0, 3650);
  retentionDays_->setSingleStep(30);
  retentionDays_->setSuffix(QStringLiteral(" 天 (0=永久)"));
  pathForm->addRow(QStringLiteral("数据保留天数："), retentionDays_);

  layout->addWidget(pathGroup);

  // ── Monitoring ───────────────────────────────────────────────────────
  auto* monGroup = new QGroupBox(QStringLiteral("监听参数"));
  auto* monForm = new QFormLayout(monGroup);
  monForm->setSpacing(8);

  stallTimeout_ = new QSpinBox();
  stallTimeout_->setRange(0, 3600);
  stallTimeout_->setSingleStep(5);
  stallTimeout_->setSuffix(QStringLiteral(" 秒 (0=禁用)"));
  monForm->addRow(QStringLiteral("卡纸检测超时："), stallTimeout_);

  perfLogEnabled_ = new QCheckBox(QStringLiteral("启用性能日志 (CSV)"));
  monForm->addRow(QString(), perfLogEnabled_);

  layout->addWidget(monGroup);
  layout->addStretch(1);

  tabs_->addTab(page, QStringLiteral("存储与监听"));
}

// ── Load / Store ────────────────────────────────────────────────────────────

void SettingsDialog::loadFromConfig()
{
  // Merchant
  merchantName_->setText(QString::fromStdString(config_.merchant.name));
  merchantPhone_->setText(QString::fromStdString(config_.merchant.phone));
  merchantAddress_->setText(QString::fromStdString(config_.merchant.address));

  // Pricing
  simplexPrice_->setValue(config_.pricing.single_sided_per_page);
  duplexPrice_->setValue(config_.pricing.double_sided_per_sheet);
  colorSurcharge_->setValue(config_.pricing.color_surcharge);
  largePaperSurcharge_->setValue(config_.pricing.large_paper_surcharge);
  highQualitySurcharge_->setValue(config_.pricing.high_quality_surcharge);

  volumeDiscountTable_->setRowCount(0);
  for (const auto& tier : config_.pricing.volume_discounts)
  {
    const int row = volumeDiscountTable_->rowCount();
    volumeDiscountTable_->insertRow(row);
    auto* pagesItem = new QTableWidgetItem();
    pagesItem->setData(Qt::EditRole, tier.min_pages);
    volumeDiscountTable_->setItem(row, 0, pagesItem);
    auto* discItem = new QTableWidgetItem();
    discItem->setData(Qt::EditRole, tier.discount);
    volumeDiscountTable_->setItem(row, 1, discItem);
  }

  // Storage
  outputRoot_->setText(QString::fromStdString(config_.storage.output_root));
  useTimestampSubdir_->setChecked(config_.storage.use_timestamp_subdir);
  queueCapacity_->setValue(static_cast<int>(config_.storage.queue_capacity));
  retentionDays_->setValue(config_.storage.retention_days);

  // Monitoring
  stallTimeout_->setValue(config_.monitoring.stall_timeout_seconds);
  perfLogEnabled_->setChecked(config_.monitoring.perf_log_enabled);
}

void SettingsDialog::storeToConfig()
{
  // Merchant
  config_.merchant.name = merchantName_->text().trimmed().toStdString();
  config_.merchant.phone = merchantPhone_->text().trimmed().toStdString();
  config_.merchant.address = merchantAddress_->text().trimmed().toStdString();

  // Pricing
  config_.pricing.single_sided_per_page = simplexPrice_->value();
  config_.pricing.double_sided_per_sheet = duplexPrice_->value();
  config_.pricing.color_surcharge = colorSurcharge_->value();
  config_.pricing.large_paper_surcharge = largePaperSurcharge_->value();
  config_.pricing.high_quality_surcharge = highQualitySurcharge_->value();

  config_.pricing.volume_discounts.clear();
  for (int row = 0; row < volumeDiscountTable_->rowCount(); ++row)
  {
    const auto* pagesItem = volumeDiscountTable_->item(row, 0);
    const auto* discItem = volumeDiscountTable_->item(row, 1);
    if (pagesItem == nullptr || discItem == nullptr) continue;

    bool ok = false;
    const int minPages = pagesItem->data(Qt::EditRole).toInt(&ok);
    if (!ok || minPages <= 0) continue;

    const double discount = discItem->data(Qt::EditRole).toDouble(&ok);
    if (!ok || discount <= 0.0 || discount > 1.0) continue;

    config_.pricing.volume_discounts.push_back({minPages, discount});
  }
  // Keep sorted.
  std::sort(config_.pricing.volume_discounts.begin(),
            config_.pricing.volume_discounts.end(),
            [](const common::VolumeDiscountTier& a, const common::VolumeDiscountTier& b)
            { return a.min_pages < b.min_pages; });

  // Storage
  config_.storage.output_root = outputRoot_->text().trimmed().toStdString();
  if (config_.storage.output_root.empty())
  {
    config_.storage.output_root = "./data";
  }
  config_.storage.use_timestamp_subdir = useTimestampSubdir_->isChecked();
  config_.storage.queue_capacity = static_cast<std::size_t>(queueCapacity_->value());
  config_.storage.retention_days = retentionDays_->value();

  // Monitoring
  config_.monitoring.stall_timeout_seconds = stallTimeout_->value();
  config_.monitoring.perf_log_enabled = perfLogEnabled_->isChecked();
}

// ── Slots ───────────────────────────────────────────────────────────────────

void SettingsDialog::onSave()
{
  storeToConfig();

  const auto config_path = common::AppConfig::default_config_path();
  if (config_.save_to_json(config_path))
  {
    accept();
  }
  else
  {
    QMessageBox::warning(this, QStringLiteral("保存失败"),
                         QStringLiteral("无法将配置写入:\n%1")
                           .arg(QString::fromStdWString(config_path.wstring())));
  }
}

void SettingsDialog::onRestoreDefaults()
{
  const auto answer = QMessageBox::question(
    this, QStringLiteral("恢复默认"),
    QStringLiteral("将价格、路径等所有设置恢复到出厂默认值？"),
    QMessageBox::Yes | QMessageBox::No);

  if (answer != QMessageBox::Yes) return;

  config_ = common::AppConfig::defaults();
  loadFromConfig();
}

}  // namespace wpce::ui
