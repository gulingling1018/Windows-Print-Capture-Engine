/// @file user_group_model.cpp
/// User-centric grouped tree model implementation.

#include "ui/user_group_model.h"
#include "common/pricing_engine.h"
#include "ui/pricing_config.h"
#include "ui/user_color_palette.h"

#include <QBrush>
#include <QColor>
#include <QFont>

#include <algorithm>
#include <climits>
#include <cmath>

namespace wpce::ui
{
namespace
{

// ── Helpers ──────────────────────────────────────────────────────────

QString formatDisplayTime(const QString& isoTime)
{
  QDateTime dt = QDateTime::fromString(isoTime, Qt::ISODate);
  if (!dt.isValid())
    dt = QDateTime::fromString(isoTime, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
  if (dt.isValid())
    return dt.toString(QStringLiteral("HH:mm_yyyy/M/d"));
  return isoTime;
}

QString phaseDisplayName(core::JobLifecyclePhase phase) noexcept
{
  switch (phase)
  {
    case core::JobLifecyclePhase::Accepted:  return QStringLiteral("已接收");
    case core::JobLifecyclePhase::Spooling:  return QStringLiteral("假脱机");
    case core::JobLifecyclePhase::Printing:  return QStringLiteral("打印中");
    case core::JobLifecyclePhase::Completed: return QStringLiteral("已完成");
    case core::JobLifecyclePhase::Failed:    return QStringLiteral("失败");
    case core::JobLifecyclePhase::Cancelled: return QStringLiteral("已取消");
    case core::JobLifecyclePhase::Stalled:   return QStringLiteral("停滞");
    case core::JobLifecyclePhase::Unknown:
    default:                                 return QStringLiteral("未知");
  }
}

QColor phaseColor(core::JobLifecyclePhase phase) noexcept
{
  switch (phase)
  {
    case core::JobLifecyclePhase::Completed: return QColor(0x0f, 0x8a, 0x5f);
    case core::JobLifecyclePhase::Failed:    return QColor(0xd9, 0x2d, 0x20);
    case core::JobLifecyclePhase::Cancelled: return QColor(0x66, 0x70, 0x85);
    case core::JobLifecyclePhase::Stalled:   return QColor(0xb5, 0x47, 0x08);
    case core::JobLifecyclePhase::Printing:
    case core::JobLifecyclePhase::Spooling:  return QColor(0x25, 0x63, 0xeb);
    case core::JobLifecyclePhase::Accepted:  return QColor(0x7c, 0x3a, 0xed);
    case core::JobLifecyclePhase::Unknown:
    default:                                 return QColor(0x66, 0x70, 0x85);
  }
}

int progressPercent(const JobRowData& data) noexcept
{
  if (data.totalPages <= 0) return -1;
  int pct = (data.pagesPrinted * 100) / data.totalPages;
  return std::clamp(pct, 0, 100);
}

QString reportedValueOrDash(const core::FieldValue& field)
{
  if (field.state == core::FieldState::Reported && !field.value.empty())
    return QString::fromStdString(field.value);
  return QStringLiteral("-");
}

bool isDefaultPaper(const std::string& paperValue,
                    core::FieldState state)
{
  if (state != core::FieldState::Reported || paperValue.empty())
    return true;  // not reported → treat as default
  // A4 and B5 are the default papers (same base price).
  return (paperValue == "A4" || paperValue == "B5" ||
          paperValue == "a4" || paperValue == "b5");
}

int parseCopies(const core::FieldValue& copies)
{
  if (copies.state != core::FieldState::Reported || copies.value.empty())
    return 1;
  try { return std::stoi(copies.value); }
  catch (...) { return 1; }
}

int parsePages(const core::FieldValue& totalPages)
{
  if (totalPages.state != core::FieldState::Reported || totalPages.value.empty())
    return 0;
  try { return std::stoi(totalPages.value); }
  catch (...) { return 0; }
}

}  // namespace

// ── UserGroupModel ───────────────────────────────────────────────────

UserGroupModel::UserGroupModel(QObject* parent)
  : QAbstractItemModel(parent)
{
  flushTimer_ = new QTimer(this);
  flushTimer_->setInterval(160);
  flushTimer_->setSingleShot(true);
  connect(flushTimer_, &QTimer::timeout, this, &UserGroupModel::flushPendingUpdates);
}

// ── QAbstractItemModel: structure ─────────────────────────────────────

QModelIndex UserGroupModel::index(int row, int column,
                                   const QModelIndex& parent) const
{
  if (!hasIndex(row, column, parent))
    return {};

  if (!parent.isValid())
  {
    // Top-level: group nodes.
    if (row >= 0 && row < groups_.size())
      return createIndex(row, column, groups_.at(row));
  }
  else
  {
    // Parent is a group node; children are job nodes.
    auto* group = static_cast<GroupNode*>(parent.internalPointer());
    if (group != nullptr && group->type == GroupNode::Group &&
        row >= 0 && row < group->children.size())
    {
      return createIndex(row, column, group->children.at(row));
    }
  }

  return {};
}

QModelIndex UserGroupModel::parent(const QModelIndex& index) const
{
  if (!index.isValid()) return {};

  auto* node = static_cast<GroupNode*>(index.internalPointer());
  if (node == nullptr || node->parent == nullptr)
    return {};  // root items (groups) have no parent in the model

  // node is a Job; its parent is the Group.
  GroupNode* group = node->parent;
  int row = 0;
  for (int i = 0; i < groups_.size(); ++i)
  {
    if (groups_.at(i) == group) { row = i; break; }
  }

  return createIndex(row, 0, group);
}

int UserGroupModel::rowCount(const QModelIndex& parent) const
{
  if (!parent.isValid())
    return static_cast<int>(groups_.size());

  auto* node = static_cast<GroupNode*>(parent.internalPointer());
  if (node != nullptr && node->type == GroupNode::Group)
    return static_cast<int>(node->children.size());

  return 0;  // job nodes have no children
}

int UserGroupModel::columnCount(const QModelIndex& /*parent*/) const
{
  return ColumnCount;
}

Qt::ItemFlags UserGroupModel::flags(const QModelIndex& index) const
{
  if (!index.isValid()) return Qt::NoItemFlags;

  auto* node = static_cast<GroupNode*>(index.internalPointer());
  if (node == nullptr) return Qt::NoItemFlags;

  return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

// ── QAbstractItemModel: data ─────────────────────────────────────────

QVariant UserGroupModel::data(const QModelIndex& index, int role) const
{
  if (!index.isValid()) return {};

  auto* node = static_cast<GroupNode*>(index.internalPointer());
  if (node == nullptr) return {};

  const int col = index.column();

  if (node->type == GroupNode::Group)
  {
    // ── Group row ──────────────────────────────────────────────────
    const QString userId = node->userId;
    const auto& s = node->summary;

    switch (role)
    {
      case Qt::DisplayRole:
      {
        switch (col)
        {
          case ColName:       return userId.isEmpty() ? QStringLiteral("?") : userId;
          case ColPhase:      return node->completed ? QStringLiteral("已结算") : QStringLiteral("进行中");
          case ColProgress:
          {
            QString txt = QStringLiteral("%1 作业").arg(s.jobCount);
            if (s.totalPrice > 0.0)
              txt += QStringLiteral(" · ¥%1").arg(QString::number(s.totalPrice, 'f', 2));
            if (s.pendingCount > 0)
              txt += QStringLiteral(" · %1项待定价").arg(s.pendingCount);
            return txt;
          }
          case ColPages:      return QString::number(s.simplexPages + s.duplexPages + s.specialPaperPages);
          case ColColor:      return s.colorPages > 0 ? QStringLiteral("彩色 %1页").arg(s.colorPages) : QString();
          case ColDuplex:     return s.duplexSheets > 0 ? QString::number(s.duplexSheets) : QString();
          case ColPaper:      return s.specialItems.join(QStringLiteral(", "));
          case ColSubmitTime: return s.earliestJobTime.isValid()
            ? s.earliestJobTime.toString(QStringLiteral("HH:mm_yyyy/M/d")) : QString();
          default:            return {};
        }
      }
      case GroupTypeRole:       return QStringLiteral("group");
      case GroupCompletedRole:  return node->completed;
      case UserIdRole:          return userId;
      case RowColorRole:        return userBackgroundColor(userId);
      case RowTextColorRole:    return userTextColor(userId);
      case SortRole:
      {
        if (col == ColName)        return userId.toCaseFolded();
        if (col == ColProgress)    return s.jobCount;
        if (col == ColPages)       return s.simplexPages + s.duplexPages;
        if (col == ColSubmitTime)  return s.earliestJobTime;
        return {};
      }
      case Qt::TextAlignmentRole:
        return (col == ColName) ? Qt::AlignVCenter : Qt::AlignCenter;
      case Qt::ForegroundRole:
        return (col == ColPhase)
          ? (node->completed ? QColor(0x0f, 0x8a, 0x5f) : QColor(0x25, 0x63, 0xeb))
          : QVariant{};
      case Qt::BackgroundRole:
        return QBrush(subtleUserTint(userId));
      case Qt::FontRole:
      {
        if (col == ColName)
        {
          QFont f;
          f.setWeight(QFont::DemiBold);
          f.setPointSize(11);
          return f;
        }
        return {};
      }
      case FreshnessSecsRole:
        return -1;  // group rows aggregate — no single freshness
      case PricePendingRole:
        return s.pendingCount > 0;
      case PendingReasonRole:
        return s.pendingCount > 0 ? QStringLiteral("pending") : QString();
      default:
        return {};
    }
  }
  else
  {
    // ── Job row ─────────────────────────────────────────────────────
    const auto& jd = node->jobData;
    const QString userId = QString::fromStdString(jd.job.user).trimmed();

    switch (role)
    {
      case Qt::DisplayRole:
      {
        switch (col)
        {
          case ColName: return QString::fromStdString(jd.job.document_name);
          case ColPhase: return phaseDisplayName(jd.phase);
          case ColProgress:
          {
            int pct = progressPercent(jd);
            if (pct >= 0)
              return QStringLiteral("%1 / %2  %3%").arg(jd.pagesPrinted).arg(jd.totalPages).arg(pct);
            if (jd.metadata.pages_printed.state == core::FieldState::Reported)
              return QString::fromStdString(jd.metadata.pages_printed.value);
            return QStringLiteral("-");
          }
          case ColPages:  return reportedValueOrDash(jd.metadata.total_pages);
          case ColColor:
          {
            if (jd.metadata.color.state == core::FieldState::Reported)
            {
              const auto& v = jd.metadata.color.value;
              if (v == "color")      return QStringLiteral("彩");
              if (v == "monochrome") return QStringLiteral("黑");
              return QString::fromStdString(v);
            }
            return QStringLiteral("-");
          }
          case ColDuplex:
          {
            if (jd.metadata.duplex.state == core::FieldState::Reported)
            {
              const auto& v = jd.metadata.duplex.value;
              if (v == "simplex")               return QStringLiteral("单面");
              if (v == "long-edge (book)")      return QStringLiteral("长边");
              if (v == "short-edge (calendar)") return QStringLiteral("短边");
              return QString::fromStdString(v);
            }
            return QStringLiteral("-");
          }
          case ColPaper:      return reportedValueOrDash(jd.metadata.paper_size);
          case ColSubmitTime: return formatDisplayTime(QString::fromStdString(jd.job.submit_time_utc));
          default:            return {};
        }
      }
      case GroupTypeRole:       return QStringLiteral("job");
      case GroupCompletedRole:  return false;
      case PhaseKeyRole:        return QString::fromStdString(std::string(core::to_string(jd.phase)));
      case ProgressPercentRole: return progressPercent(jd);
      case UserIdRole:          return userId;
      case RowColorRole:        return userBackgroundColor(userId);
      case RowTextColorRole:    return userTextColor(userId);
      case SortRole:
      {
        if (col == ColName)       return QString::fromStdString(jd.job.document_name).toCaseFolded();
        if (col == ColPhase)      return QString::fromStdString(std::string(core::to_string(jd.phase)));
        if (col == ColProgress)   return progressPercent(jd);
        if (col == ColPages)      return jd.totalPages;
        if (col == ColSubmitTime) return QString::fromStdString(jd.job.submit_time_utc);
        return {};
      }
      case Qt::TextAlignmentRole:
        return (col == ColName || col == ColSubmitTime) ? Qt::AlignVCenter : Qt::AlignCenter;
      case Qt::ForegroundRole:
        return (col == ColPhase) ? QBrush(phaseColor(jd.phase)) : QVariant{};
      case Qt::BackgroundRole:
        return QBrush(subtleUserTint(userId));
      case Qt::FontRole:
      {
        if (col == ColName)
        {
          QFont f;
          f.setWeight(QFont::DemiBold);
          f.setUnderline(true);
          return f;
        }
        return {};
      }
      case FreshnessSecsRole:
      {
        if (!jd.lastUpdated.isValid()) return -1;
        return static_cast<int>(jd.lastUpdated.secsTo(QDateTime::currentDateTimeUtc()));
      }
      case PricePendingRole:
        return jd.pricing.price_pending;
      case PendingReasonRole:
        return QString::fromStdString(jd.pricing.pending_reason);
      default:
        return {};
    }
  }
}

QVariant UserGroupModel::headerData(int section, Qt::Orientation orientation,
                                     int role) const
{
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
    return {};

  switch (section)
  {
    case ColName:       return QStringLiteral("用户 / 文档");
    case ColPhase:      return QStringLiteral("阶段");
    case ColProgress:   return QStringLiteral("进度");
    case ColPages:      return QStringLiteral("页数");
    case ColColor:      return QStringLiteral("颜色");
    case ColDuplex:     return QStringLiteral("单|双面");
    case ColPaper:      return QStringLiteral("纸张");
    case ColSubmitTime: return QStringLiteral("时间");
    default:            return {};
  }
}

// ── Public accessors ──────────────────────────────────────────────────

int UserGroupModel::groupCount() const
{
  return static_cast<int>(groups_.size());
}

int UserGroupModel::totalJobCount() const
{
  int n = 0;
  for (auto* g : groups_) n += g->children.size();
  return n;
}

bool UserGroupModel::isGroupRow(const QModelIndex& index) const
{
  if (!index.isValid()) return false;
  auto* node = static_cast<GroupNode*>(index.internalPointer());
  return node != nullptr && node->type == GroupNode::Group;
}

bool UserGroupModel::isJobRow(const QModelIndex& index) const
{
  if (!index.isValid()) return false;
  auto* node = static_cast<GroupNode*>(index.internalPointer());
  return node != nullptr && node->type == GroupNode::Job;
}

QString UserGroupModel::userIdAt(const QModelIndex& index) const
{
  if (!index.isValid()) return {};
  auto* node = static_cast<GroupNode*>(index.internalPointer());
  if (node == nullptr) return {};
  if (node->type == GroupNode::Group) return node->userId;
  return QString::fromStdString(node->jobData.job.user).trimmed();
}

bool UserGroupModel::isGroupCompleted(const QModelIndex& index) const
{
  if (!index.isValid()) return false;
  auto* node = static_cast<GroupNode*>(index.internalPointer());
  return node != nullptr && node->type == GroupNode::Group && node->completed;
}

UserGroupSummary UserGroupModel::groupSummaryAt(const QModelIndex& index) const
{
  if (!index.isValid()) return {};
  auto* node = static_cast<GroupNode*>(index.internalPointer());
  if (node != nullptr && node->type == GroupNode::Group)
    return node->summary;
  return {};
}

std::optional<JobRowData> UserGroupModel::jobDataAt(const QModelIndex& index) const
{
  if (!index.isValid()) return std::nullopt;
  auto* node = static_cast<GroupNode*>(index.internalPointer());
  if (node != nullptr && node->type == GroupNode::Job)
    return node->jobData;
  return std::nullopt;
}

QModelIndex UserGroupModel::groupIndexForUser(const QString& userId) const
{
  for (int i = 0; i < groups_.size(); ++i)
  {
    if (groups_.at(i)->userId == userId)
      return createIndex(i, 0, groups_.at(i));
  }
  return {};
}

// ── Data ingestion ────────────────────────────────────────────────────

void UserGroupModel::onJobSnapshot(core::JobSnapshot snapshot)
{
  if (snapshot.job_id == 0) return;

  const QString userId = QString::fromStdString(snapshot.user).trimmed();
  if (userId.isEmpty()) return;

  const std::uint32_t jobId = snapshot.job_id;

  {
    std::scoped_lock lock(dataMutex_);

    // Check if job already exists.
    auto* existingJob = findJob(jobId);
    if (existingJob != nullptr)
    {
      existingJob->jobData.job = std::move(snapshot);
      existingJob->jobData.lastUpdated = QDateTime::currentDateTimeUtc();
      markDirty(existingJob);
      return;
    }

    // Find or create the user group.
    GroupNode* group = findGroup(userId);
    int groupRow = -1;

    if (group == nullptr)
    {
      // New user — insert group in submission-time order.
      // Find insertion position (oldest first = reverse chronological in submission).
      const QString submitTime = QString::fromStdString(snapshot.submit_time_utc);
      groupRow = 0;
      for (int i = 0; i < groups_.size(); ++i)
      {
        if (groups_.at(i)->summary.earliestJobTime.toString(Qt::ISODate) > submitTime)
          break;
        groupRow = i + 1;
      }
      group = insertGroupNode(userId, groupRow);
    }
    else
    {
      // Find the group's row.
      for (int i = 0; i < groups_.size(); ++i)
      {
        if (groups_.at(i) == group) { groupRow = i; break; }
      }
    }

    // Insert job child under group (appended at end).
    int childRow = static_cast<int>(group->children.size());
    insertJobNode(group, childRow, std::move(snapshot));

    // Recompute group summary.
    recomputeGroupSummary(group);
    markDirty(group);
  }

  if (!flushTimer_->isActive())
    QMetaObject::invokeMethod(flushTimer_, "start", Qt::QueuedConnection);
}

void UserGroupModel::onMetadata(core::MetadataSnapshot metadata)
{
  if (metadata.job_id == 0) return;

  {
    std::scoped_lock lock(dataMutex_);

    auto* jobNode = findJob(metadata.job_id);
    if (jobNode == nullptr) return;

    auto& row = jobNode->jobData;
    row.metadata = std::move(metadata);
    row.lastUpdated = QDateTime::currentDateTimeUtc();

    try { row.totalPages = std::stoi(row.metadata.total_pages.value); }
    catch (...) { row.totalPages = 0; }
    try { row.pagesPrinted = std::stoi(row.metadata.pages_printed.value); }
    catch (...) { row.pagesPrinted = 0; }

    row.pricing = common::compute_job_pricing(row.metadata,
        pricingConfig_ ? *pricingConfig_ : PricingConfig{},
        common::PrinterProfileConfig{});
    row.price = row.pricing.total_price;

    // Recompute the parent group's summary.
    if (jobNode->parent != nullptr)
    {
      recomputeGroupSummary(jobNode->parent);
      markDirty(jobNode->parent);
    }

    markDirty(jobNode);
  }

  if (!flushTimer_->isActive())
    QMetaObject::invokeMethod(flushTimer_, "start", Qt::QueuedConnection);
}

void UserGroupModel::onLifecycleEvent(core::JobLifecycleEvent event)
{
  if (event.job_id == 0) return;

  {
    std::scoped_lock lock(dataMutex_);

    auto* jobNode = findJob(event.job_id);
    if (jobNode == nullptr) return;

    auto& row = jobNode->jobData;
    row.phase = event.phase;
    row.lastUpdated = QDateTime::currentDateTimeUtc();

    if (event.total_pages > 0)
    {
      row.totalPages = event.total_pages;
      row.metadata.total_pages =
        core::FieldValue{std::to_string(event.total_pages), core::FieldState::Reported};
    }
    row.pagesPrinted = event.pages_printed;
    row.metadata.pages_printed =
      core::FieldValue{std::to_string(event.pages_printed), core::FieldState::Reported};

    row.pricing = common::compute_job_pricing(row.metadata,
        pricingConfig_ ? *pricingConfig_ : PricingConfig{},
        common::PrinterProfileConfig{});
    row.price = row.pricing.total_price;

    row.lifecycleEvents.push_back(std::move(event));

    if (jobNode->parent != nullptr)
    {
      recomputeGroupSummary(jobNode->parent);
      markDirty(jobNode->parent);
    }

    markDirty(jobNode);
  }

  if (!flushTimer_->isActive())
    QMetaObject::invokeMethod(flushTimer_, "start", Qt::QueuedConnection);
}

// ── Operator actions ──────────────────────────────────────────────────

void UserGroupModel::completeUser(const QString& userId)
{
  int totalGroups = 0;
  int activeGroups = 0;

  {
    std::scoped_lock lock(dataMutex_);

    GroupNode* group = findGroup(userId);
    if (group == nullptr || group->completed) return;

    group->completed = true;
    markDirty(group);

    totalGroups = static_cast<int>(groups_.size());
    activeGroups = static_cast<int>(std::count_if(groups_.begin(), groups_.end(),
      [](GroupNode* g) { return !g->completed; }));
  }

  flushTimer_->stop();
  flushPendingUpdates();

  emit userCompleted(userId);
  emit groupCountChanged(totalGroups, activeGroups);
}

void UserGroupModel::clear()
{
  flushTimer_->stop();

  beginResetModel();
  {
    std::scoped_lock lock(dataMutex_);
    for (auto* group : groups_)
    {
      qDeleteAll(group->children);
      delete group;
    }
    groups_.clear();
    groupMap_.clear();
    jobMap_.clear();
    dirtyNodes_.clear();
  }
  endResetModel();
}

// ── Flush ─────────────────────────────────────────────────────────────

void UserGroupModel::flushPendingUpdates()
{
  // Collect dirty nodes, compute row ranges.
  struct Range { int top; int bottom; QModelIndex parent; };
  QVector<Range> ranges;

  {
    std::scoped_lock lock(dataMutex_);

    QSet<GroupNode*> processed;
    for (auto* node : dirtyNodes_)
    {
      if (processed.contains(node)) continue;
      processed.insert(node);

      if (node->type == GroupNode::Group)
      {
        // Find this group's row in the top-level list.
        int row = -1;
        for (int i = 0; i < groups_.size(); ++i)
        {
          if (groups_.at(i) == node) { row = i; break; }
        }
        if (row >= 0)
          ranges.append({row, row, QModelIndex{}});
      }
      else
      {
        // Job node — find its row under its parent.
        auto* parent = node->parent;
        if (parent != nullptr)
        {
          int pRow = -1;
          for (int i = 0; i < groups_.size(); ++i)
          {
            if (groups_.at(i) == parent) { pRow = i; break; }
          }
          if (pRow >= 0)
          {
            int jRow = -1;
            for (int i = 0; i < parent->children.size(); ++i)
            {
              if (parent->children.at(i) == node) { jRow = i; break; }
            }
            if (jRow >= 0)
              ranges.append({jRow, jRow,
                createIndex(pRow, 0, parent)});
          }
        }
      }
    }
    dirtyNodes_.clear();
  }

  // Emit dataChanged for each affected range.
  for (const auto& r : ranges)
  {
    emit dataChanged(index(r.top, 0, r.parent),
                     index(r.bottom, ColumnCount - 1, r.parent));
  }
}

// ── Forward-declared helper used by recomputeGroupSummary ───────────
static bool is_high_quality_meta(const GroupNode* group)
{
  for (auto* child : group->children)
  {
    if (is_high_quality(child->jobData.metadata))
      return true;
  }
  return false;
}

// ── Internal helpers ──────────────────────────────────────────────────

GroupNode* UserGroupModel::findGroup(const QString& userId)
{
  auto it = groupMap_.constFind(userId);
  return (it != groupMap_.constEnd()) ? it.value() : nullptr;
}

GroupNode* UserGroupModel::findJob(std::uint32_t jobId)
{
  auto it = jobMap_.constFind(jobId);
  return (it != jobMap_.constEnd()) ? it.value() : nullptr;
}

void UserGroupModel::recomputeGroupSummary(GroupNode* group)
{
  if (group == nullptr || group->type != GroupNode::Group) return;

  UserGroupSummary s;
  QMap<QString, int> specialPaperMap;  // paper_name -> page count
  int totalColorOnDefault = 0;

  s.jobCount = static_cast<int>(group->children.size());

  for (auto* child : group->children)
  {
    const auto& meta = child->jobData.metadata;
    int pages = parsePages(meta.total_pages);
    int copies = parseCopies(meta.copies);
    int effectivePages = pages * copies;

    bool isDuplex = is_duplex_job(meta);
    bool isColor = is_color_job(meta);
    bool isDefault = isDefaultPaper(meta.paper_size.value, meta.paper_size.state);

    // Paper name for display.
    QString paperName;
    if (meta.paper_size.state == core::FieldState::Reported && !meta.paper_size.value.empty())
      paperName = QString::fromStdString(meta.paper_size.value);

    if (isDuplex)
    {
      if (isDefault)
      {
        s.duplexPages += effectivePages;
        s.duplexSheets += static_cast<int>(std::ceil(effectivePages / 2.0));
        if (isColor) totalColorOnDefault += effectivePages;
      }
      else
      {
        specialPaperMap[paperName] += effectivePages;
        s.specialPaperPages += effectivePages;
      }
    }
    else
    {
      if (isDefault)
      {
        s.simplexPages += effectivePages;
        if (isColor) totalColorOnDefault += effectivePages;
      }
      else
      {
        specialPaperMap[paperName] += effectivePages;
        s.specialPaperPages += effectivePages;
      }
    }

    // Aggregate pricing.
    s.totalPrice += child->jobData.pricing.total_price;
    if (child->jobData.pricing.price_pending)
      ++s.pendingCount;

    // Track earliest job time.
    QDateTime t = QDateTime::fromString(
      QString::fromStdString(child->jobData.job.submit_time_utc), Qt::ISODate);
    if (t.isValid() && (!s.earliestJobTime.isValid() || t < s.earliestJobTime))
      s.earliestJobTime = t;
  }

  s.colorPages = totalColorOnDefault;

  // Build special items list.
  if (totalColorOnDefault > 0)
    s.specialItems.append(QStringLiteral("彩色 %1页").arg(totalColorOnDefault));
  for (auto it = specialPaperMap.begin(); it != specialPaperMap.end(); ++it)
    s.specialItems.append(QStringLiteral("%1 %2页").arg(it.key()).arg(it.value()));
  if (is_high_quality_meta(group))
    s.specialItems.append(QStringLiteral("高质量"));

  group->summary = s;
}

GroupNode* UserGroupModel::insertGroupNode(const QString& userId, int row)
{
  // Notify Qt BEFORE inserting.
  beginInsertRows(QModelIndex(), row, row);

  auto* node = new GroupNode;
  node->type = GroupNode::Group;
  node->userId = userId;
  node->completed = false;

  groups_.insert(row, node);
  groupMap_[userId] = node;

  endInsertRows();
  emit groupCountChanged(
    static_cast<int>(groups_.size()),
    static_cast<int>(std::count_if(groups_.begin(), groups_.end(),
      [](GroupNode* g) { return !g->completed; })));

  return node;
}

void UserGroupModel::insertJobNode(GroupNode* group, int row,
                                    core::JobSnapshot snapshot)
{
  QModelIndex groupIdx;
  for (int i = 0; i < groups_.size(); ++i)
  {
    if (groups_.at(i) == group) { groupIdx = createIndex(i, 0, group); break; }
  }

  beginInsertRows(groupIdx, row, row);

  auto* node = new GroupNode;
  node->type = GroupNode::Job;
  node->parent = group;
  node->jobId = snapshot.job_id;
  node->jobData.job = std::move(snapshot);
  node->jobData.lastUpdated = QDateTime::currentDateTimeUtc();

  group->children.insert(row, node);
  jobMap_[node->jobId] = node;

  endInsertRows();
}

void UserGroupModel::markDirty(GroupNode* node)
{
  dirtyNodes_.insert(node);
}

}  // namespace wpce::ui
