#include "ui/job_detail_panel.h"

#include <QColor>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QSizePolicy>

#include <string_view>

namespace wpce::ui
{

namespace
{

QString fromView(const std::string_view value)
{
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString stateKey(const core::FieldState state)
{
  return fromView(core::to_string(state));
}

QString stateLabel(const core::FieldState state)
{
  switch (state)
  {
    case core::FieldState::Reported:
      return QStringLiteral("已报告");
    case core::FieldState::Unsupported:
      return QStringLiteral("不支持");
    case core::FieldState::NotReported:
      return QStringLiteral("未报告");
    case core::FieldState::Error:
      return QStringLiteral("错误");
    case core::FieldState::Unknown:
    default:
      return QStringLiteral("未知");
  }
}

QColor stateColor(const core::FieldState state)
{
  switch (state)
  {
    case core::FieldState::Reported:
      return QColor(0x0f, 0x8a, 0x5f);
    case core::FieldState::Unsupported:
      return QColor(0xb5, 0x47, 0x08);
    case core::FieldState::NotReported:
      return QColor(0x66, 0x70, 0x85);
    case core::FieldState::Unknown:
      return QColor(0x47, 0x55, 0x67);
    case core::FieldState::Error:
      return QColor(0xd9, 0x2d, 0x20);
    default:
      return QColor(0x66, 0x70, 0x85);
  }
}

QLabel* makeSectionHeader(const QString& text)
{
  auto* label = new QLabel(text);
  label->setObjectName(QStringLiteral("PanelHeader"));
  return label;
}

QLabel* makeMonoLabel(const QString& text)
{
  auto* label = new QLabel(text.isEmpty() ? QStringLiteral("-") : text);
  QFont font(QStringLiteral("Cascadia Mono"));
  font.setStyleHint(QFont::Monospace);
  font.setPointSize(10);
  label->setFont(font);
  label->setObjectName(QStringLiteral("MonoValue"));
  label->setWordWrap(true);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  return label;
}

QLabel* makeNameLabel(const QString& text)
{
  auto* label = new QLabel(text);
  label->setObjectName(QStringLiteral("FieldName"));
  label->setMinimumWidth(82);
  return label;
}

QLabel* makeStateBadge(const core::FieldState state)
{
  auto* label = new QLabel(stateLabel(state));
  label->setObjectName(QStringLiteral("StateBadge"));
  label->setProperty("state", stateKey(state));
  label->setAlignment(Qt::AlignCenter);
  label->setMinimumWidth(74);
  return label;
}

QFrame* makeDivider()
{
  auto* divider = new QFrame();
  divider->setObjectName(QStringLiteral("Divider"));
  divider->setFrameShape(QFrame::HLine);
  return divider;
}

QLabel* makeEmptyHint(const QString& text)
{
  auto* label = new QLabel(text);
  label->setObjectName(QStringLiteral("EmptyHint"));
  label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  label->setWordWrap(true);
  return label;
}

QFrame* makePlainRow(const QString& name, const QString& value)
{
  auto* rowFrame = new QFrame();
  rowFrame->setObjectName(QStringLiteral("FieldRow"));

  auto* row = new QHBoxLayout(rowFrame);
  row->setContentsMargins(10, 7, 10, 7);
  row->setSpacing(10);

  row->addWidget(makeNameLabel(name));
  row->addWidget(makeMonoLabel(value), 1);
  return rowFrame;
}

}  // namespace

JobDetailPanel::JobDetailPanel(QWidget* parent) : QWidget(parent)
{
  setObjectName(QStringLiteral("DetailPanel"));

  root_ = new QVBoxLayout(this);
  root_->setContentsMargins(16, 16, 16, 16);
  root_->setSpacing(12);

  jobHeader_ = makeSectionHeader(QStringLiteral("作业详情"));
  root_->addWidget(jobHeader_);
  root_->addWidget(makeDivider());

  jobInfoLayout_ = new QVBoxLayout();
  jobInfoLayout_->setContentsMargins(0, 0, 0, 0);
  jobInfoLayout_->setSpacing(8);
  root_->addLayout(jobInfoLayout_);

  root_->addSpacing(6);

  metaHeader_ = makeSectionHeader(QStringLiteral("打印参数"));
  root_->addWidget(metaHeader_);
  root_->addWidget(makeDivider());

  metaLayout_ = new QVBoxLayout();
  metaLayout_->setContentsMargins(0, 0, 0, 0);
  metaLayout_->setSpacing(8);
  root_->addLayout(metaLayout_);

  root_->addStretch();
  clear();
}

void JobDetailPanel::showJob(const core::JobSnapshot& job,
                             const core::MetadataSnapshot& metadata)
{
  rebuildJobInfo(job);
  rebuildMetadata(metadata);
}

void JobDetailPanel::clear()
{
  clearLayout(jobInfoLayout_);
  clearLayout(metaLayout_);

  jobInfoLayout_->addWidget(makeEmptyHint(QStringLiteral("暂无选中作业")));
  metaLayout_->addWidget(makeEmptyHint(QStringLiteral("等待打印参数")));
}

void JobDetailPanel::rebuildJobInfo(const core::JobSnapshot& job)
{
  clearLayout(jobInfoLayout_);

  jobInfoLayout_->addWidget(makePlainRow(QStringLiteral("Job ID"),
                                         QString::number(job.job_id)));
  jobInfoLayout_->addWidget(makePlainRow(QStringLiteral("文档"),
                                         QString::fromStdString(job.document_name)));
  jobInfoLayout_->addWidget(makePlainRow(QStringLiteral("用户"),
                                         QString::fromStdString(job.user)));
  jobInfoLayout_->addWidget(makePlainRow(QStringLiteral("打印机"),
                                         QString::fromStdString(job.printer_name)));
  jobInfoLayout_->addWidget(makePlainRow(QStringLiteral("提交"),
                                         QString::fromStdString(job.submit_time_utc)));
  jobInfoLayout_->addWidget(makePlainRow(QStringLiteral("状态"),
                                         QString::fromStdString(job.status)));
}

void JobDetailPanel::rebuildMetadata(const core::MetadataSnapshot& metadata)
{
  clearLayout(metaLayout_);

  addFieldRow(QStringLiteral("总页数"), metadata.total_pages);
  addFieldRow(QStringLiteral("已打印"), metadata.pages_printed);
  addFieldRow(QStringLiteral("份数"), metadata.copies);
  addFieldRow(QStringLiteral("双面"), metadata.duplex);
  addFieldRow(QStringLiteral("颜色"), metadata.color);
  addFieldRow(QStringLiteral("纸张"), metadata.paper_size);
  addFieldRow(QStringLiteral("方向"), metadata.orientation);
  addFieldRow(QStringLiteral("N-up"), metadata.n_up);
  addFieldRow(QStringLiteral("逐份"), metadata.collate);
  addFieldRow(QStringLiteral("质量"), metadata.print_quality);

  if (!metadata.dm_fields_hex.empty())
  {
    auto* rowFrame = makePlainRow(QStringLiteral("dm_fields"),
                                  QString::fromStdString(metadata.dm_fields_hex));
    metaLayout_->addWidget(rowFrame);
  }
}

void JobDetailPanel::clearLayout(QLayout* layout)
{
  if (layout == nullptr)
  {
    return;
  }

  QLayoutItem* item = nullptr;
  while ((item = layout->takeAt(0)) != nullptr)
  {
    if (auto* childLayout = item->layout())
    {
      clearLayout(childLayout);
    }

    if (auto* widget = item->widget())
    {
      widget->deleteLater();
    }

    delete item;
  }
}

void JobDetailPanel::addFieldRow(const QString& label,
                                 const core::FieldValue& value)
{
  auto* rowFrame = new QFrame();
  rowFrame->setObjectName(QStringLiteral("FieldRow"));

  auto* row = new QHBoxLayout(rowFrame);
  row->setContentsMargins(10, 7, 10, 7);
  row->setSpacing(10);

  auto* valueLabel = makeMonoLabel(QString::fromStdString(value.value));
  valueLabel->setStyleSheet(QStringLiteral("color: %1;")
                              .arg(stateColor(value.state).name()));

  row->addWidget(makeNameLabel(label));
  row->addWidget(valueLabel, 1);
  row->addWidget(makeStateBadge(value.state));

  metaLayout_->addWidget(rowFrame);
}

}  // namespace wpce::ui
