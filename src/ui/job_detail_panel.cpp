#include "ui/job_detail_panel.h"

#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QScrollArea>

namespace wpce::ui
{

namespace
{

QColor stateColor(core::FieldState state)
{
  switch (state)
  {
    case core::FieldState::Reported:     return QColor(0x1a, 0x8c, 0x4a);
    case core::FieldState::NotReported:  return QColor(0x99, 0x99, 0x99);
    case core::FieldState::Unsupported:  return QColor(0xcc, 0x88, 0x00);
    case core::FieldState::Unknown:      return QColor(0xcc, 0x66, 0x00);
    case core::FieldState::Error:        return QColor(0xcc, 0x22, 0x22);
    default:                             return QColor(0x99, 0x99, 0x99);
  }
}

QLabel* makeSectionHeader(const QString& text)
{
  auto* label = new QLabel(text);
  QFont f = label->font();
  f.setBold(true);
  f.setPointSize(f.pointSize() + 1);
  label->setFont(f);
  return label;
}

QFrame* makeRule()
{
  auto* rule = new QFrame();
  rule->setFrameShape(QFrame::HLine);
  rule->setFrameShadow(QFrame::Sunken);
  return rule;
}

QLabel* makeMonoLabel(const QString& text)
{
  auto* label = new QLabel(text);
  QFont f("Consolas");
  f.setPointSize(10);
  label->setFont(f);
  label->setWordWrap(true);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return label;
}

QLabel* makeStateBadge(const core::FieldState state)
{
  auto* label = new QLabel(QString::fromUtf8(core::to_string(state).data()));
  QFont f = label->font();
  f.setPointSize(f.pointSize() - 1);
  label->setFont(f);
  const QColor c = stateColor(state);
  label->setStyleSheet(QStringLiteral("color: %1;").arg(c.name()));
  return label;
}

}  // namespace

// ── JobDetailPanel ─────────────────────────────────────────────────────

JobDetailPanel::JobDetailPanel(QWidget* parent) : QWidget(parent)
{
  root_ = new QVBoxLayout(this);
  root_->setContentsMargins(8, 8, 8, 8);

  // ── Job info section ─────────────────────────────────────────────────

  jobHeader_ = makeSectionHeader(
    QStringLiteral("\xe4\xbb\xbb\xe5\x8a\xa1\xe4\xbf\xa1\xe6\x81\xaf")); // 任务信息
  root_->addWidget(jobHeader_);
  root_->addWidget(makeRule());

  jobInfoLayout_ = new QVBoxLayout();
  jobInfoLayout_->setContentsMargins(4, 4, 4, 4);
  root_->addLayout(jobInfoLayout_);

  // ── Spacer ────────────────────────────────────────────────────────────

  root_->addSpacing(12);

  // ── Metadata section ──────────────────────────────────────────────────

  metaHeader_ = makeSectionHeader(
    QStringLiteral("\xe6\x89\x93\xe5\x8d\xb0\xe5\x8f\x82\xe6\x95\xb0")); // 打印参数
  root_->addWidget(metaHeader_);
  root_->addWidget(makeRule());

  metaLayout_ = new QVBoxLayout();
  metaLayout_->setContentsMargins(4, 4, 4, 4);
  root_->addLayout(metaLayout_);

  root_->addStretch();
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
}

// ── Job info ──────────────────────────────────────────────────────────

void JobDetailPanel::rebuildJobInfo(const core::JobSnapshot& job)
{
  clearLayout(jobInfoLayout_);

  // Use a form-like pair of mono labels.
  auto add = [&](const QString& label, const QString& value)
  {
    auto* row = new QHBoxLayout();
    row->setContentsMargins(2, 1, 2, 1);

    auto* nameLabel = new QLabel(label);
    QFont f = nameLabel->font();
    f.setPointSize(f.pointSize() - 1);
    nameLabel->setFont(f);
    nameLabel->setStyleSheet(QStringLiteral("color: #666;"));
    nameLabel->setMinimumWidth(70);

    auto* valLabel = makeMonoLabel(value);

    row->addWidget(nameLabel);
    row->addWidget(valLabel, 1);
    jobInfoLayout_->addLayout(row);
  };

  add(QStringLiteral("Job ID"),     QString::number(job.job_id));
  add(QStringLiteral("\xe6\x96\x87\xe6\xa1\xa3"),        // 文档
      QString::fromStdString(job.document_name));
  add(QStringLiteral("\xe7\x94\xa8\xe6\x88\xb7"),        // 用户
      QString::fromStdString(job.user));
  add(QStringLiteral("\xe6\x89\x93\xe5\x8d\xb0\xe6\x9c\xba"),  // 打印机
      QString::fromStdString(job.printer_name));
  add(QStringLiteral("\xe6\x8f\x90\xe4\xba\xa4"),        // 提交
      QString::fromStdString(job.submit_time_utc));
  add(QStringLiteral("\xe7\x8a\xb6\xe6\x80\x81"),        // 状态
      QString::fromStdString(job.status));
}

// ── Metadata ──────────────────────────────────────────────────────────

void JobDetailPanel::rebuildMetadata(const core::MetadataSnapshot& metadata)
{
  clearLayout(metaLayout_);

  addFieldRow(QStringLiteral("\xe6\x80\xbb\xe9\xa1\xb5\xe6\x95\xb0"), metadata.total_pages);    // 总页数
  addFieldRow(QStringLiteral("\xe5\xb7\xb2\xe6\x89\x93\xe5\x8d\xb0"), metadata.pages_printed); // 已打印
  addFieldRow(QStringLiteral("\xe4\xbb\xbd\xe6\x95\xb0"),     metadata.copies);               // 份数
  addFieldRow(QStringLiteral("\xe5\x8f\x8c\xe9\x9d\xa2"),     metadata.duplex);               // 双面
  addFieldRow(QStringLiteral("\xe9\xa2\x9c\xe8\x89\xb2"),     metadata.color);                // 颜色
  addFieldRow(QStringLiteral("\xe7\xba\xb8\xe5\xbc\xa0"),     metadata.paper_size);            // 纸张
  addFieldRow(QStringLiteral("\xe6\x96\xb9\xe5\x90\x91"),     metadata.orientation);           // 方向
  addFieldRow(QStringLiteral("N-up"),        metadata.n_up);
  addFieldRow(QStringLiteral("\xe9\x80\x90\xe4\xbb\xbd"),     metadata.collate);               // 逐份
  addFieldRow(QStringLiteral("\xe6\x89\x93\xe5\x8d\xb0\xe8\xb4\xa8\xe9\x87\x8f"), metadata.print_quality); // 打印质量

  // dm_fields is a plain diagnostic string, not a FieldValue.
  if (!metadata.dm_fields_hex.empty())
  {
    auto* row = new QHBoxLayout();
    row->setContentsMargins(2, 1, 2, 1);

    auto* nameLabel = new QLabel(QStringLiteral("dm_fields"));
    QFont f = nameLabel->font();
    f.setPointSize(f.pointSize() - 1);
    nameLabel->setFont(f);
    nameLabel->setStyleSheet(QStringLiteral("color: #666;"));
    nameLabel->setMinimumWidth(70);

    auto* valLabel = makeMonoLabel(
      QString::fromStdString(metadata.dm_fields_hex));
    valLabel->setStyleSheet(
      QStringLiteral("color: %1;").arg(stateColor(core::FieldState::Reported).name()));

    row->addWidget(nameLabel);
    row->addWidget(valLabel, 1);
    metaLayout_->addLayout(row);
  }
}

// ── Helpers ─────────────────────────────────────────────────────────────

void JobDetailPanel::clearLayout(QLayout* layout)
{
  if (layout == nullptr) return;

  QLayoutItem* item;
  while ((item = layout->takeAt(0)) != nullptr)
  {
    // Recursively clear child layouts.
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
                                 const core::FieldValue& fv)
{
  auto* row = new QHBoxLayout();
  row->setContentsMargins(2, 1, 2, 1);

  auto* nameLabel = new QLabel(label);
  QFont f = nameLabel->font();
  f.setPointSize(f.pointSize() - 1);
  nameLabel->setFont(f);
  nameLabel->setStyleSheet(QStringLiteral("color: #666;"));
  nameLabel->setMinimumWidth(70);

  const QColor color = stateColor(fv.state);

  auto* valLabel = makeMonoLabel(QString::fromStdString(fv.value));
  valLabel->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));

  auto* badge = makeStateBadge(fv.state);

  row->addWidget(nameLabel);
  row->addWidget(valLabel, 1);
  row->addWidget(badge);

  metaLayout_->addLayout(row);
}

}  // namespace wpce::ui
