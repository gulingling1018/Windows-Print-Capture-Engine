#include "ui/job_table_model.h"
#include "ui/pricing_config.h"
#include "ui/user_color_palette.h"

#include <QBrush>
#include <QColor>
#include <QFont>

#include <algorithm>
#include <climits>
#include <string_view>

namespace wpce::ui
{

namespace
{

QString fromView(const std::string_view value)
{
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString phaseDisplayName(const core::JobLifecyclePhase phase) noexcept
{
  switch (phase)
  {
    case core::JobLifecyclePhase::Accepted:
      return QStringLiteral("已接收");
    case core::JobLifecyclePhase::Spooling:
      return QStringLiteral("假脱机");
    case core::JobLifecyclePhase::Printing:
      return QStringLiteral("打印中");
    case core::JobLifecyclePhase::Completed:
      return QStringLiteral("已完成");
    case core::JobLifecyclePhase::Failed:
      return QStringLiteral("失败");
    case core::JobLifecyclePhase::Cancelled:
      return QStringLiteral("已取消");
    case core::JobLifecyclePhase::Stalled:
      return QStringLiteral("停滞");
    case core::JobLifecyclePhase::Unknown:
    default:
      return QStringLiteral("未知");
  }
}

QColor phaseColor(const core::JobLifecyclePhase phase) noexcept
{
  switch (phase)
  {
    case core::JobLifecyclePhase::Completed:
      return QColor(0x0f, 0x8a, 0x5f);
    case core::JobLifecyclePhase::Failed:
      return QColor(0xd9, 0x2d, 0x20);
    case core::JobLifecyclePhase::Cancelled:
      return QColor(0x66, 0x70, 0x85);
    case core::JobLifecyclePhase::Stalled:
      return QColor(0xb5, 0x47, 0x08);
    case core::JobLifecyclePhase::Printing:
    case core::JobLifecyclePhase::Spooling:
      return QColor(0x25, 0x63, 0xeb);
    case core::JobLifecyclePhase::Accepted:
      return QColor(0x7c, 0x3a, 0xed);
    case core::JobLifecyclePhase::Unknown:
    default:
      return QColor(0x66, 0x70, 0x85);
  }
}

int progressPercent(const JobRowData& data) noexcept
{
  if (data.totalPages <= 0)
  {
    return -1;
  }

  const int pct = (data.pagesPrinted * 100) / data.totalPages;
  return std::clamp(pct, 0, 100);
}

QString reportedValueOrDash(const core::FieldValue& field)
{
  if (field.state == core::FieldState::Reported && !field.value.empty())
  {
    return QString::fromStdString(field.value);
  }
  return QStringLiteral("-");
}

QVariant sortValueForColumn(const JobRowData& data, const int column)
{
  switch (column)
  {
    case JobTableModel::ColJobId:
      return static_cast<qlonglong>(data.job.job_id);
    case JobTableModel::ColDocument:
      return QString::fromStdString(data.job.document_name).toCaseFolded();
    case JobTableModel::ColUser:
      return QString::fromStdString(data.job.user).toCaseFolded();
    case JobTableModel::ColPhase:
      return phaseDisplayName(data.phase);
    case JobTableModel::ColProgress:
      return progressPercent(data);
    case JobTableModel::ColTotalPages:
      return data.totalPages;
    case JobTableModel::ColCopies:
      return reportedValueOrDash(data.metadata.copies);
    case JobTableModel::ColPrice:
      return data.price;
    case JobTableModel::ColSubmitTime:
      return QString::fromStdString(data.job.submit_time_utc);
    default:
      return {};
  }
}

}  // namespace

JobTableModel::JobTableModel(QObject* parent) : QAbstractTableModel(parent)
{
  flushTimer_ = new QTimer(this);
  flushTimer_->setInterval(160);
  flushTimer_->setSingleShot(true);
  connect(flushTimer_, &QTimer::timeout, this, &JobTableModel::flushPendingUpdates);
}

int JobTableModel::rowCount(const QModelIndex& parent) const
{
  if (parent.isValid())
  {
    return 0;
  }

  std::scoped_lock lock(dataMutex_);
  return static_cast<int>(jobOrder_.size());
}

int JobTableModel::columnCount(const QModelIndex& parent) const
{
  if (parent.isValid())
  {
    return 0;
  }
  return ColumnCount;
}

QVariant JobTableModel::data(const QModelIndex& index, const int role) const
{
  if (!index.isValid())
  {
    return {};
  }

  const int row = index.row();
  const int col = index.column();

  std::scoped_lock lock(dataMutex_);
  if (row < 0 || row >= jobOrder_.size())
  {
    return {};
  }

  const auto it = jobs_.find(jobOrder_.at(row));
  if (it == jobs_.end())
  {
    return {};
  }

  const JobRowData& data = it.value();
  const QString userId = QString::fromStdString(data.job.user).trimmed();

  if (role == PhaseKeyRole)
  {
    return fromView(core::to_string(data.phase));
  }

  if (role == ProgressPercentRole)
  {
    return progressPercent(data);
  }

  if (role == SortRole)
  {
    return sortValueForColumn(data, col);
  }

  if (role == UserIdRole)
  {
    return userId;
  }

  if (role == DocumentPathRole)
  {
    return QString::fromStdString(data.job.document_name);
  }

  if (role == RowColorRole)
  {
    return userBackgroundColor(userId);
  }

  if (role == RowTextColorRole)
  {
    return userTextColor(userId);
  }

  if (role == Qt::BackgroundRole)
  {
    return QBrush(subtleUserTint(userId));
  }

  if (role == Qt::TextAlignmentRole)
  {
    switch (col)
    {
      case ColJobId:
      case ColPhase:
      case ColProgress:
      case ColTotalPages:
      case ColCopies:
        return Qt::AlignCenter;
      case ColPrice:
        return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
      default:
        return Qt::AlignVCenter;
    }
  }

  if (role == Qt::ForegroundRole && col == ColPhase)
  {
    return QBrush(phaseColor(data.phase));
  }

  if (role == Qt::FontRole && (col == ColJobId || col == ColPhase || col == ColPrice))
  {
    QFont font;
    font.setWeight(QFont::DemiBold);
    if (col == ColPrice)
    {
      font.setFamily(QStringLiteral("Cascadia Mono"));
      font.setStyleHint(QFont::Monospace);
    }
    return font;
  }

  if (role == Qt::ToolTipRole)
  {
    return QStringLiteral("Job ID %1\n文档: %2\n用户: %3\n打印机: %4")
      .arg(data.job.job_id)
      .arg(QString::fromStdString(data.job.document_name))
      .arg(userId)
      .arg(QString::fromStdString(data.job.printer_name));
  }

  if (role != Qt::DisplayRole && role != Qt::EditRole)
  {
    return {};
  }

  switch (col)
  {
    case ColJobId:
      return data.job.job_id;
    case ColDocument:
      return QString::fromStdString(data.job.document_name);
    case ColUser:
      return userId;
    case ColPhase:
      return phaseDisplayName(data.phase);
    case ColProgress:
    {
      const int pct = progressPercent(data);
      if (pct >= 0)
      {
        return QStringLiteral("%1 / %2  %3%")
          .arg(data.pagesPrinted)
          .arg(data.totalPages)
          .arg(pct);
      }
      if (data.metadata.pages_printed.state == core::FieldState::Reported)
      {
        return QString::fromStdString(data.metadata.pages_printed.value);
      }
      return QStringLiteral("-");
    }
    case ColTotalPages:
      return reportedValueOrDash(data.metadata.total_pages);
    case ColCopies:
      return reportedValueOrDash(data.metadata.copies);
    case ColPrice:
      return data.price > 0.0 ? QString::number(data.price, 'f', 2) : QStringLiteral("-");
    case ColSubmitTime:
      return QString::fromStdString(data.job.submit_time_utc);
    default:
      return {};
  }
}

QVariant JobTableModel::headerData(const int section,
                                   const Qt::Orientation orientation,
                                   const int role) const
{
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
  {
    return {};
  }

  switch (section)
  {
    case ColJobId:
      return QStringLiteral("Job ID");
    case ColDocument:
      return QStringLiteral("文档");
    case ColUser:
      return QStringLiteral("用户");
    case ColPhase:
      return QStringLiteral("阶段");
    case ColProgress:
      return QStringLiteral("进度");
    case ColTotalPages:
      return QStringLiteral("页数");
    case ColCopies:
      return QStringLiteral("份数");
    case ColPrice:
      return QStringLiteral("费用 (¥)");
    case ColSubmitTime:
      return QStringLiteral("提交时间");
    default:
      return {};
  }
}

std::optional<JobRowData> JobTableModel::jobAtRow(const int row) const
{
  std::scoped_lock lock(dataMutex_);
  if (row < 0 || row >= jobOrder_.size())
  {
    return std::nullopt;
  }

  const auto it = jobs_.find(jobOrder_.at(row));
  if (it == jobs_.end())
  {
    return std::nullopt;
  }

  return it.value();
}

void JobTableModel::clear()
{
  flushTimer_->stop();

  beginResetModel();
  {
    std::scoped_lock lock(dataMutex_);
    jobOrder_.clear();
    jobs_.clear();
    dirtyRows_.clear();
  }
  endResetModel();
}

void JobTableModel::onJobSnapshot(core::JobSnapshot snapshot)
{
  if (snapshot.job_id == 0)
  {
    return;
  }

  const std::uint32_t jobId = snapshot.job_id;
  int newRow = -1;
  bool insertRow = false;

  {
    std::scoped_lock lock(dataMutex_);

    const int modelRow = jobOrder_.indexOf(jobId);
    if (modelRow < 0)
    {
      newRow = static_cast<int>(jobOrder_.size());
      insertRow = true;
    }
    else
    {
      jobs_[jobId].job = std::move(snapshot);
      dirtyRows_.insert(modelRow);
    }
  }

  if (insertRow)
  {
    beginInsertRows(QModelIndex(), newRow, newRow);
    {
      std::scoped_lock lock(dataMutex_);
      auto& row = jobs_[jobId];
      row.job = std::move(snapshot);
      jobOrder_.append(jobId);
    }
    endInsertRows();
  }

  if (!flushTimer_->isActive())
  {
    QMetaObject::invokeMethod(flushTimer_, "start", Qt::QueuedConnection);
  }
}

void JobTableModel::onMetadata(core::MetadataSnapshot metadata)
{
  if (metadata.job_id == 0)
  {
    return;
  }

  {
    std::scoped_lock lock(dataMutex_);

    auto& row = jobs_[metadata.job_id];
    row.metadata = std::move(metadata);

    try
    {
      row.totalPages = std::stoi(row.metadata.total_pages.value);
    }
    catch (...)
    {
      row.totalPages = 0;
    }

    try
    {
      row.pagesPrinted = std::stoi(row.metadata.pages_printed.value);
    }
    catch (...)
    {
      row.pagesPrinted = 0;
    }

    row.price = calculate_job_cost(row.metadata, PricingConfig{});

    const int modelRow = jobOrder_.indexOf(row.metadata.job_id);
    if (modelRow >= 0)
    {
      dirtyRows_.insert(modelRow);
    }
  }

  if (!flushTimer_->isActive())
  {
    QMetaObject::invokeMethod(flushTimer_, "start", Qt::QueuedConnection);
  }
}

void JobTableModel::onLifecycleEvent(core::JobLifecycleEvent event)
{
  if (event.job_id == 0)
  {
    return;
  }

  const std::uint32_t jobId = event.job_id;

  {
    std::scoped_lock lock(dataMutex_);

    auto& row = jobs_[jobId];
    row.phase = event.phase;

    if (event.total_pages > 0)
    {
      row.totalPages = event.total_pages;
    }
    row.pagesPrinted = event.pages_printed;
    row.lifecycleEvents.push_back(std::move(event));

    const int modelRow = jobOrder_.indexOf(jobId);
    if (modelRow >= 0)
    {
      dirtyRows_.insert(modelRow);
    }
  }

  if (!flushTimer_->isActive())
  {
    QMetaObject::invokeMethod(flushTimer_, "start", Qt::QueuedConnection);
  }
}

void JobTableModel::flushPendingUpdates()
{
  QSet<int> rows;
  {
    std::scoped_lock lock(dataMutex_);
    rows.swap(dirtyRows_);
  }

  if (rows.isEmpty())
  {
    return;
  }

  int minRow = INT_MAX;
  int maxRow = -1;
  for (const int row : rows)
  {
    minRow = std::min(minRow, row);
    maxRow = std::max(maxRow, row);
  }

  if (minRow <= maxRow)
  {
    emit dataChanged(index(minRow, 0), index(maxRow, ColumnCount - 1));
  }
}

}  // namespace wpce::ui
