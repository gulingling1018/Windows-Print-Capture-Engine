#include "ui/user_task_model.h"
#include "common/pricing_engine.h"
#include "ui/pricing_config.h"
#include "ui/user_color_palette.h"

#include <QBrush>
#include <QColor>
#include <QDateTime>
#include <QFile>
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

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

UserTaskModel::UserTaskModel(QObject* parent) : QAbstractTableModel(parent)
{
  defaultPricing_ = common::PricingConfig{};
  pricing_ = &defaultPricing_;

  // Debounced auto-save: fire 2 s after the last change.
  autoSaveTimer_.setSingleShot(true);
  autoSaveTimer_.setInterval(2000);
  connect(&autoSaveTimer_, &QTimer::timeout, this, &UserTaskModel::flushSave);
}

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
      case ColTaskId:
        return task.taskId;
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
      case ColTaskId:
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
    case ColTaskId:
      return QStringLiteral("任务ID");
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

// ── Persistence ─────────────────────────────────────────────────────

QJsonObject UserTaskModel::toJson() const
{
  QJsonObject root;
  QJsonArray taskArray;
  for (const auto& task : tasks_)
  {
    QJsonObject tj;
    tj[QStringLiteral("taskId")] = task.taskId;
    tj[QStringLiteral("userId")] = task.userId;
    tj[QStringLiteral("date")] = task.date.toString(Qt::ISODate);
    tj[QStringLiteral("completed")] = task.completed;
    tj[QStringLiteral("totalPages")] = task.totalPages;
    tj[QStringLiteral("totalPrice")] = task.totalPrice;

    QJsonArray jobIdArray;
    for (auto jid : task.jobIds)
    {
      jobIdArray.append(static_cast<int>(jid));
    }
    tj[QStringLiteral("jobIds")] = jobIdArray;

    QJsonObject costObj;
    for (auto it = task.jobCosts.cbegin(); it != task.jobCosts.cend(); ++it)
    {
      QJsonObject entry;
      entry[QStringLiteral("pages")] = it->pages;
      entry[QStringLiteral("price")] = it->price;
      costObj[QString::number(it.key())] = entry;
    }
    tj[QStringLiteral("jobCosts")] = costObj;

    taskArray.append(tj);
  }
  root[QStringLiteral("tasks")] = taskArray;

  // Save counter state so IDs don't reset across restarts.
  root[QStringLiteral("dailyTaskCounter")] = dailyTaskCounter_;
  if (lastTaskDate_.isValid())
  {
    root[QStringLiteral("lastTaskDate")] = lastTaskDate_.toString(Qt::ISODate);
  }

  return root;
}

bool UserTaskModel::fromJson(const QJsonObject& json)
{
  clear();

  const QJsonArray taskArray = json[QStringLiteral("tasks")].toArray();

  // Pass 1: restore task list.
  for (const QJsonValue& val : taskArray)
  {
    const QJsonObject tj = val.toObject();

    UserTask task;
    task.taskId   = tj[QStringLiteral("taskId")].toString();
    task.userId   = tj[QStringLiteral("userId")].toString();
    task.date     = QDate::fromString(tj[QStringLiteral("date")].toString(), Qt::ISODate);
    task.completed = tj[QStringLiteral("completed")].toBool();
    task.totalPages = tj[QStringLiteral("totalPages")].toInt();
    task.totalPrice = tj[QStringLiteral("totalPrice")].toDouble();

    const QJsonArray jids = tj[QStringLiteral("jobIds")].toArray();
    for (const QJsonValue& jv : jids)
    {
      task.jobIds.push_back(static_cast<std::uint32_t>(jv.toInt()));
    }

    const QJsonObject costObj = tj[QStringLiteral("jobCosts")].toObject();
    for (auto cit = costObj.constBegin(); cit != costObj.constEnd(); ++cit)
    {
      const std::uint32_t jid = static_cast<std::uint32_t>(cit.key().toUInt());
      const QJsonObject entry = cit.value().toObject();
      JobCostEntry cost;
      cost.pages = entry[QStringLiteral("pages")].toInt();
      cost.price = entry[QStringLiteral("price")].toDouble();
      task.jobCosts[jid] = cost;
    }

    const int row = static_cast<int>(tasks_.size());
    tasks_.push_back(std::move(task));

    // Build active index.
    if (!tasks_.back().completed)
    {
      activeTaskIndex_[tasks_.back().userId] = row;
    }
    // Build job→task mapping.
    for (auto jid : tasks_.back().jobIds)
    {
      jobToTaskRow_[jid] = row;
    }
  }

  // Restore counter.
  dailyTaskCounter_ = json[QStringLiteral("dailyTaskCounter")].toInt(0);
  const QString dateStr = json[QStringLiteral("lastTaskDate")].toString();
  if (!dateStr.isEmpty())
  {
    lastTaskDate_ = QDate::fromString(dateStr, Qt::ISODate);
  }

  if (!tasks_.empty())
  {
    beginInsertRows(QModelIndex(), 0, static_cast<int>(tasks_.size()) - 1);
    endInsertRows();
  }

  return true;
}

