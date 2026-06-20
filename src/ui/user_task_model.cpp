#include "ui/user_task_model.h"
#include "ui/pricing_config.h"
#include "ui/user_color_palette.h"

#include <QBrush>
#include <QColor>
#include <QDateTime>
#include <QFont>

namespace wpce::ui
{

// ── UserTask ────────────────────────────────────────────────────────

void UserTask::recomputeTotals()
{
  totalPages = 0;
  totalPrice = 0.0;
  for (auto it = jobCosts.cbegin(); it != jobCosts.cend(); ++it)
  {
    totalPages += it->pages;
    totalPrice += it->price;
  }
}

// ── UserTaskModel ───────────────────────────────────────────────────

UserTaskModel::UserTaskModel(QObject* parent) : QAbstractTableModel(parent) {}

int UserTaskModel::rowCount(const QModelIndex& parent) const
{
  if (parent.isValid()) return 0;
  return static_cast<int>(tasks_.size());
}

int UserTaskModel::columnCount(const QModelIndex& parent) const
{
  if (parent.isValid()) return 0;
  return ColumnCount;
}

QVariant UserTaskModel::data(const QModelIndex& index, int role) const
{
  if (!index.isValid() || index.row() < 0 || index.row() >= tasks_.size())
  {
    return {};
  }

  const auto& task = tasks_[index.row()];

  if (role == Qt::DisplayRole)
  {
    switch (index.column())
    {
      case ColUser:
        return task.userId;
      case ColDate:
        return task.date.toString(QStringLiteral("yyyy-MM-dd"));
      case ColJobCount:
        return task.jobIds.size();
      case ColTotalPages:
        return task.totalPages;
      case ColPrice:
        return QString::number(task.totalPrice, 'f', 2);
      case ColStatus:
        return task.completed ? QStringLiteral("已完成") : QStringLiteral("进行中");
    }
  }

  if (role == Qt::TextAlignmentRole)
  {
    switch (index.column())
    {
      case ColJobCount:
      case ColTotalPages:
        return static_cast<int>(Qt::AlignCenter);
      case ColPrice:
        return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
    }
  }

  // ── Per-user row coloring ────────────────────────────────────────
  if (role == Qt::BackgroundRole)
  {
    const QColor tint = userBackgroundColor(task.userId);
    return QBrush(QColor(
      (tint.red()   + 255 * 3) / 4,
      (tint.green() + 255 * 3) / 4,
      (tint.blue()  + 255 * 3) / 4
    ));
  }

  if (role == Qt::ForegroundRole && index.column() == ColStatus)
  {
    return task.completed ? QColor(0x06, 0x76, 0x47) : QColor(0x25, 0x63, 0xeb);
  }

  if (role == Qt::FontRole && index.column() == ColPrice)
  {
    QFont font;
    font.setFamily(QStringLiteral("Cascadia Mono"));
    font.setPointSize(10);
    return font;
  }

  return {};
}

QVariant UserTaskModel::headerData(int section, Qt::Orientation orientation, int role) const
{
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
  {
    return {};
  }

  switch (section)
  {
    case ColUser:
      return QStringLiteral("用户");
    case ColDate:
      return QStringLiteral("日期");
    case ColJobCount:
      return QStringLiteral("作业数");
    case ColTotalPages:
      return QStringLiteral("总页数");
    case ColPrice:
      return QStringLiteral("金额 (¥)");
    case ColStatus:
      return QStringLiteral("状态");
  }

  return {};
}

const UserTask* UserTaskModel::taskAtRow(int row) const
{
  if (row < 0 || row >= tasks_.size()) return nullptr;
  return &tasks_[row];
}

const UserTask* UserTaskModel::taskForUser(const QString& userId) const
{
  auto it = activeTaskIndex_.find(userId);
  if (it != activeTaskIndex_.end() && it.value() < tasks_.size())
  {
    return &tasks_[it.value()];
  }
  return nullptr;
}

int UserTaskModel::todayJobCount() const
{
  int count = 0;
  for (const auto& t : tasks_)
  {
    count += t.jobIds.size();
  }
  return count;
}

double UserTaskModel::todayRevenue() const
{
  double total = 0.0;
  for (const auto& t : tasks_)
  {
    total += t.totalPrice;
  }
  return total;
}

int UserTaskModel::activeUserCount() const
{
  return static_cast<int>(activeTaskIndex_.size());
}

int UserTaskModel::completedTaskCount() const
{
  int count = 0;
  for (const auto& t : tasks_)
  {
    if (t.completed) ++count;
  }
  return count;
}

// ── Slots ──────────────────────────────────────────────────────────

void UserTaskModel::onJobSnapshot(core::JobSnapshot snapshot)
{
  const QString userId = QString::fromStdString(snapshot.user).trimmed();
  if (userId.isEmpty()) return;

  const QDate today = QDate::currentDate();
  const std::uint32_t jobId = snapshot.job_id;

  // Check if this job is already mapped to a task.
  auto jobMapIt = jobToTaskRow_.find(jobId);
  if (jobMapIt != jobToTaskRow_.end() && jobMapIt.value() < tasks_.size())
  {
    // Job already tracked — just ensure it's in the jobIds list.
    auto& task = tasks_[jobMapIt.value()];
    if (!task.jobIds.contains(jobId))
    {
      task.jobIds.push_back(jobId);
      emit dataChanged(index(jobMapIt.value(), ColJobCount),
                       index(jobMapIt.value(), ColJobCount));
    }
    return;
  }

  // New job — bind to this user's active task or create one.
  auto activeIt = activeTaskIndex_.find(userId);
  if (activeIt != activeTaskIndex_.end() && activeIt.value() < tasks_.size())
  {
    auto& task = tasks_[activeIt.value()];
    if (!task.completed)
    {
      task.jobIds.push_back(jobId);
      jobToTaskRow_[jobId] = activeIt.value();
      emit dataChanged(index(activeIt.value(), ColJobCount),
                       index(activeIt.value(), ColStatus));
      return;
    }
  }

  // Create new task.
  const int newRow = static_cast<int>(tasks_.size());
  beginInsertRows(QModelIndex(), newRow, newRow);
  UserTask newTask;
  newTask.userId = userId;
  newTask.date = today;
  newTask.jobIds.push_back(jobId);
  tasks_.push_back(std::move(newTask));
  activeTaskIndex_[userId] = newRow;
  jobToTaskRow_[jobId] = newRow;
  endInsertRows();
}

void UserTaskModel::onMetadata(core::MetadataSnapshot metadata)
{
  const std::uint32_t jobId = metadata.job_id;

  auto jobMapIt = jobToTaskRow_.find(jobId);
  if (jobMapIt == jobToTaskRow_.end() || jobMapIt.value() >= tasks_.size())
  {
    return;
  }

  auto& task = tasks_[jobMapIt.value()];
  int pages = 0;
  try { pages = std::stoi(metadata.total_pages.value); } catch (...) {}

  JobCostEntry entry;
  entry.pages = pages > 0 ? pages : 1;
  entry.price = calculate_job_cost(metadata, PricingConfig{});
  task.jobCosts[jobId] = entry;
  task.recomputeTotals();

  const int row = jobMapIt.value();
  emit dataChanged(index(row, ColTotalPages), index(row, ColPrice));
}

void UserTaskModel::onLifecycleEvent(core::JobLifecycleEvent event)
{
  // Terminal phases could auto-complete, but we keep completion manual.
  (void)event;
}

void UserTaskModel::completeUserTask(const QString& userId)
{
  auto it = activeTaskIndex_.find(userId);
  if (it == activeTaskIndex_.end()) return;

  const int row = it.value();
  if (row < 0 || row >= tasks_.size()) return;

  tasks_[row].completed = true;
  // Move jobs from this completed task's job_id mappings so they
  // don't block new tasks for the same user.
  for (auto jid : tasks_[row].jobIds)
  {
    jobToTaskRow_.remove(jid);
  }
  activeTaskIndex_.erase(it);
  emit dataChanged(index(row, 0), index(row, ColumnCount - 1));
}

void UserTaskModel::clear()
{
  beginResetModel();
  tasks_.clear();
  activeTaskIndex_.clear();
  jobToTaskRow_.clear();
  endResetModel();
}

// ── Private ────────────────────────────────────────────────────────

int UserTaskModel::rowForUser(const QString& userId) const
{
  auto it = activeTaskIndex_.find(userId);
  if (it != activeTaskIndex_.end() && it.value() < tasks_.size())
  {
    return it.value();
  }
  return -1;
}

}  // namespace wpce::ui
