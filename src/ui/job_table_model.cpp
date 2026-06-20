#include "ui/job_table_model.h"

#include "watcher/print_watcher.h"

#include <cstdint>

namespace wpce::ui
{

namespace
{

QString phaseIcon(core::JobLifecyclePhase phase) noexcept
{
  switch (phase)
  {
    case core::JobLifecyclePhase::Accepted:  return QStringLiteral("\xe2\x9e\xa4");   // ➤
    case core::JobLifecyclePhase::Spooling:  return QStringLiteral("\xe2\x8f\xb3");   // ⏳
    case core::JobLifecyclePhase::Printing:  return QStringLiteral("\xf0\x9f\x96\xa8"); // 🖨
    case core::JobLifecyclePhase::Completed: return QStringLiteral("\xe2\x9c\x93");   // ✓
    case core::JobLifecyclePhase::Failed:    return QStringLiteral("\xe2\x9c\x97");   // ✗
    case core::JobLifecyclePhase::Cancelled: return QStringLiteral("\xe2\x9c\x82");   // ✂
    case core::JobLifecyclePhase::Stalled:   return QStringLiteral("\xe2\x9a\xa0");   // ⚠
    default:                                 return QStringLiteral("?");
  }
}

QString phaseName(core::JobLifecyclePhase phase) noexcept
{
  return QString::fromUtf8(core::to_string(phase).data());
}

}  // namespace

JobTableModel::JobTableModel(QObject* parent) : QAbstractTableModel(parent)
{
  flushTimer_ = new QTimer(this);
  flushTimer_->setInterval(200);
  flushTimer_->setSingleShot(true);
  connect(flushTimer_, &QTimer::timeout, this, &JobTableModel::flushPendingUpdates);
}

int JobTableModel::rowCount(const QModelIndex& parent) const
{
  if (parent.isValid()) return 0;
  return static_cast<int>(jobOrder_.size());
}

int JobTableModel::columnCount(const QModelIndex& parent) const
{
  if (parent.isValid()) return 0;
  return ColumnCount;
}

QVariant JobTableModel::data(const QModelIndex& index, int role) const
{
  if (!index.isValid() || role != Qt::DisplayRole)
  {
    return {};
  }

  const int row = index.row();
  const int col = index.column();

  if (row < 0 || row >= jobOrder_.size())
  {
    return {};
  }

  std::scoped_lock lock(dataMutex_);

  const std::uint32_t jobId = jobOrder_.at(row);
  auto it = jobs_.find(jobId);
  if (it == jobs_.end())
  {
    return {};
  }

  const JobRowData& d = it.value();

  switch (col)
  {
    case ColJobId:
      return d.job.job_id;

    case ColDocument:
      return QString::fromStdString(d.job.document_name);

    case ColUser:
      return QString::fromStdString(d.job.user);

    case ColPhase:
      return QStringLiteral("%1 %2")
        .arg(phaseIcon(d.phase), phaseName(d.phase));

    case ColProgress:
      if (d.totalPages > 0)
      {
        const int pct = (d.pagesPrinted * 100) / d.totalPages;
        return QStringLiteral("%1/%2 (%3%)")
          .arg(d.pagesPrinted).arg(d.totalPages).arg(pct);
      }
      if (d.metadata.pages_printed.state == core::FieldState::Reported)
      {
        return QString::fromStdString(d.metadata.pages_printed.value);
      }
      return QStringLiteral("-");

    case ColTotalPages:
      if (d.metadata.total_pages.state == core::FieldState::Reported)
      {
        return QString::fromStdString(d.metadata.total_pages.value);
      }
      return QStringLiteral("-");

    case ColCopies:
      if (d.metadata.copies.state == core::FieldState::Reported)
      {
        return QString::fromStdString(d.metadata.copies.value);
      }
      return QStringLiteral("-");

    case ColSubmitTime:
      return QString::fromStdString(d.job.submit_time_utc);

    default:
      return {};
  }
}

QVariant JobTableModel::headerData(int section, Qt::Orientation orientation,
                                   int role) const
{
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
  {
    return {};
  }

  switch (section)
  {
    case ColJobId:       return QStringLiteral("Job ID");
    case ColDocument:    return QStringLiteral("\xe6\x96\x87\xe6\xa1\xa3");   // 文档
    case ColUser:        return QStringLiteral("\xe7\x94\xa8\xe6\x88\xb7");   // 用户
    case ColPhase:       return QStringLiteral("\xe9\x98\xb6\xe6\xae\xb5");   // 阶段
    case ColProgress:    return QStringLiteral("\xe8\xbf\x9b\xe5\xba\xa6");   // 进度
    case ColTotalPages:  return QStringLiteral("\xe9\xa1\xb5\xe6\x95\xb0");   // 页数
    case ColCopies:      return QStringLiteral("\xe4\xbb\xbd\xe6\x95\xb0");   // 份数
    case ColSubmitTime:  return QStringLiteral("\xe6\x97\xb6\xe9\x97\xb4");   // 时间
    default:             return {};
  }
}

const JobRowData* JobTableModel::jobAtRow(int row) const
{
  std::scoped_lock lock(dataMutex_);
  if (row < 0 || row >= jobOrder_.size()) return nullptr;

  auto it = jobs_.find(jobOrder_.at(row));
  if (it == jobs_.end()) return nullptr;

  return &it.value();
}

// ── Slot: onJobSnapshot ───────────────────────────────────────────────

void JobTableModel::onJobSnapshot(core::JobSnapshot snapshot)
{
  if (snapshot.job_id == 0) return;

  {
    std::scoped_lock lock(dataMutex_);

    auto& row = jobs_[snapshot.job_id];
    row.job = std::move(snapshot);

    const int modelRow = jobOrder_.indexOf(row.job.job_id);
    if (modelRow < 0)
    {
      // New job — insert at end.
      const int newRow = static_cast<int>(jobOrder_.size());
      beginInsertRows(QModelIndex(), newRow, newRow);
      jobOrder_.append(row.job.job_id);
      endInsertRows();
    }
    else
    {
      dirtyRows_.insert(modelRow);
    }
  }

  // Schedule a flush.
  if (!flushTimer_->isActive())
  {
    QMetaObject::invokeMethod(flushTimer_, "start", Qt::QueuedConnection);
  }
}

// ── Slot: onMetadata ──────────────────────────────────────────────────

void JobTableModel::onMetadata(core::MetadataSnapshot metadata)
{
  if (metadata.job_id == 0) return;

  {
    std::scoped_lock lock(dataMutex_);

    auto& row = jobs_[metadata.job_id];
    row.metadata = std::move(metadata);

    // Update pages from metadata.
    try { row.totalPages = std::stoi(row.metadata.total_pages.value); }
    catch (...) { row.totalPages = 0; }
    try { row.pagesPrinted = std::stoi(row.metadata.pages_printed.value); }
    catch (...) { row.pagesPrinted = 0; }

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

// ── Slot: onLifecycleEvent ────────────────────────────────────────────

void JobTableModel::onLifecycleEvent(core::JobLifecycleEvent event)
{
  if (event.job_id == 0) return;

  {
    std::scoped_lock lock(dataMutex_);

    auto& row = jobs_[event.job_id];
    row.phase = event.phase;

    // Pages from lifecycle event may be more up-to-date than metadata.
    if (event.total_pages > 0)
    {
      row.totalPages = event.total_pages;
    }
    row.pagesPrinted = event.pages_printed;

    const int modelRow = jobOrder_.indexOf(event.job_id);
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

// ── Flush ─────────────────────────────────────────────────────────────

void JobTableModel::flushPendingUpdates()
{
  QSet<int> rows;
  {
    std::scoped_lock lock(dataMutex_);
    rows.swap(dirtyRows_);
  }

  if (rows.isEmpty()) return;

  int minRow = INT_MAX;
  int maxRow = -1;
  for (int r : rows)
  {
    if (r < minRow) minRow = r;
    if (r > maxRow) maxRow = r;
  }

  if (minRow <= maxRow)
  {
    emit dataChanged(index(minRow, 0), index(maxRow, ColumnCount - 1));
  }
}

}  // namespace wpce::ui
