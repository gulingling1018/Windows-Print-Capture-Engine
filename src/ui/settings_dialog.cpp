#include "ui/settings_dialog.h"

#include "common/pricing_engine.h"
#include "common/printer_profile.h"

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
  resize(config_.window.settings_width, config_.window.settings_height);
  setMinimumSize(560, 420);

  auto* root = new QVBoxLayout(this);

  tabs_ = new QTabWidget();

  buildMerchantTab();
  buildPricingTab();
  buildPrinterTab();
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

namespace
{

QStringList splitDimension(const QString& text)
{
  if (text.trimmed() == QStringLiteral("全部")) return {};
  return text.split('|', Qt::SkipEmptyParts);
}

QString mapDuplex(const QString& text)
{
  if (text.trimmed() == QStringLiteral("单面")) return QStringLiteral("simplex");
  if (text.trimmed() == QStringLiteral("双面")) return QStringLiteral("duplex");
  return {};
}

QString mapPrinter(const QString& text)
{
  if (text.trimmed() == QStringLiteral("全部")) return {};
  return text.trimmed();
}

}  // namespace

void SettingsDialog::buildPricingTab()
{
  auto* page = new QWidget();
  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(14);

  // ── Colour auto-pricing toggle ─────────────────────────────────────
  colorPricePending_ = new QCheckBox(QStringLiteral("彩色作业不自动定价，需操作员手动填写"));
  layout->addWidget(colorPricePending_);

  // ── Pricing rules matrix ───────────────────────────────────────────
  auto* rulesGroup = new QGroupBox(QStringLiteral("定价规则矩阵 (支持 A4|B5 多值)"));
  auto* rulesLayout = new QVBoxLayout(rulesGroup);

  pricingRulesTable_ = new QTableWidget(0, 6);
  pricingRulesTable_->setHorizontalHeaderLabels({
    QStringLiteral("纸张"), QStringLiteral("单|双面"),
    QStringLiteral("打印机"), QStringLiteral("颜色"),
    QStringLiteral("文件类型"), QStringLiteral("单价(元/张)") });
  pricingRulesTable_->horizontalHeader()->setStretchLastSection(true);
  pricingRulesTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
  pricingRulesTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
  pricingRulesTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
  pricingRulesTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Interactive);
  pricingRulesTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Interactive);
  pricingRulesTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Interactive);
  pricingRulesTable_->setColumnWidth(0, 90);
  pricingRulesTable_->setColumnWidth(1, 72);
  pricingRulesTable_->setColumnWidth(2, 120);
  pricingRulesTable_->setColumnWidth(3, 60);
  pricingRulesTable_->setColumnWidth(4, 80);
  pricingRulesTable_->setColumnWidth(5, 100);
  pricingRulesTable_->verticalHeader()->hide();
  pricingRulesTable_->setAlternatingRowColors(true);
  pricingRulesTable_->setStyleSheet(QStringLiteral(
    "QTableWidget { border: 1px solid #e2e8f0; border-radius: 6px; background: white; }"
    "QTableWidget::item { padding: 6px 10px; font-size: 12px; }"
    "QHeaderView::section { background: #f1f5f9; color: #475569; font-weight: 600;"
    "  padding: 6px 10px; font-size: 12px; border: none;"
    "  border-bottom: 2px solid #cbd5e1; border-right: 1px solid #e2e8f0; }"
    "QHeaderView::section:last { border-right: none; }"
    "QTableWidget::item:selected { background: #dbeafe; color: #1e40af; }"));

  rulesLayout->addWidget(pricingRulesTable_);

  // ── Add-rule row with combos (idiot-proof) ─────────────────────────
  auto* addRow = new QFrame();
  addRow->setObjectName(QStringLiteral("PricingAddRow"));
  addRow->setStyleSheet(QStringLiteral(
    "QFrame#PricingAddRow { background: #f8fafc; border: 1px solid #e2e8f0; border-radius: 8px; }"
    "QLabel { color: #475569; font-size: 12px; font-weight: 600; }"));
  auto* addOuter = new QVBoxLayout(addRow);
  addOuter->setContentsMargins(12, 10, 12, 10);
  addOuter->setSpacing(10);

  auto* addTop = new QHBoxLayout();
  addTop->setSpacing(8);

  auto* paperCombo = new QComboBox();
  paperCombo->setEditable(true);
  paperCombo->addItems({QStringLiteral("A4|B5"), QStringLiteral("A4"), QStringLiteral("B5"),
                        QStringLiteral("A3"), QStringLiteral("Letter"), QStringLiteral("Legal"),
                        QStringLiteral("全部")});
  paperCombo->setCurrentText(QStringLiteral("A4|B5"));
  paperCombo->setMinimumWidth(76);

  auto* duplexCombo = new QComboBox();
  duplexCombo->addItems({QStringLiteral("单面"), QStringLiteral("双面"), QStringLiteral("全部")});
  duplexCombo->setMinimumWidth(66);

  auto* printerCombo = new QComboBox();
  printerCombo->addItem(QStringLiteral("全部"));
  for (const auto& p : config_.printers.profiles)
    printerCombo->addItem(QString::fromStdString(p.printer_name));
  printerCombo->setCurrentText(QStringLiteral("全部"));
  printerCombo->setMinimumWidth(110);

  // Context-sensitive paper sizes: when a printer is selected, show that
  // printer's supported paper sizes.  When "全部" (all printers), show
  // the union of all printers' paper sizes.
  connect(printerCombo, &QComboBox::currentTextChanged, this, [=](const QString& printer) {
    QString currentPaper = paperCombo->currentText();
    paperCombo->clear();
    // Always include the common presets.
    paperCombo->addItems({QStringLiteral("A4|B5"), QStringLiteral("A4"), QStringLiteral("B5"),
                          QStringLiteral("A3"), QStringLiteral("Letter"), QStringLiteral("Legal"),
                          QStringLiteral("全部")});
    if (printer == QStringLiteral("全部"))
    {
      auto sizes = common::get_all_paper_sizes(config_.printers);
      for (const auto& s : sizes)
      {
        QString name = QString::fromStdString(s);
        if (paperCombo->findText(name) < 0)
          paperCombo->addItem(name);
      }
    }
    else
    {
      auto sizes = common::get_paper_sizes_for_printer(config_.printers,
          printer.toStdString());
      for (const auto& s : sizes)
      {
        QString name = QString::fromStdString(s);
        if (paperCombo->findText(name) < 0)
          paperCombo->addItem(name);
      }
    }
    // Restore previously selected paper if still available.
    if (paperCombo->findText(currentPaper) >= 0)
      paperCombo->setCurrentText(currentPaper);
    else
      paperCombo->setCurrentText(QStringLiteral("A4|B5"));
  });

  auto* extCombo = new QComboBox();
  extCombo->setEditable(true);
  extCombo->addItems({QStringLiteral("全部"), QStringLiteral(".pdf"), QStringLiteral(".doc"),
                      QStringLiteral(".docx"), QStringLiteral(".dwg"),
                      QStringLiteral(".ppt"), QStringLiteral(".pptx")});
  extCombo->setCurrentText(QStringLiteral("全部"));
  extCombo->setMinimumWidth(76);

  auto* colorCombo = new QComboBox();
  colorCombo->addItems({QStringLiteral("黑白"), QStringLiteral("彩色"), QStringLiteral("全部")});
  colorCombo->setCurrentText(QStringLiteral("黑白"));
  colorCombo->setMinimumWidth(56);

  auto* priceSpin = new QDoubleSpinBox();
  priceSpin->setRange(0.00, 999.99);
  priceSpin->setDecimals(2);
  priceSpin->setSingleStep(0.05);
  priceSpin->setValue(0.15);
  priceSpin->setMinimumWidth(70);

  auto* addRuleBtn = new QPushButton(QStringLiteral("+ 添加"));
  addRuleBtn->setCursor(Qt::PointingHandCursor);
  addRuleBtn->setStyleSheet(QStringLiteral(
    "QPushButton { background: #2563eb; color: white; border: none; border-radius: 5px;"
    "  padding: 5px 14px; font-weight: 600; font-size: 12px; }"
    "QPushButton:hover { background: #1d4ed8; }"));

  auto* removeRuleBtn = new QPushButton(QStringLiteral("- 移除"));
  removeRuleBtn->setCursor(Qt::PointingHandCursor);
  removeRuleBtn->setStyleSheet(QStringLiteral(
    "QPushButton { background: #f1f5f9; color: #475569; border: 1px solid #cbd5e1;"
    "  border-radius: 5px; padding: 5px 14px; font-weight: 600; font-size: 12px; }"
    "QPushButton:hover { background: #e2e8f0; }"));

  addTop->addWidget(new QLabel(QStringLiteral("纸张")));
  addTop->addWidget(paperCombo);
  addTop->addWidget(new QLabel(QStringLiteral("单双")));
  addTop->addWidget(duplexCombo);
  addTop->addWidget(new QLabel(QStringLiteral("打印机")));
  addTop->addWidget(printerCombo);
  addTop->addWidget(new QLabel(QStringLiteral("颜色")));
  addTop->addWidget(colorCombo);
  addTop->addStretch(1);

  auto* addBottom = new QHBoxLayout();
  addBottom->setSpacing(8);
  addBottom->addWidget(new QLabel(QStringLiteral("类型")));
  addBottom->addWidget(extCombo);
  addBottom->addWidget(new QLabel(QStringLiteral("单价")));
  addBottom->addWidget(priceSpin);
  addBottom->addWidget(addRuleBtn);
  addBottom->addWidget(removeRuleBtn);
  addBottom->addStretch(1);

  addOuter->addLayout(addTop);
  addOuter->addLayout(addBottom);

  rulesLayout->addWidget(addRow);

  // Preset buttons
  auto* presetRow = new QHBoxLayout();
  auto* presetLabel = new QLabel(QStringLiteral("快速预设:"));
  presetLabel->setStyleSheet(QStringLiteral("color: #64748b; font-size: 11px;"));
  presetRow->addWidget(presetLabel);

  auto addPreset = [&](const QString& name, const QString& paper, const QString& duplex,
                       const QString& printer, const QString& color, const QString& ext, double price)
  {
    auto* btn = new QPushButton(name);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setStyleSheet(QStringLiteral(
      "QPushButton { background: #eff6ff; color: #1d4ed8; border: 1px solid #bfdbfe;"
      "  border-radius: 5px; padding: 4px 10px; font-size: 11px; font-weight: 600; }"
      "QPushButton:hover { background: #dbeafe; }"));
    connect(btn, &QPushButton::clicked, this, [=]() {
      paperCombo->setCurrentText(paper);
      duplexCombo->setCurrentText(duplex);
      printerCombo->setCurrentText(printer);
      colorCombo->setCurrentText(color);
      extCombo->setCurrentText(ext);
      priceSpin->setValue(price);
    });
    presetRow->addWidget(btn);
  };

  addPreset(QStringLiteral("A4单"), QStringLiteral("A4|B5"), QStringLiteral("单面"),
            QStringLiteral("全部"), QStringLiteral("黑白"), QStringLiteral("全部"), 0.15);
  addPreset(QStringLiteral("A4双"), QStringLiteral("A4|B5"), QStringLiteral("双面"),
            QStringLiteral("全部"), QStringLiteral("黑白"), QStringLiteral("全部"), 0.25);
  addPreset(QStringLiteral("A3单"), QStringLiteral("A3"), QStringLiteral("单面"),
            QStringLiteral("全部"), QStringLiteral("黑白"), QStringLiteral("全部"), 0.5);
  addPreset(QStringLiteral("A3双"), QStringLiteral("A3"), QStringLiteral("双面"),
            QStringLiteral("全部"), QStringLiteral("黑白"), QStringLiteral("全部"), 1.0);
  addPreset(QStringLiteral("A3+dwg"), QStringLiteral("A3"), QStringLiteral("单面"),
            QStringLiteral("全部"), QStringLiteral("黑白"), QStringLiteral(".dwg"), 1.0);
  addPreset(QStringLiteral("A4+dwg"), QStringLiteral("A4|B5"), QStringLiteral("单面"),
            QStringLiteral("全部"), QStringLiteral("黑白"), QStringLiteral(".dwg"), 0.5);
  addPreset(QStringLiteral("A4彩单"), QStringLiteral("A4|B5"), QStringLiteral("单面"),
            QStringLiteral("全部"), QStringLiteral("彩色"), QStringLiteral("全部"), 1.0);
  addPreset(QStringLiteral("A4彩双"), QStringLiteral("A4|B5"), QStringLiteral("双面"),
            QStringLiteral("全部"), QStringLiteral("彩色"), QStringLiteral("全部"), 2.0);
  addPreset(QStringLiteral("A3彩色"), QStringLiteral("A3"), QStringLiteral("单面"),
            QStringLiteral("全部"), QStringLiteral("彩色"), QStringLiteral("全部"), 2.0);

  presetRow->addStretch(1);
  rulesLayout->addLayout(presetRow);

  connect(addRuleBtn, &QPushButton::clicked, this, [=]() {
    const int row = pricingRulesTable_->rowCount();
    pricingRulesTable_->insertRow(row);
    pricingRulesTable_->setItem(row, 0, new QTableWidgetItem(paperCombo->currentText()));
    pricingRulesTable_->setItem(row, 1, new QTableWidgetItem(duplexCombo->currentText()));
    pricingRulesTable_->setItem(row, 2, new QTableWidgetItem(printerCombo->currentText()));
    pricingRulesTable_->setItem(row, 3, new QTableWidgetItem(colorCombo->currentText()));
    pricingRulesTable_->setItem(row, 4, new QTableWidgetItem(extCombo->currentText()));
    auto* priceItem = new QTableWidgetItem();
    priceItem->setData(Qt::EditRole, priceSpin->value());
    pricingRulesTable_->setItem(row, 5, priceItem);
  });

  connect(removeRuleBtn, &QPushButton::clicked, this, [this]() {
    const int row = pricingRulesTable_->currentRow();
    if (row >= 0)
      pricingRulesTable_->removeRow(row);
  });

  layout->addWidget(rulesGroup);
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
  colorPricePending_->setChecked(config_.pricing.color_price_pending);

  pricingRulesTable_->setRowCount(0);
  if (config_.pricing.rules.empty())
  {
    // Load factory defaults.
    auto addDefault = [&](const QString& paper, const QString& duplex,
                          const QString& printer, const QString& color,
                          const QString& ext, double price)
    {
      int row = pricingRulesTable_->rowCount();
      pricingRulesTable_->insertRow(row);
      pricingRulesTable_->setItem(row, 0, new QTableWidgetItem(paper));
      pricingRulesTable_->setItem(row, 1, new QTableWidgetItem(duplex));
      pricingRulesTable_->setItem(row, 2, new QTableWidgetItem(printer));
      pricingRulesTable_->setItem(row, 3, new QTableWidgetItem(color));
      pricingRulesTable_->setItem(row, 4, new QTableWidgetItem(ext));
      auto* priceItem = new QTableWidgetItem();
      priceItem->setData(Qt::EditRole, price);
      pricingRulesTable_->setItem(row, 5, priceItem);
    };
    addDefault(QStringLiteral("A4|B5"), QStringLiteral("单面"), QStringLiteral("全部"), QStringLiteral("全部"), QStringLiteral("全部"), 0.15);
    addDefault(QStringLiteral("A4|B5"), QStringLiteral("双面"), QStringLiteral("全部"), QStringLiteral("全部"), QStringLiteral("全部"), 0.25);
    addDefault(QStringLiteral("A3"),    QStringLiteral("单面"), QStringLiteral("全部"), QStringLiteral("全部"), QStringLiteral("全部"), 0.5);
    addDefault(QStringLiteral("A3"),    QStringLiteral("双面"), QStringLiteral("全部"), QStringLiteral("全部"), QStringLiteral("全部"), 1.0);
    addDefault(QStringLiteral("A3"),    QStringLiteral("单面"), QStringLiteral("全部"), QStringLiteral("全部"), QStringLiteral(".dwg"),  1.0);
    addDefault(QStringLiteral("A4|B5"), QStringLiteral("单面"), QStringLiteral("全部"), QStringLiteral("全部"), QStringLiteral(".dwg"),  0.5);
    addDefault(QStringLiteral("A4|B5"), QStringLiteral("单面"), QStringLiteral("全部"), QStringLiteral("彩色"), QStringLiteral("全部"), 1.0);
    addDefault(QStringLiteral("A3"),    QStringLiteral("单面"), QStringLiteral("全部"), QStringLiteral("彩色"), QStringLiteral("全部"), 2.0);
  }
  else
  {
    for (const auto& rule : config_.pricing.rules)
    {
      const int row = pricingRulesTable_->rowCount();
      pricingRulesTable_->insertRow(row);
      QString paper = QString::fromStdString(rule.paper_size);
      QString duplex = QString::fromStdString(rule.duplex_mode);
      QString printer = QString::fromStdString(rule.printer_name);
      QString color = QString::fromStdString(rule.color_mode);
      QString ext = QString::fromStdString(rule.file_extension);
      if (paper.isEmpty()) paper = QStringLiteral("全部");
      if (duplex.isEmpty()) duplex = QStringLiteral("全部");
      else if (duplex == QStringLiteral("simplex")) duplex = QStringLiteral("单面");
      else if (duplex == QStringLiteral("duplex")) duplex = QStringLiteral("双面");
      if (printer.isEmpty()) printer = QStringLiteral("全部");
      if (color.isEmpty()) color = QStringLiteral("全部");
      else if (color == QStringLiteral("color")) color = QStringLiteral("彩色");
      else if (color == QStringLiteral("monochrome")) color = QStringLiteral("黑白");
       if (ext.isEmpty()) ext = QStringLiteral("全部");
       else if (!ext.startsWith('.') && ext != QStringLiteral("全部")) ext = QStringLiteral(".%1").arg(ext);
       pricingRulesTable_->setItem(row, 0, new QTableWidgetItem(paper));
      pricingRulesTable_->setItem(row, 1, new QTableWidgetItem(duplex));
      pricingRulesTable_->setItem(row, 2, new QTableWidgetItem(printer));
      pricingRulesTable_->setItem(row, 3, new QTableWidgetItem(color));
      pricingRulesTable_->setItem(row, 4, new QTableWidgetItem(ext));
      auto* priceItem = new QTableWidgetItem();
      priceItem->setData(Qt::EditRole, rule.price_per_sheet);
      pricingRulesTable_->setItem(row, 5, priceItem);
    }
  }

  // Storage
  outputRoot_->setText(QString::fromStdString(config_.storage.output_root));
  queueCapacity_->setValue(static_cast<int>(config_.storage.queue_capacity));
  retentionDays_->setValue(config_.storage.retention_days);

  // Monitoring
  stallTimeout_->setValue(config_.monitoring.stall_timeout_seconds);
  perfLogEnabled_->setChecked(config_.monitoring.perf_log_enabled);

  // Printers
  reloadPrinterList();
}

