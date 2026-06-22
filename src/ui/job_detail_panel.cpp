/// @file job_detail_panel.cpp
/// Detail panel — dual-mode: group summary + job detail.

#include "ui/job_detail_panel.h"

#include <QColor>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSizePolicy>
#include <QTimer>

#include <initializer_list>
#include <string_view>

namespace wpce::ui
{
namespace
{

QString fromView(std::string_view value)
{
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString stateLabel(core::FieldState state)
{
  switch (state)
  {
    case core::FieldState::Reported:     return QStringLiteral("已报告");
    case core::FieldState::Unsupported:  return QStringLiteral("不支持");
    case core::FieldState::NotReported:  return QStringLiteral("未报告");
    case core::FieldState::Error:        return QStringLiteral("错误");
    case core::FieldState::Unknown:
    default:                             return QStringLiteral("未知");
  }
}

QColor stateColor(core::FieldState state)
{
  switch (state)
  {
    case core::FieldState::Reported:     return QColor(0x0f, 0x8a, 0x5f);
    case core::FieldState::Unsupported:  return QColor(0xb5, 0x47, 0x08);
    case core::FieldState::NotReported:  return QColor(0x94, 0xa3, 0xb8);
    case core::FieldState::Error:        return QColor(0xd9, 0x2d, 0x20);
    case core::FieldState::Unknown:
    default:                             return QColor(0x6b, 0x72, 0x80);
  }
}

QString fieldValueText(const core::FieldValue& field)
{
  if (field.state == core::FieldState::Reported && !field.value.empty())
    return QString::fromStdString(field.value);
  return QStringLiteral("-");
}

QWidget* makePlainRow(const QString& name, const QString& value)
{
  auto* row = new QFrame();
  row->setObjectName(QStringLiteral("FieldRow"));
  row->setMinimumWidth(0);
  row->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(10, 6, 10, 6);
  layout->setSpacing(10);

  auto* nameLabel = new QLabel(name);
  nameLabel->setObjectName(QStringLiteral("FieldName"));
  nameLabel->setMinimumWidth(52);

  auto* valueLabel = new QLabel(value.isEmpty() ? QStringLiteral("-") : value);
  valueLabel->setObjectName(QStringLiteral("MonoValue"));
  valueLabel->setMinimumWidth(0);
  valueLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  valueLabel->setWordWrap(true);
  valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  QFont mono(QStringLiteral("Cascadia Mono"));
  mono.setStyleHint(QFont::Monospace);
  mono.setPointSize(10);
  valueLabel->setFont(mono);

  layout->addWidget(nameLabel);
  layout->addWidget(valueLabel, 1);
  return row;
}

struct ParamField
{
  QString label;
  const core::FieldValue* field = nullptr;
};

QFrame* makeParamTile(const QString& label, const core::FieldValue& field)
{
  const QString stateKey = fromView(core::to_string(field.state)).toLower();

  auto* tile = new QFrame();
  tile->setObjectName(QStringLiteral("ParamTile"));
  tile->setProperty("state", stateKey);
  tile->setMinimumWidth(0);
  tile->setMinimumHeight(72);
  tile->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

  auto* layout = new QVBoxLayout(tile);
  layout->setContentsMargins(10, 8, 10, 8);
  layout->setSpacing(6);

  auto* topRow = new QHBoxLayout();
  topRow->setContentsMargins(0, 0, 0, 0);
  topRow->setSpacing(8);

  auto* nameLabel = new QLabel(label);
  nameLabel->setObjectName(QStringLiteral("ParamName"));
  nameLabel->setMinimumWidth(0);
  nameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  nameLabel->setWordWrap(false);

  auto* badge = new QLabel(stateLabel(field.state));
  badge->setObjectName(QStringLiteral("StateBadge"));
  badge->setProperty("state", stateKey);
  badge->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

  topRow->addWidget(nameLabel, 1);
  topRow->addWidget(badge, 0, Qt::AlignRight);

  auto* valueLabel = new QLabel(fieldValueText(field));
  valueLabel->setObjectName(QStringLiteral("ParamValue"));
  valueLabel->setMinimumWidth(0);
  valueLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  valueLabel->setStyleSheet(
    QStringLiteral("color: %1;").arg(stateColor(field.state).name()));
  valueLabel->setWordWrap(true);
  valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  QFont mono(QStringLiteral("Cascadia Mono"));
  mono.setStyleHint(QFont::Monospace);
  mono.setPointSize(11);
  mono.setWeight(QFont::DemiBold);
  valueLabel->setFont(mono);

  layout->addLayout(topRow);
  layout->addWidget(valueLabel, 1);
  return tile;
}

QFrame* makeKeyParamTile(const QString& label, const core::FieldValue& field)
{
  const QString stateKey = fromView(core::to_string(field.state)).toLower();

  auto* tile = new QFrame();
  tile->setObjectName(QStringLiteral("ParamTile"));
  tile->setProperty("state", stateKey);
  tile->setMinimumWidth(0);
  tile->setMinimumHeight(58);
  tile->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

  auto* layout = new QVBoxLayout(tile);
  layout->setContentsMargins(10, 7, 10, 7);
  layout->setSpacing(4);

  auto* nameLabel = new QLabel(label);
  nameLabel->setObjectName(QStringLiteral("ParamName"));
  nameLabel->setMinimumWidth(0);
  nameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

  auto* valueLabel = new QLabel(fieldValueText(field));
  valueLabel->setObjectName(QStringLiteral("ParamValue"));
  valueLabel->setMinimumWidth(0);
  valueLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  valueLabel->setStyleSheet(
    QStringLiteral("color: %1;").arg(stateColor(field.state).name()));
  valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  QFont vf(QStringLiteral("Cascadia Mono"));
  vf.setStyleHint(QFont::Monospace);
  vf.setPointSize(12);
  vf.setWeight(QFont::DemiBold);
  valueLabel->setFont(vf);

  layout->addWidget(nameLabel);
  layout->addWidget(valueLabel);
  return tile;
}

QWidget* makeKeyParamGrid(std::initializer_list<ParamField> fields,
                           int columns = 2)
{
  auto* wrap = new QWidget();
  wrap->setObjectName(QStringLiteral("ParamGrid"));
  wrap->setMinimumWidth(0);
  wrap->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

  auto* grid = new QGridLayout(wrap);
  grid->setContentsMargins(0, 0, 0, 0);
  grid->setHorizontalSpacing(8);
  grid->setVerticalSpacing(8);

  columns = std::max(1, columns);
  for (int c = 0; c < columns; ++c)
    grid->setColumnStretch(c, 1);

  int i = 0;
  for (const auto& item : fields)
  {
    if (item.field == nullptr) continue;
    grid->addWidget(makeKeyParamTile(item.label, *item.field),
                    i / columns, i % columns);
    ++i;
  }

  return wrap;
}

QWidget* makeParamGrid(std::initializer_list<ParamField> fields)
{
  auto* wrap = new QWidget();
  wrap->setObjectName(QStringLiteral("ParamGrid"));
  wrap->setMinimumWidth(0);
  wrap->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

  auto* grid = new QGridLayout(wrap);
  grid->setContentsMargins(0, 0, 0, 0);
  grid->setHorizontalSpacing(8);
  grid->setVerticalSpacing(8);
  grid->setColumnStretch(0, 1);

  int i = 0;
  for (const auto& item : fields)
  {
    if (item.field == nullptr) continue;
    grid->addWidget(makeParamTile(item.label, *item.field), i, 0);
    ++i;
  }

  return wrap;
}

QWidget* makeSectionLabel(const QString& text)
{
  auto* label = new QLabel(text);
  label->setObjectName(QStringLiteral("FieldGroupLabel"));
  return label;
}

QFrame* makeDividerH()
{
  auto* d = new QFrame();
  d->setObjectName(QStringLiteral("Divider"));
  d->setFrameShape(QFrame::HLine);
  return d;
}

// ── Collapsible section widget ───────────────────────────────────────

class CollapsibleSection : public QFrame
{
 public:
  explicit CollapsibleSection(const QString& title, QWidget* parent = nullptr)
      : QFrame(parent), expanded_(true)
  {
    setObjectName(QStringLiteral("CollapsibleSection"));

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    headerBtn_ = new QPushButton(
      QStringLiteral("▼ ") + title);
    headerBtn_->setObjectName(QStringLiteral("CollapsibleHeader"));
    headerBtn_->setCheckable(true);
    headerBtn_->setChecked(expanded_);
    headerBtn_->setCursor(Qt::PointingHandCursor);
    headerBtn_->setStyleSheet(QStringLiteral(
      "QPushButton#CollapsibleHeader {"
      "  text-align: left;"
      "  padding: 7px 10px;"
      "  border: 1px solid #e2e8f0;"
      "  border-radius: 6px;"
      "  background: #f8fafc;"
      "  color: #334155;"
      "  font-weight: 600;"
      "  font-size: 11px;"
      "}"
      "QPushButton#CollapsibleHeader:hover {"
      "  background: #f1f5f9;"
      "  border-color: #cbd5e1;"
      "}"
      "QPushButton#CollapsibleHeader:pressed {"
      "  background: #e2e8f0;"
      "}"));

    content_ = new QWidget();
    content_->setObjectName(QStringLiteral("CollapsibleContent"));
    auto* cl = new QVBoxLayout(content_);
    cl->setContentsMargins(4, 6, 4, 6);
    cl->setSpacing(0);
    contentLayout_ = cl;

    connect(headerBtn_, &QPushButton::toggled, this,
            [this, title](bool checked) {
              expanded_ = checked;
              headerBtn_->setText(
                expanded_ ? QStringLiteral("▼ ") + title
                          : QStringLiteral("▶ ") + title);
              content_->setVisible(expanded_);
            });

    mainLayout->addWidget(headerBtn_);
    mainLayout->addWidget(content_);
  }

  void setContent(QWidget* widget) { contentLayout_->addWidget(widget); }

  void setExpanded(bool expanded) { headerBtn_->setChecked(expanded); }

  bool isExpanded() const { return expanded_; }

 private:
  QPushButton* headerBtn_ = nullptr;
  QWidget* content_ = nullptr;
  QVBoxLayout* contentLayout_ = nullptr;
  bool expanded_;
};

}  // namespace

JobDetailPanel::JobDetailPanel(QWidget* parent) : QWidget(parent)
{
  setObjectName(QStringLiteral("DetailPanel"));
  setMinimumWidth(0);
  setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);