bool UserTaskModel::saveToJson(const QString& filePath) const
{
  const QJsonObject json = toJson();
  QJsonDocument doc(json);
  const QByteArray data = doc.toJson(QJsonDocument::Indented);

  // Atomic write via temp + rename.
  const QString tempPath = filePath + QStringLiteral(".tmp");
  {
    QFile file(tempPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
      return false;
    }
    if (file.write(data) != data.size())
    {
      file.close();
      QFile::remove(tempPath);
      return false;
    }
    file.close();
  }

  // Rename.
  QFile::remove(filePath);
  if (!QFile::rename(tempPath, filePath))
  {
    // Fallback: copy + remove.
    if (!QFile::copy(tempPath, filePath))
    {
      QFile::remove(tempPath);
      return false;
    }
    QFile::remove(tempPath);
  }
  return true;
}

bool UserTaskModel::loadFromJson(const QString& filePath)
{
  QFile file(filePath);
  if (!file.open(QIODevice::ReadOnly))
  {
    return false;
  }

  const QByteArray data = file.readAll();
  file.close();

  QJsonParseError parseError;
  const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
  if (parseError.error != QJsonParseError::NoError)
  {
    return false;
  }

  return fromJson(doc.object());
}

void UserTaskModel::setPricingConfig(const common::PricingConfig* pricing)
{
  if (pricing != nullptr)
  {
    pricing_ = pricing;
  }
  else
  {
    defaultPricing_ = common::PricingConfig{};
    pricing_ = &defaultPricing_;
  }
}

void UserTaskModel::setAutoSavePath(const QString& filePath)
{
  autoSavePath_ = filePath;
}

QString UserTaskModel::autoSavePath() const
{
  return autoSavePath_;
}

void UserTaskModel::scheduleAutoSave()
{
  if (!autoSavePath_.isEmpty())
  {
    dirty_ = true;
    autoSaveTimer_.start();
  }
}

void UserTaskModel::flushSave()
{
  if (!dirty_ || autoSavePath_.isEmpty()) return;
  dirty_ = false;
  (void)saveToJson(autoSavePath_);
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
      scheduleAutoSave();
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
      scheduleAutoSave();
      return;
    }
  }

  // Create new task — always at the top so newest billing items are visible first.
  beginInsertRows(QModelIndex(), 0, 0);

  // Shift existing row indices up by one to make room at row 0.
  for (auto it = activeTaskIndex_.begin(); it != activeTaskIndex_.end(); ++it)
    ++(it.value());
  for (auto it = jobToTaskRow_.begin(); it != jobToTaskRow_.end(); ++it)
    ++(it.value());

  UserTask newTask;
  newTask.taskId = generateTaskId(today);
  newTask.userId = userId;
  newTask.date = today;
  newTask.jobIds.push_back(jobId);
  tasks_.insert(tasks_.begin(), std::move(newTask));
  activeTaskIndex_[userId] = 0;
  jobToTaskRow_[jobId] = 0;
  endInsertRows();

  scheduleAutoSave();
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

  // Use the configured pricing (live from AppConfig), or built-in defaults.
  const PricingConfig& pc = pricing_ ? *pricing_ : defaultPricing_;
  auto pricing = common::compute_job_pricing(metadata, pc, common::PrinterProfileConfig{});
  entry.price = pricing.total_price;

  task.jobCosts[jobId] = entry;
  task.recomputeTotals();

  const int row = jobMapIt.value();
  emit dataChanged(index(row, ColTotalPages), index(row, ColPrice));

  scheduleAutoSave();
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

  scheduleAutoSave();
}

void UserTaskModel::clear()
{
  // Flush any pending save before clearing.
  if (dirty_ && !autoSavePath_.isEmpty())
  {
    flushSave();
  }

  beginResetModel();
  tasks_.clear();
  activeTaskIndex_.clear();
  jobToTaskRow_.clear();
  dailyTaskCounter_ = 0;
  lastTaskDate_ = QDate();
  endResetModel();
  dirty_ = false;
}

// ── Private ────────────────────────────────────────────────────────

QString UserTaskModel::generateTaskId(const QDate& date)
{
  if (date != lastTaskDate_)
  {
    dailyTaskCounter_ = 0;
    lastTaskDate_ = date;
  }
  ++dailyTaskCounter_;
  return date.toString(QStringLiteral("yyyyMMdd-"))
    + QStringLiteral("%1").arg(dailyTaskCounter_, 3, 10, QLatin1Char('0'));
}

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