void SettingsDialog::storeToConfig()
{
  // Merchant
  config_.merchant.name = merchantName_->text().trimmed().toStdString();
  config_.merchant.phone = merchantPhone_->text().trimmed().toStdString();
  config_.merchant.address = merchantAddress_->text().trimmed().toStdString();

  // Pricing
  config_.pricing.color_price_pending = colorPricePending_->isChecked();

  config_.pricing.rules.clear();
  for (int row = 0; row < pricingRulesTable_->rowCount(); ++row)
  {
    auto* paperItem = pricingRulesTable_->item(row, 0);
    auto* duplexItem = pricingRulesTable_->item(row, 1);
    auto* printerItem = pricingRulesTable_->item(row, 2);
    auto* colorItem = pricingRulesTable_->item(row, 3);
    auto* extItem = pricingRulesTable_->item(row, 4);
    auto* priceItem = pricingRulesTable_->item(row, 5);

    double price = 0.0;
    if (priceItem)
    {
      bool ok = false;
      price = priceItem->data(Qt::EditRole).toDouble(&ok);
      if (!ok) price = 0.0;
    }

    QStringList papers = splitDimension(paperItem ? paperItem->text() : QString());
    QStringList duplexes = splitDimension(duplexItem ? duplexItem->text() : QString());
    QStringList printers = splitDimension(printerItem ? printerItem->text() : QString());
    QStringList colors = splitDimension(colorItem ? colorItem->text() : QString());
    QStringList exts = splitDimension(extItem ? extItem->text() : QString());

    if (papers.isEmpty()) papers.append(QString());
    if (duplexes.isEmpty()) duplexes.append(QString());
    if (printers.isEmpty()) printers.append(QString());
    if (colors.isEmpty()) colors.append(QString());
    if (exts.isEmpty()) exts.append(QString());

    for (const QString& p : papers)
    {
      for (const QString& d : duplexes)
      {
        for (const QString& pr : printers)
        {
          for (const QString& c : colors)
          {
            for (const QString& e : exts)
            {
              core::PricingRule rule;
              rule.rule_id = std::to_string(config_.pricing.rules.size() + 1);
              rule.paper_size = p.trimmed().toStdString();
              rule.duplex_mode = mapDuplex(d).toStdString();
              rule.printer_name = mapPrinter(pr).toStdString();
              // Map color display text to internal value.
              QString colorTrimmed = c.trimmed();
              if (colorTrimmed == QStringLiteral("彩色")) rule.color_mode = "color";
              else if (colorTrimmed == QStringLiteral("黑白")) rule.color_mode = "monochrome";
              else rule.color_mode.clear();
              QString extClean = e.trimmed().toLower();
              if (extClean.startsWith('.')) extClean.remove(0, 1);
              rule.file_extension = extClean.toStdString();
              rule.price_per_sheet = price;
              rule.priority = wpce::common::rule_specificity(rule);
              config_.pricing.rules.push_back(std::move(rule));
            }
          }
        }
      }
    }
  }
  wpce::common::sort_rules_by_priority(config_.pricing.rules);

  // Printers (color capability profiles)
  config_.printers.profiles.clear();
  if (printerTable_ != nullptr)
  {
    for (int row = 0; row < printerTable_->rowCount(); ++row)
    {
      auto* nameItem = printerTable_->item(row, 0);
      auto* combo = qobject_cast<QComboBox*>(printerTable_->cellWidget(row, 1));
      if (nameItem == nullptr) continue;
      common::PrinterProfile profile;
      profile.printer_name = nameItem->text().trimmed().toStdString();
      if (combo != nullptr)
      {
        int idx = combo->currentIndex();
        if (idx == 0) profile.is_color_capable = true;
        else if (idx == 1) profile.is_color_capable = false;
        else continue;  // "不监听" — skip
      }
      if (!profile.printer_name.empty())
        config_.printers.profiles.push_back(std::move(profile));
    }
  }

  // Storage
  config_.storage.output_root = outputRoot_->text().trimmed().toStdString();
  if (config_.storage.output_root.empty())
  {
    config_.storage.output_root = "./data";
  }
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

// ── Printer tab ─────────────────────────────────────────────────────────────

void SettingsDialog::buildPrinterTab()
{
  auto* page = new QWidget();
  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(14);

  auto* headerRow = new QHBoxLayout();
  auto* headerLabel = new QLabel(QStringLiteral("打印机能力配置"));
  headerLabel->setStyleSheet(QStringLiteral(
    "font-size: 14px; font-weight: bold; color: #0f172a;"));
  headerRow->addWidget(headerLabel);
  headerRow->addStretch(1);

  detectPrintersBtn_ = new QPushButton(QStringLiteral("重新探测"));
  detectPrintersBtn_->setCursor(Qt::PointingHandCursor);
  detectPrintersBtn_->setStyleSheet(QStringLiteral(
    "QPushButton { background: #2563eb; color: white; border: none;"
    "  border-radius: 5px; padding: 6px 14px; font-weight: 600; font-size: 12px; }"
    "QPushButton:hover { background: #1d4ed8; }"));
  connect(detectPrintersBtn_, &QPushButton::clicked, this, [this]() {
    config_.printers = wpce::common::detect_printer_capabilities();
    reloadPrinterList();
  });
  headerRow->addWidget(detectPrintersBtn_);
  layout->addLayout(headerRow);

  auto* hintLabel = new QLabel(QStringLiteral(
    "黑白打印机收到彩色作业时按黑白计价；\n"
    "彩色打印机收到彩色作业时留空等操作员填价。"));
  hintLabel->setStyleSheet(QStringLiteral("color: #64748b; font-size: 11px;"));
  layout->addWidget(hintLabel);

  printerTable_ = new QTableWidget(0, 2);
  printerTable_->setHorizontalHeaderLabels({
    QStringLiteral("打印机名称"), QStringLiteral("能力") });
  printerTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  printerTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  printerTable_->verticalHeader()->hide();
  printerTable_->setAlternatingRowColors(true);
  printerTable_->setStyleSheet(QStringLiteral(
    "QTableWidget { border: 1px solid #e2e8f0; border-radius: 6px; }"
    "QTableWidget::item { padding: 5px 8px; }"));
  layout->addWidget(printerTable_);

  layout->addStretch(1);
  tabs_->addTab(page, QStringLiteral("打印机"));
}

void SettingsDialog::reloadPrinterList()
{
  if (printerTable_ == nullptr) return;
  printerTable_->setRowCount(0);

  for (const auto& profile : config_.printers.profiles)
  {
    int row = printerTable_->rowCount();
    printerTable_->insertRow(row);
    printerTable_->setItem(row, 0, new QTableWidgetItem(
      QString::fromStdString(profile.printer_name)));

    auto* combo = new QComboBox();
    combo->addItem(QStringLiteral("彩色"));
    combo->addItem(QStringLiteral("黑白"));
    combo->addItem(QStringLiteral("不监听"));
    combo->setCurrentIndex(profile.is_color_capable ? 0 : 1);
    combo->setStyleSheet(QStringLiteral("QComboBox { padding: 4px 8px; }"));
    printerTable_->setCellWidget(row, 1, combo);
  }
}

}  // namespace wpce::ui
