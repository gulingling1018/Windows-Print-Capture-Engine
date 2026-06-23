/// @file billing_batch_model.cpp
/// BillingBatchModel implementation.

#include "ui/billing_batch_model.h"
#include "ui/user_color_palette.h"

#include <QBrush>
#include <QColor>
#include <QFont>

#include <algorithm>

namespace wpce::ui
{

namespace
{

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

QString stateBadge(domain::BatchState state)
{
  switch (state)
  {
    case domain::BatchState::Active:    return QStringLiteral("进行中");
    case domain::BatchState::Suspended: return QStringLiteral("挂单");
    case domain::BatchState::Settled:   return QStringLiteral("已结算");
    case domain::BatchState::Voided:    return QStringLiteral("已作废");
  }
  return QStringLiteral("?");
}

QColor stateColor(domain::BatchState state)
{
  switch (state)
  {
    case domain::BatchState::Active:    return QColor(0x25, 0x63, 0xeb);
    case domain::BatchState::Suspended: return QColor(0xb5, 0x47, 0x08);
    case domain::BatchState::Settled:   return QColor(0x06, 0x76, 0x47);
    case domain::BatchState::Voided:    return QColor(0x66, 0x70, 0x85);
  }
  return QColor(0x66, 0x70, 0x85);
}

QString formatDisplayTime(const QString& isoTime)
{
  QDateTime dt = QDateTime::fromString(isoTime, Qt::ISODate);
  if (!dt.isValid())
    dt = QDateTime::fromString(isoTime, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
  if (dt.isValid())
    return dt.toString(QStringLiteral("HH:mm yyyy/M/d"));
  return isoTime;
}

QString duplexLabel(const core::MetadataSnapshot& meta)
{
  if (meta.duplex.state != core::FieldState::Reported) return QStringLiteral("-");
  const auto& v = meta.duplex.value;
  if (v == "simplex")               return QStringLiteral("单面");
  if (v == "long-edge (book)" ||
      v == "TwoSidedLongEdge" ||
      v == "vertical")              return QStringLiteral("长边");
  if (v == "short-edge (calendar)" ||
      v == "TwoSidedShortEdge" ||
      v == "horizontal")            return QStringLiteral("短边");
  if (v == "duplex")                return QStringLiteral("双面");
  return QString::fromStdString(v);
}

int progressPercent(const domain::PrintJobRecord& rec) noexcept
{
  if (rec.pricing.total_pages <= 0) return -1;
  int printed = 0;
  try { printed = std::stoi(rec.metadata.pages_printed.value); } catch (...) {}
  if (printed <= 0) return -1;
  int pct = (printed * 100) / rec.pricing.total_pages;
  return std::clamp(pct, 0, 100);
}

}  // namespace

BillingBatchModel::BillingBatchModel(BillingService* service, QObject* parent)
  : QAbstractItemModel(parent), service_(service)
{
  connect(service_, &BillingService::batchesChanged, this, &BillingBatchModel::onBatchesChanged);
  connect(service_, &BillingService::batchUpdated, this, &BillingBatchModel::onBatchUpdated);
  rebuildOrdering();
}

bool BillingBatchModel::orderLess(const domain::BillingBatch& a,
                                   const domain::BillingBatch& b) const
{
  // Order: Active (current first) → Suspended (recent first) → Settled (recent first)
  const auto rank = [](domain::BatchState s) -> int {
    switch (s)
    {
      case domain::BatchState::Active:    return 0;
      case domain::BatchState::Suspended: return 1;
      case domain::BatchState::Settled:   return 2;
      case domain::BatchState::Voided:    return 3;
    }
    return 4;
  };
  const int ra = rank(a.state), rb = rank(b.state);
  if (ra != rb) return ra < rb;

  // Within Active: current active batch first, then by period_start desc.
  const auto* cur = service_->currentActiveBatch();
  const bool aCur = cur != nullptr && a.batch_id == cur->batch_id;
  const bool bCur = cur != nullptr && b.batch_id == cur->batch_id;
  if (aCur != bCur) return aCur;

  // Within Suspended: by suspend_time desc.
  if (a.state == domain::BatchState::Suspended)
    return a.suspend_time_utc > b.suspend_time_utc;

  // Within Settled: by period_end desc.
  if (a.state == domain::BatchState::Settled)
    return a.period_end_utc > b.period_end_utc;

  // Within Active (non-current): by period_start desc.
  return a.period_start_utc > b.period_start_utc;
}

void BillingBatchModel::rebuildOrdering()
{
  orderedBatchIds_.clear();
  const auto& batches = service_->batches();
  orderedBatchIds_.reserve(batches.size());
  for (const auto& b : batches) orderedBatchIds_.push_back(b.batch_id);
  // We need to sort the batch ids based on their batch entities.  Pull
  // them out into a temp vector for sorting.
  std::vector<const domain::BillingBatch*> ptrs;
  ptrs.reserve(batches.size());
  for (const auto& b : batches) ptrs.push_back(&b);
  std::sort(ptrs.begin(), ptrs.end(),
            [this](const domain::BillingBatch* a, const domain::BillingBatch* b) {
              return orderLess(*a, *b);
            });
  orderedBatchIds_.clear();
  for (auto* p : ptrs) orderedBatchIds_.push_back(p->batch_id);
}

void BillingBatchModel::refreshAll()
{
  beginResetModel();
  rebuildOrdering();
  endResetModel();
}

void BillingBatchModel::onBatchesChanged()
{
  refreshAll();
}

void BillingBatchModel::onBatchUpdated(const QString& batchId)
{
  // Find the batch row and emit dataChanged for it + its children.
  for (int i = 0; i < static_cast<int>(orderedBatchIds_.size()); ++i)
  {
    if (orderedBatchIds_[static_cast<std::size_t>(i)] == batchId.toStdString())
    {
      QModelIndex batchIdx = index(i, 0);
      emit dataChanged(batchIdx, index(i, ColumnCount - 1));
      const auto* batch = service_->batchById(batchId.toStdString());
      if (batch != nullptr)
      {
        int n = static_cast<int>(batch->job_ids.size());
        if (n > 0)
          emit dataChanged(this->index(0, 0, batchIdx),
                           this->index(n - 1, ColumnCount - 1, batchIdx));
      }
      return;
    }
  }
}

QModelIndex BillingBatchModel::index(int row, int column,
                                      const QModelIndex& parent) const
{
  if (!hasIndex(row, column, parent)) return {};

  if (!parent.isValid())
  {
    if (row >= 0 && row < static_cast<int>(orderedBatchIds_.size()))
    {
      // Use the row index as the internal pointer (we re-resolve via
      // orderedBatchIds_ on demand).
      return createIndex(row, column, nullptr);
    }
  }
  else
  {
    // Parent is a batch row; row is the job index within the batch.
    int batchRow = parent.row();
    if (batchRow >= 0 && batchRow < static_cast<int>(orderedBatchIds_.size()))
    {
      // Encode batchRow in the internal pointer so parent() can recover it.
      return createIndex(row, column, static_cast<quintptr>(batchRow + 1));
    }
  }
  return {};
}

QModelIndex BillingBatchModel::parent(const QModelIndex& index) const
{
  if (!index.isValid()) return {};
  quintptr payload = reinterpret_cast<quintptr>(index.internalPointer());
  if (payload == 0) return {};  // batch row → no parent
  int batchRow = static_cast<int>(payload - 1);
  return createIndex(batchRow, 0, nullptr);
}

int BillingBatchModel::rowCount(const QModelIndex& parent) const
{
  if (!parent.isValid())
    return static_cast<int>(orderedBatchIds_.size());

  quintptr payload = reinterpret_cast<quintptr>(parent.internalPointer());
  if (payload != 0) return 0;  // job rows have no children

  int batchRow = parent.row();
  if (batchRow < 0 || batchRow >= static_cast<int>(orderedBatchIds_.size())) return 0;
  const auto* batch = service_->batchById(orderedBatchIds_[static_cast<std::size_t>(batchRow)]);
  if (batch == nullptr) return 0;
  return static_cast<int>(batch->job_ids.size());
}

int BillingBatchModel::columnCount(const QModelIndex&) const
{
  return ColumnCount;
}

Qt::ItemFlags BillingBatchModel::flags(const QModelIndex& index) const
{
  if (!index.isValid()) return Qt::NoItemFlags;
  return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

QVariant BillingBatchModel::data(const QModelIndex& index, int role) const
{
  if (!index.isValid()) return {};
  quintptr payload = reinterpret_cast<quintptr>(index.internalPointer());
  int col = index.column();

  if (payload == 0)
  {
    // Batch row
    int batchRow = index.row();
    if (batchRow < 0 || batchRow >= static_cast<int>(orderedBatchIds_.size())) return {};
    const auto* batch = service_->batchById(orderedBatchIds_[static_cast<std::size_t>(batchRow)]);
    if (batch == nullptr) return {};

    switch (role)
    {
      case Qt::DisplayRole:
      {
        switch (col)
        {
          case ColName:
            return QString::fromStdString(batch->display_name);
          case ColState:
            return stateBadge(batch->state);
          case ColProgress:
          {
            QString txt = QStringLiteral("%1 文档 · %2 作业")
              .arg(batch->document_count)
              .arg(static_cast<int>(batch->job_ids.size()));
            if (batch->total_price > 0.0)
              txt += QStringLiteral(" · ¥%1").arg(QString::number(batch->total_price, 'f', 2));
            if (batch->pending_count > 0)
              txt += QStringLiteral(" · %1项待定价").arg(batch->pending_count);
            return txt;
          }
          case ColPages:
            return batch->total_pages;
          case ColDuplex:
            return QStringLiteral("单%1 双%2")
              .arg(batch->simplex_pages).arg(batch->duplex_pages);
          case ColPaper:
            return QStringLiteral("文档 %1").arg(batch->document_count);
          case ColSubmitTime:
            return formatDisplayTime(QString::fromStdString(batch->period_start_utc));
          case ColPrice:
            return QString::number(batch->total_price, 'f', 2);
          default: return {};
        }
      }
      case NodeTypeRole:      return QStringLiteral("batch");
      case BatchIdRole:       return QString::fromStdString(batch->batch_id);
      case StateRole:         return static_cast<int>(batch->state);
      case DisplayNameRole:   return QString::fromStdString(batch->display_name);
      case AnchorFirstRole:   return QString::fromStdString(batch->first_doc_anchor.document_name);
      case AnchorLastRole:    return QString::fromStdString(batch->last_doc_anchor.document_name);
      case DocumentCountRole: return batch->document_count;
      case SimplexPagesRole:  return batch->simplex_pages;
      case DuplexPagesRole:   return batch->duplex_pages;
      case PricePendingRole:  return batch->pending_count > 0;
      case PendingReasonRole: return batch->pending_count > 0 ? QStringLiteral("pending") : QString();
      case RowColorRole:      return userBackgroundColor(QString::fromStdString(batch->batch_id));
      case RowTextColorRole:  return userTextColor(QString::fromStdString(batch->batch_id));
      case SortRole:
      {
        if (col == ColName) return QString::fromStdString(batch->display_name).toCaseFolded();
        if (col == ColPages) return batch->total_pages;
        if (col == ColPrice) return batch->total_price;
        if (col == ColSubmitTime) return QString::fromStdString(batch->period_start_utc);
        return {};
      }
      case Qt::TextAlignmentRole:
        return (col == ColName || col == ColSubmitTime) ? Qt::AlignVCenter : Qt::AlignCenter;
      case Qt::ForegroundRole:
        return (col == ColState) ? QBrush(stateColor(batch->state)) : QVariant{};
      case Qt::BackgroundRole:
        return QBrush(userBackgroundColor(QString::fromStdString(batch->batch_id)).lighter(180));
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
      default: return {};
    }
  }
  else
  {
    // Job row
    int batchRow = static_cast<int>(payload - 1);
    int jobRow = index.row();
    if (batchRow < 0 || batchRow >= static_cast<int>(orderedBatchIds_.size())) return {};
    const auto* batch = service_->batchById(orderedBatchIds_[static_cast<std::size_t>(batchRow)]);
    if (batch == nullptr || jobRow < 0 || jobRow >= static_cast<int>(batch->job_ids.size())) return {};
    const auto* job = service_->jobRecord(batch->job_ids[static_cast<std::size_t>(jobRow)]);
    if (job == nullptr) return {};

    switch (role)
    {
      case Qt::DisplayRole:
      {
        switch (col)
        {
          case ColName:       return QString::fromStdString(job->snapshot.document_name);
          case ColState:      return phaseDisplayName(job->phase);
          case ColProgress:
          {
            int pct = progressPercent(*job);
            if (pct >= 0) return QStringLiteral("%1%").arg(pct);
            return QStringLiteral("-");
          }
          case ColPages:      return job->pricing.total_pages;
          case ColDuplex:     return duplexLabel(job->metadata);
          case ColPaper:
          {
            if (job->metadata.paper_size.state == core::FieldState::Reported)
              return QString::fromStdString(job->metadata.paper_size.value);
            return QStringLiteral("-");
          }
          case ColSubmitTime: return formatDisplayTime(QString::fromStdString(job->snapshot.submit_time_utc));
          case ColPrice:
            return QString::number(job->pricing.total_price, 'f', 2);
          default: return {};
        }
      }
      case NodeTypeRole:       return QStringLiteral("job");
      case JobIdRole:          return static_cast<qulonglong>(job->job_id);
      case BatchIdRole:        return QString::fromStdString(batch->batch_id);
      case PhaseKeyRole:       return QString::fromStdString(std::string(core::to_string(job->phase)));
      case ProgressPercentRole: return progressPercent(*job);
      case ColorDowngradedRole: return job->pricing.color_downgraded;
      case PricePendingRole:   return job->pricing.price_pending;
      case PendingReasonRole:  return QString::fromStdString(job->pricing.pending_reason);
      case RowColorRole:       return userBackgroundColor(QString::fromStdString(batch->batch_id));
      case RowTextColorRole:   return userTextColor(QString::fromStdString(batch->batch_id));
      case SortRole:
      {
        if (col == ColName) return QString::fromStdString(job->snapshot.document_name).toCaseFolded();
        if (col == ColPages) return job->pricing.total_pages;
        if (col == ColPrice) return job->pricing.total_price;
        if (col == ColSubmitTime) return QString::fromStdString(job->snapshot.submit_time_utc);
        return {};
      }
      case Qt::TextAlignmentRole:
        return (col == ColName || col == ColSubmitTime) ? Qt::AlignVCenter : Qt::AlignCenter;
      case Qt::ForegroundRole:
        return (col == ColState) ? QBrush(phaseColor(job->phase)) : QVariant{};
      case Qt::BackgroundRole:
        return QBrush(userBackgroundColor(QString::fromStdString(batch->batch_id)).lighter(195));
      case Qt::FontRole:
      {
        if (col == ColName)
        {
          QFont f;
          f.setWeight(QFont::DemiBold);
          return f;
        }
        return {};
      }
      default: return {};
    }
  }
}

QVariant BillingBatchModel::headerData(int section, Qt::Orientation orientation,
                                        int role) const
{
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
  switch (section)
  {
    case ColName:        return QStringLiteral("用户 / 文档");
    case ColState:       return QStringLiteral("状态");
    case ColProgress:    return QStringLiteral("进度");
    case ColPages:       return QStringLiteral("页数");
    case ColDuplex:      return QStringLiteral("单|双面");
    case ColPaper:       return QStringLiteral("纸张");
    case ColSubmitTime:  return QStringLiteral("时间");
    case ColPrice:       return QStringLiteral("金额");
    default:             return {};
  }
}

int BillingBatchModel::batchCount() const
{
  return static_cast<int>(orderedBatchIds_.size());
}

QModelIndex BillingBatchModel::batchIndex(const QString& batchId) const
{
  for (int i = 0; i < static_cast<int>(orderedBatchIds_.size()); ++i)
  {
    if (QString::fromStdString(orderedBatchIds_[static_cast<std::size_t>(i)]) == batchId)
      return createIndex(i, 0, nullptr);
  }
  return {};
}

QString BillingBatchModel::batchIdAt(const QModelIndex& index) const
{
  if (!index.isValid()) return {};
  quintptr payload = reinterpret_cast<quintptr>(index.internalPointer());
  if (payload != 0)
  {
    // Job row → resolve parent batch.
    int batchRow = static_cast<int>(payload - 1);
    if (batchRow >= 0 && batchRow < static_cast<int>(orderedBatchIds_.size()))
      return QString::fromStdString(orderedBatchIds_[static_cast<std::size_t>(batchRow)]);
    return {};
  }
  int row = index.row();
  if (row < 0 || row >= static_cast<int>(orderedBatchIds_.size())) return {};
  return QString::fromStdString(orderedBatchIds_[static_cast<std::size_t>(row)]);
}

std::uint32_t BillingBatchModel::jobIdAt(const QModelIndex& index) const
{
  if (!index.isValid()) return 0;
  quintptr payload = reinterpret_cast<quintptr>(index.internalPointer());
  if (payload == 0) return 0;
  int batchRow = static_cast<int>(payload - 1);
  int jobRow = index.row();
  if (batchRow < 0 || batchRow >= static_cast<int>(orderedBatchIds_.size())) return 0;
  const auto* batch = service_->batchById(orderedBatchIds_[static_cast<std::size_t>(batchRow)]);
  if (batch == nullptr || jobRow < 0 || jobRow >= static_cast<int>(batch->job_ids.size())) return 0;
  return batch->job_ids[static_cast<std::size_t>(jobRow)];
}

bool BillingBatchModel::isBatchRow(const QModelIndex& index) const
{
  if (!index.isValid()) return false;
  return reinterpret_cast<quintptr>(index.internalPointer()) == 0;
}

bool BillingBatchModel::isJobRow(const QModelIndex& index) const
{
  if (!index.isValid()) return false;
  return reinterpret_cast<quintptr>(index.internalPointer()) != 0;
}

domain::BatchState BillingBatchModel::batchStateAt(const QModelIndex& index) const
{
  const auto* batch = batchAt(index);
  return batch != nullptr ? batch->state : domain::BatchState::Voided;
}

const domain::BillingBatch* BillingBatchModel::batchAt(const QModelIndex& index) const
{
  QString id = batchIdAt(index);
  if (id.isEmpty()) return nullptr;
  return service_->batchById(id.toStdString());
}

const domain::PrintJobRecord* BillingBatchModel::jobAt(const QModelIndex& index) const
{
  std::uint32_t jid = jobIdAt(index);
  if (jid == 0) return nullptr;
  return service_->jobRecord(jid);
}

int BillingBatchModel::currentActiveRow() const
{
  const auto* cur = service_->currentActiveBatch();
  if (cur == nullptr) return -1;
  for (int i = 0; i < static_cast<int>(orderedBatchIds_.size()); ++i)
  {
    if (orderedBatchIds_[static_cast<std::size_t>(i)] == cur->batch_id)
      return i;
  }
  return -1;
}

int BillingBatchModel::nextRelevantRow(int fromRow) const
{
  int n = static_cast<int>(orderedBatchIds_.size());
  if (n == 0) return -1;

  // Sequential forward cycle through Active + Suspended batches.
  for (int offset = 1; offset <= n; ++offset)
  {
    int i = (fromRow + offset) % n;
    const auto* b = service_->batchById(orderedBatchIds_[static_cast<std::size_t>(i)]);
    if (b != nullptr && !b->is_settled())
      return i;
  }
  return -1;
}

int BillingBatchModel::prevRelevantRow(int fromRow) const
{
  int n = static_cast<int>(orderedBatchIds_.size());
  if (n == 0) return -1;

  // Sequential backward cycle through Active + Suspended batches.
  for (int offset = 1; offset <= n; ++offset)
  {
    int i = ((fromRow - offset) % n + n) % n;
    const auto* b = service_->batchById(orderedBatchIds_[static_cast<std::size_t>(i)]);
    if (b != nullptr && !b->is_settled())
      return i;
  }
  return -1;
}

}  // namespace wpce::ui