  auto* panelLayout = new QVBoxLayout(this);
  panelLayout->setContentsMargins(0, 0, 0, 0);
  panelLayout->setSpacing(0);

  scroll_ = new QScrollArea(this);
  scroll_->setObjectName(QStringLiteral("DetailScroll"));
  scroll_->setWidgetResizable(true);
  scroll_->setFrameShape(QFrame::NoFrame);
  scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);

  content_ = new QWidget(scroll_);
  content_->setObjectName(QStringLiteral("DetailContent"));
  content_->setMinimumWidth(0);
  content_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

  root_ = new QVBoxLayout(content_);
  root_->setContentsMargins(14, 14, 14, 14);
  root_->setSpacing(8);
  scroll_->setWidget(content_);
  panelLayout->addWidget(scroll_);

  clear();
}

void JobDetailPanel::clear()
{
  clearLayout(root_);
  showingJob_ = false;

  auto* hint = new QLabel(QStringLiteral("选中用户组或作业查看详情"));
  hint->setObjectName(QStringLiteral("EmptyHint"));
  hint->setAlignment(Qt::AlignCenter);
  hint->setWordWrap(true);
  root_->addWidget(hint);
  root_->addStretch();
  resetScroll();
}

void JobDetailPanel::showJob(const core::JobSnapshot& job,
                              const core::MetadataSnapshot& metadata)
{
  cachedJob_ = job;
  cachedMetadata_ = metadata;
  showingJob_ = true;
  cachedKeyParamCols_ = (width() >= 360) ? 2 : 1;

  clearLayout(root_);

  // ── Title ──────────────────────────────────────────────────────────
  auto* title = new QLabel(QStringLiteral("作业详情"));
  QFont tf;
  tf.setPointSize(13);
  tf.setWeight(QFont::DemiBold);
  title->setFont(tf);
  title->setObjectName(QStringLiteral("PanelHeader"));
  root_->addWidget(title);
  root_->addWidget(makeDividerH());

  auto* docTitle = new QLabel(QString::fromStdString(job.document_name));
  docTitle->setObjectName(QStringLiteral("DetailTitle"));
  docTitle->setMinimumWidth(0);
  docTitle->setWordWrap(true);
  docTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
  root_->addWidget(docTitle);

  auto* docMeta = new QLabel(
    QStringLiteral("Job #%1 · %2 · %3")
      .arg(job.job_id)
      .arg(QString::fromStdString(job.user))
      .arg(QString::fromStdString(job.printer_name)));
  docMeta->setObjectName(QStringLiteral("MutedText"));
  docMeta->setMinimumWidth(0);
  docMeta->setWordWrap(true);
  docMeta->setTextInteractionFlags(Qt::TextSelectableByMouse);
  root_->addWidget(docMeta);

  root_->addSpacing(6);

  // ── Key parameters (always visible, compact) ───────────────────────
  auto* keySection = new CollapsibleSection(QStringLiteral("关键打印参数"));
  keySection->setExpanded(true);
  keySection->setContent(makeKeyParamGrid({
    {QStringLiteral("总页数"), &metadata.total_pages},
    {QStringLiteral("已打印"), &metadata.pages_printed},
    {QStringLiteral("份数"),   &metadata.copies},
    {QStringLiteral("双面"),   &metadata.duplex},
    {QStringLiteral("颜色"),   &metadata.color},
    {QStringLiteral("纸张"),   &metadata.paper_size},
  }, cachedKeyParamCols_));
  root_->addWidget(keySection);

  root_->addSpacing(4);

  // ── Job identity ───────────────────────────────────────────────────
  root_->addWidget(makePlainRow(QStringLiteral("Job ID"),
    QString::number(job.job_id)));
  root_->addWidget(makePlainRow(QStringLiteral("文档"),
    QString::fromStdString(job.document_name)));
  root_->addWidget(makePlainRow(QStringLiteral("用户"),
    QString::fromStdString(job.user)));
  root_->addWidget(makePlainRow(QStringLiteral("打印机"),
    QString::fromStdString(job.printer_name)));
  root_->addWidget(makePlainRow(QStringLiteral("提交时间"),
    QString::fromStdString(job.submit_time_utc)));
  root_->addWidget(makePlainRow(QStringLiteral("状态"),
    QString::fromStdString(job.status)));

  root_->addSpacing(4);

  // ── Data source ────────────────────────────────────────────────────
  const bool fromPT = (metadata.dm_fields_hex == "PrintTicket");
  {
    auto* srcRow = new QFrame();
    srcRow->setObjectName(QStringLiteral("FieldRow"));
    auto* srcLayout = new QHBoxLayout(srcRow);
    srcLayout->setContentsMargins(10, 4, 10, 4);

    auto* srcLabel = new QLabel(
      fromPT
        ? QStringLiteral("参数来源: XPS PrintTicket")
        : QStringLiteral("参数来源: Spooler"));
    srcLabel->setObjectName(QStringLiteral("SourceLabel"));
    srcLabel->setProperty("pt", fromPT);
    srcLayout->addWidget(srcLabel);
    srcLayout->addStretch();
    root_->addWidget(srcRow);
  }

  root_->addSpacing(4);

  // ── Print parameters — grouped in collapsible sections ─────────────
  auto* pageSection = new CollapsibleSection(QStringLiteral("页面参数"));
  pageSection->setExpanded(true);
  pageSection->setContent(makeParamGrid({
    {QStringLiteral("总页数"), &metadata.total_pages},
    {QStringLiteral("已打印"), &metadata.pages_printed},
    {QStringLiteral("份数"),   &metadata.copies},
    {QStringLiteral("选页"),   &metadata.page_range},
    {QStringLiteral("N-up"),   &metadata.n_up},
    {QStringLiteral("逐份"),   &metadata.collate},
  }));
  root_->addWidget(pageSection);

  auto* layoutSection = new CollapsibleSection(QStringLiteral("版面参数"));
  layoutSection->setExpanded(true);
  layoutSection->setContent(makeParamGrid({
    {QStringLiteral("双面"), &metadata.duplex},
    {QStringLiteral("纸张"), &metadata.paper_size},
    {QStringLiteral("方向"), &metadata.orientation},
  }));
  root_->addWidget(layoutSection);

  auto* colorSection = new CollapsibleSection(QStringLiteral("色彩与质量"));
  colorSection->setExpanded(true);
  colorSection->setContent(makeParamGrid({
    {QStringLiteral("颜色"), &metadata.color},
    {QStringLiteral("介质"), &metadata.media_type},
    {QStringLiteral("质量"), &metadata.print_quality},
  }));
  root_->addWidget(colorSection);

  // ── Driver flags — only meaningful for DEVMODE-sourced data.
  if (!metadata.dm_fields_hex.empty() && !fromPT)
  {
    root_->addSpacing(4);
    root_->addWidget(makeSectionLabel(QStringLiteral("驱动标志")));
    root_->addWidget(makePlainRow(QStringLiteral("dm_fields"),
      QString::fromStdString(metadata.dm_fields_hex)));
  }

  root_->addStretch();
  resetScroll();
}

