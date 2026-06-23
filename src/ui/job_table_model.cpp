#include "ui/job_table_model.h"
#include "common/pricing_engine.h"
#include "ui/pricing_config.h"
#include "ui/user_color_palette.h"

#include <QBrush>
#include <QColor>
#include <QDateTime>
#include <QFont>

#include <algorithm>
#include <climits>
#include <string_view>

namespace wpce::ui
{

namespace
{

QString formatDisplayTime(const QString& isoTime)
{
  QDateTime dt = QDateTime::fromString(isoTime, Qt::ISODate);
  if (!dt.isValid())
    dt = QDateTime::fromString(isoTime, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
  if (dt.isValid())
    return dt.toString(QStringLiteral("HH:mm_yyyy/M/d"));
  return isoTime;
}

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
    case JobTableModel::ColPages:
      return data.totalPages;
    case JobTableModel::ColCopies:
      return reportedValueOrDash(data.metadata.copies);
    case JobTableModel::ColColor:
      return reportedValueOrDash(data.metadata.color);
    case JobTableModel::ColDuplex:
      return reportedValueOrDash(data.metadata.duplex);
    case JobTableModel::ColPaper:
      return reportedValueOrDash(data.metadata.paper_size);
    case JobTableModel::ColPrinter:
      return QString::fromStdString(data.job.printer_name).toCaseFolded();
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

  if (role == FreshnessSecsRole)
  {
    if (!data.lastUpdated.isValid())
      return -1;
    return static_cast<int>(data.lastUpdated.secsTo(QDateTime::currentDateTimeUtc()));
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
      case ColPages:
      case ColCopies:
      case ColColor:
      case ColDuplex:
      case ColPaper:
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
    auto tipField = [&](const char* name, const core::FieldValue& fv) -> QString {
      if (fv.state == core::FieldState::Reported && !fv.value.empty())
        return QStringLiteral("%1: %2").arg(QLatin1StringView(name), QString::fromStdString(fv.value));
      return {};
    };
    QStringList tips;
    tips << QStringLiteral("Job ID: %1").arg(data.job.job_id);
    tips << QStringLiteral("文档: %1").arg(QString::fromStdString(data.job.document_name));
    tips << QStringLiteral("用户: %1").arg(userId);
    tips << QStringLiteral("打印机: %1").arg(QString::fromStdString(data.job.printer_name));
    tips << QStringLiteral("阶段: %1").arg(phaseDisplayName(data.phase));
    // Inline metadata snapshot — saves a click to see params.
    tips << tipField("页数", data.metadata.total_pages);
    tips << tipField("已打印", data.metadata.pages_printed);
    tips << tipField("份数", data.metadata.copies);
    tips << tipField("双面", data.metadata.duplex);
    tips << tipField("颜色", data.metadata.color);
    tips << tipField("纸张", data.metadata.paper_size);
    tips << tipField("方向", data.metadata.orientation);
    tips << tipField("N-up", data.metadata.n_up);
    tips << tipField("介质", data.metadata.media_type);
    if (data.price > 0) tips << QStringLiteral("费用: ¥%1").arg(data.price, 0, 'f', 2);
    // Freshness indicator — helps users spot stale rows.
    if (data.lastUpdated.isValid())
    {
      const int age = static_cast<int>(data.lastUpdated.secsTo(QDateTime::currentDateTimeUtc()));
      if (age < 5)
        tips << QStringLiteral("更新: 刚刚");
      else if (age < 120)
        tips << QStringLiteral("更新: %1 秒前").arg(age);
      else
        tips << QStringLiteral("更新: %1 分钟前").arg(age / 60);
    }
    tips.removeAll({});
    return tips.join('\n');
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
    case ColPages:
      return reportedValueOrDash(data.metadata.total_pages);
    case ColCopies:
      return reportedValueOrDash(data.metadata.copies);
    case ColColor:
    {
      if (data.metadata.color.state == core::FieldState::Reported)
      {
        const auto& v = data.metadata.color.value;
        if (v == "color")      return QStringLiteral("彩");
        if (v == "monochrome") return QStringLiteral("黑");
        return QString::fromStdString(v);
      }
      return QStringLiteral("-");
    }
    case ColDuplex:
    {
      if (data.metadata.duplex.state == core::FieldState::Reported)
      {
        const auto& v = data.metadata.duplex.value;
        if (v == "simplex")                return QStringLiteral("单面");
        if (v == "long-edge (book)")       return QStringLiteral("长边");
        if (v == "short-edge (calendar)")  return QStringLiteral("短边");
        return QString::fromStdString(v);
      }
      return QStringLiteral("-");
    }
    case ColPaper:
      return reportedValueOrDash(data.metadata.paper_size);
    case ColPrinter:
      return QString::fromStdString(data.job.printer_name);
    case ColPrice:
      return data.price > 0.0 ? QString::number(data.price, 'f', 2) : QStringLiteral("-");
    case ColSubmitTime:
      return formatDisplayTime(QString::fromStdString(data.job.submit_time_utc));
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
    case ColPages:
      return QStringLiteral("页数");
    case ColCopies:
      return QStringLiteral("份数");
    case ColColor:
      return QStringLiteral("颜色");
    case ColDuplex:
      return QStringLiteral("单|双面");
    case ColPaper:
      return QStringLiteral("纸张");
    case ColPrinter:
      return QStringLiteral("打印机");
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
      auto& row = jobs_[jobId];
      row.job = std::move(snapshot);
      row.lastUpdated = QDateTime::currentDateTimeUtc();
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
      row.lastUpdated = QDateTime::currentDateTimeUtc();
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

  // Phase 1 — check if a placeholder row is needed, under lock.
  bool needsInsert = false;
  int newRow = -1;
  {
    std::scoped_lock lock(dataMutex_);
    if (jobs_.find(metadata.job_id) == jobs_.end())
    {
      newRow = static_cast<int>(jobOrder_.size());
      needsInsert = true;
    }
  }

  // Phase 2 — notify Qt BEFORE the data changes.
  if (needsInsert)
  {
    beginInsertRows(QModelIndex(), newRow, newRow);
  }

  // Phase 3 — commit the metadata update + optional row creation.
  {
    std::scoped_lock lock(dataMutex_);

    auto it = jobs_.find(metadata.job_id);
    if (it == jobs_.end())
    {
      it = jobs_.insert(metadata.job_id, JobRowData{});
      jobOrder_.append(metadata.job_id);
    }

    auto& row = it.value();
    row.metadata = std::move(metadata);
    row.lastUpdated = QDateTime::currentDateTimeUtc();

    try { row.totalPages = std::stoi(row.metadata.total_pages.value); }
    catch (...) { row.totalPages = 0; }

    try { row.pagesPrinted = std::stoi(row.metadata.pages_printed.value); }
    catch (...) { row.pagesPrinted = 0; }

    row.pricing = common::compute_job_pricing(row.metadata, PricingConfig{}, common::PrinterProfileConfig{});
    row.price = row.pricing.total_price;

    const int modelRow = jobOrder_.indexOf(row.metadata.job_id);
    if (modelRow >= 0) { dirtyRows_.insert(modelRow); }
  }

  // Phase 4 — end the insertion if we started one.
  if (needsInsert)
  {
    endInsertRows();
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
    row.lastUpdated = QDateTime::currentDateTimeUtc();

    if (event.total_pages > 0)
    {
      row.totalPages = event.total_pages;
      // Sync back to metadata so the detail panel reflects the latest values.
      row.metadata.total_pages =
        core::FieldValue{std::to_string(event.total_pages), core::FieldState::Reported};
    }
    row.pagesPrinted = event.pages_printed;
    // Sync back to metadata so the detail panel reflects the latest values.
    row.metadata.pages_printed =
      core::FieldValue{std::to_string(event.pages_printed), core::FieldState::Reported};

    row.pricing = common::compute_job_pricing(row.metadata, PricingConfig{}, common::PrinterProfileConfig{});
    row.price = row.pricing.total_price;

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