void JobDetailPanel::showGroupSummary(const QString& userId,
                                       const UserGroupSummary& summary,
                                       int activeJobCount,
                                       bool completed)
{
  clearLayout(root_);

  // ── Title ──────────────────────────────────────────────────────────
  auto* title = new QLabel(
    completed ? QStringLiteral("已结算 — ") + userId
              : QStringLiteral("进行中 — ") + userId);
  QFont tf;
  tf.setPointSize(12);
  tf.setWeight(QFont::DemiBold);
  title->setFont(tf);
  title->setObjectName(QStringLiteral("PanelHeader"));
  root_->addWidget(title);
  root_->addWidget(makeDividerH());

  // ── Summary card ───────────────────────────────────────────────────
  auto* card = new QFrame();
  card->setObjectName(QStringLiteral("SummaryCard"));
  card->setStyleSheet(QStringLiteral(
    "#SummaryCard { background: #f8fafc; border: 1px solid #e2e8f0;"
    "  border-radius: 8px; }"));
  auto* cardLayout = new QVBoxLayout(card);
  cardLayout->setContentsMargins(14, 12, 14, 12);
  cardLayout->setSpacing(6);

  // Stats row.
  auto addStat = [&](const QString& icon, const QString& label, int val) {
    if (val <= 0) return;
    auto* row = new QHBoxLayout();
    auto* ico = new QLabel(icon);
    ico->setFixedWidth(24);
    ico->setObjectName(QStringLiteral("StatIcon"));
    auto* lab = new QLabel(label);
    lab->setObjectName(QStringLiteral("MutedText"));
    auto* num = new QLabel(QString::number(val));
    num->setObjectName(QStringLiteral("MetricValue"));
    QFont nf;
    nf.setWeight(QFont::DemiBold);
    nf.setPointSize(14);
    num->setFont(nf);
    row->addWidget(ico);
    row->addWidget(lab);
    row->addStretch();
    row->addWidget(num);
    cardLayout->addLayout(row);
  };

  int totalP = summary.simplexPages + summary.duplexPages + summary.specialPaperPages;
  addStat(QStringLiteral("📊"), QStringLiteral("总页数"), totalP);
  addStat(QStringLiteral("📄"), QStringLiteral("单面 (A4/B5)"), summary.simplexPages);
  addStat(QStringLiteral("📑"), QStringLiteral("双面 (A4/B5)"), summary.duplexPages);
  addStat(QStringLiteral("📦"), QStringLiteral("双面张数"), summary.duplexSheets);
  addStat(QStringLiteral("🎨"), QStringLiteral("彩色页数"), summary.colorPages);
  addStat(QStringLiteral("📐"), QStringLiteral("特殊纸张"), summary.specialPaperPages);
  addStat(QStringLiteral("📋"), QStringLiteral("作业数"), summary.jobCount);

  // Active jobs indicator.
  if (activeJobCount > 0)
  {
    auto* activeRow = new QHBoxLayout();
    auto* aico = new QLabel(QStringLiteral("🖨"));
    aico->setFixedWidth(24);
    aico->setObjectName(QStringLiteral("StatIcon"));
    auto* alab = new QLabel(QStringLiteral("活跃作业"));
    alab->setObjectName(QStringLiteral("MutedText"));
    auto* anum = new QLabel(QString::number(activeJobCount));
    anum->setStyleSheet(QStringLiteral("color: #2563eb;"));
    QFont af;
    af.setWeight(QFont::DemiBold);
    af.setPointSize(14);
    anum->setFont(af);
    activeRow->addWidget(aico);
    activeRow->addWidget(alab);
    activeRow->addStretch();
    activeRow->addWidget(anum);
    cardLayout->addLayout(activeRow);
  }

  root_->addWidget(card);

  // ── Special items callout ──────────────────────────────────────────
  if (!summary.specialItems.isEmpty())
  {
    root_->addSpacing(4);
    root_->addWidget(makeSectionLabel(QStringLiteral("特殊计费项目")));

    for (const QString& item : summary.specialItems)
    {
      auto* itemRow = new QFrame();
      itemRow->setObjectName(QStringLiteral("FieldRow"));
      itemRow->setStyleSheet(QStringLiteral(
        "QFrame#FieldRow { border-left: 3px solid #f59e0b; }"));
      auto* itemLayout = new QHBoxLayout(itemRow);
      itemLayout->setContentsMargins(10, 5, 10, 5);
      auto* itemLabel = new QLabel(item);
      itemLabel->setStyleSheet(QStringLiteral("color: #92400e; font-weight: 600;"));
      itemLayout->addWidget(itemLabel);
      itemLayout->addStretch();
      root_->addWidget(itemRow);
    }
  }

  // ── Time ───────────────────────────────────────────────────────────
  if (summary.earliestJobTime.isValid())
  {
    root_->addSpacing(4);
    auto* timeLabel = new QLabel(
      QStringLiteral("最早作业: ") +
      summary.earliestJobTime.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    timeLabel->setObjectName(QStringLiteral("MutedText"));
    root_->addWidget(timeLabel);
  }

  root_->addStretch();
  resetScroll();
}

void JobDetailPanel::resetScroll()
{
  if (scroll_ == nullptr) return;

  QTimer::singleShot(0, scroll_, [this]() {
    if (scroll_ != nullptr && scroll_->verticalScrollBar() != nullptr)
      scroll_->verticalScrollBar()->setValue(0);
  });
}

void JobDetailPanel::clearLayout(QLayout* layout)
{
  if (layout == nullptr) return;
  QLayoutItem* item = nullptr;
  while ((item = layout->takeAt(0)) != nullptr)
  {
    if (auto* childLayout = item->layout())
      clearLayout(childLayout);
    if (auto* widget = item->widget())
      widget->deleteLater();
    delete item;
  }
}

void JobDetailPanel::rebuildJobLayout()
{
  if (!showingJob_) return;
  showJob(cachedJob_, cachedMetadata_);
}

void JobDetailPanel::resizeEvent(QResizeEvent* event)
{
  QWidget::resizeEvent(event);

  if (!showingJob_) return;

  const int desiredCols = (width() >= 360) ? 2 : 1;
  if (desiredCols != cachedKeyParamCols_)
  {
    cachedKeyParamCols_ = desiredCols;
    rebuildJobLayout();
  }
}

}  // namespace wpce::ui
