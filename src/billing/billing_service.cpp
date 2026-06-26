/// @file billing_service.cpp
/// BillingService implementation.

#include "billing/billing_service.h"

#include "bridge/settlement_bridge.h"
#include "common/app_config.h"
#include "common/pricing_engine.h"

#include <QDateTime>
#include <QDir>
#include <QFile>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <sstream>

namespace wpce::ui
{

namespace
{

std::string utcNowIso8601()
{
  const auto now = std::chrono::system_clock::now();
  const auto t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec);
  return buf;
}

}  // namespace

BillingService::BillingService(QObject* parent)
  : QObject(parent)
{
  defaultPricing_ = wpce::common::AppConfig::defaults().pricing;
  pricingConfig_ = &defaultPricing_;

  saveTimer_.setSingleShot(true);
  saveTimer_.setInterval(1500);
  connect(&saveTimer_, &QTimer::timeout, this, &BillingService::flushSave);

  // Periodic safety flush every 30s to guard against crash data loss.
  periodicSaveTimer_.setInterval(30000);
  periodicSaveTimer_.setSingleShot(false);
  connect(&periodicSaveTimer_, &QTimer::timeout, this, &BillingService::flushSave);
  periodicSaveTimer_.start();
}

BillingService::~BillingService()
{
  flushSave();
}

void BillingService::setStateDirectory(const QString& stateDir)
{
  stateDir_ = stateDir;
  QDir().mkpath(stateDir_);

  const QString counterPath = stateDir_ + QDir::separator() + QStringLiteral("counters.json");
  counterStore_.setStatePath(counterPath.toStdWString());

  const QString batchesPath = stateDir_ + QDir::separator() + QStringLiteral("today_active.json");
  repository_.setFilePath(batchesPath.toStdWString());
}

void BillingService::setHistoryRoot(const QString& historyRoot)
{
  historyRoot_ = historyRoot;
  QDir().mkpath(historyRoot_);
}

void BillingService::setPricingConfig(const wpce::common::PricingConfig* pricing)
{
  pricingConfig_ = (pricing != nullptr) ? pricing : &defaultPricing_;
}

void BillingService::setPrinterProfiles(const wpce::common::PrinterProfileConfig* printers)
{
  printerProfiles_ = (printers != nullptr) ? printers : &defaultPrinters_;
}

void BillingService::load()
{
  std::vector<domain::BillingBatch> loaded;
  std::unordered_map<std::uint32_t, domain::PrintJobRecord> loadedJobs;
  std::string savedDate;
  if (repository_.load(loaded, loadedJobs, savedDate))
  {
    const std::string today = todayDateString();

    // Cross-day detection: if the saved date is from a different day,
    // archive settled batches and discard them from active state.
    if (!savedDate.empty() && savedDate != today && !loaded.empty())
    {
      // Remove already-settled batches from the loaded set (they belong
      // to yesterday's history and should already be persisted there).
      // Also remove those batches' jobs from the registry.
      loaded.erase(
        std::remove_if(loaded.begin(), loaded.end(),
                       [&loadedJobs](const domain::BillingBatch& b) {
                         if (b.is_settled())
                         {
                           for (auto jid : b.job_ids) loadedJobs.erase(jid);
                           return true;
                         }
                         return false;
                       }),
        loaded.end());
    }

    batches_ = std::move(loaded);
    jobRegistry_ = std::move(loadedJobs);
    batchIndex_.clear();
    for (std::size_t i = 0; i < batches_.size(); ++i)
      batchIndex_[batches_[i].batch_id] = i;

    currentActiveBatchId_.clear();
    std::string mostRecentActive;
    std::string mostRecentActiveTime;
    for (const auto& b : batches_)
    {
      if (b.is_active() && !b.period_start_utc.empty() &&
          (mostRecentActiveTime.empty() || b.period_start_utc > mostRecentActiveTime))
      {
        mostRecentActive = b.batch_id;
        mostRecentActiveTime = b.period_start_utc;
      }
    }
    currentActiveBatchId_ = mostRecentActive;

    // If we loaded legacy-format data (batches exist but job records don't),
    // mark dirty so flushSave() upgrades the file to schema_v3 + jobs array.
    if (!batches_.empty() && jobRegistry_.empty())
    {
      dirty_ = true;
      scheduleSave();
    }

    emit batchesChanged();
  }
}

void BillingService::flushSave()
{
  if (!dirty_) return;
  dirty_ = false;

  // Only persist jobs that belong to a known batch (excludes unassigned jobs).
  std::unordered_map<std::uint32_t, domain::PrintJobRecord> batchJobs;
  for (const auto& batch : batches_)
  {
    for (auto jid : batch.job_ids)
    {
      auto it = jobRegistry_.find(jid);
      if (it != jobRegistry_.end())
        batchJobs[jid] = it->second;
    }
  }

  (void)repository_.save(batches_, batchJobs, todayDateString());
}

void BillingService::clear()
{
  saveTimer_.stop();
  batches_.clear();
  jobRegistry_.clear();
  batchIndex_.clear();
  currentActiveBatchId_.clear();
  dirty_ = false;
  // Note: we do NOT clear the CounterStore file — counters must remain
  // monotonic across sessions.  The repository file is overwritten on
  // the next save.
  emit batchesChanged();
}

void BillingService::scheduleSave()
{
  dirty_ = true;
  if (!stateDir_.isEmpty()) saveTimer_.start();
}

const domain::BillingBatch* BillingService::batchById(const std::string& batchId) const
{
  auto it = batchIndex_.find(batchId);
  if (it == batchIndex_.end()) return nullptr;
  return &batches_[it->second];
}

domain::BillingBatch* BillingService::batchById(const std::string& batchId)
{
  auto it = batchIndex_.find(batchId);
  if (it == batchIndex_.end()) return nullptr;
  return &batches_[it->second];
}

const domain::PrintJobRecord* BillingService::jobRecord(std::uint32_t jobId) const
{
  auto it = jobRegistry_.find(jobId);
  if (it == jobRegistry_.end()) return nullptr;
  return &it->second;
}

int BillingService::activeBatchCount() const
{
  int n = 0;
  for (const auto& b : batches_) if (b.is_active()) ++n;
  return n;
}

int BillingService::suspendedBatchCount() const
{
  int n = 0;
  for (const auto& b : batches_) if (b.is_suspended()) ++n;
  return n;
}

int BillingService::settledBatchCount() const
{
  int n = 0;
  for (const auto& b : batches_) if (b.is_settled()) ++n;
  return n;
}

int BillingService::todayJobCount() const
{
  return static_cast<int>(jobRegistry_.size());
}

double BillingService::todayRevenue() const
{
  double total = 0.0;
  for (const auto& b : batches_)
    if (!b.is_settled()) total += b.total_price;
  return total;
}

const domain::BillingBatch* BillingService::currentActiveBatch() const
{
  if (currentActiveBatchId_.empty()) return nullptr;
  return batchById(currentActiveBatchId_);
}

domain::BillingBatch& BillingService::createNewActiveBatch(const std::string& sourceUser)
{
  const std::string batchId = counterStore_.nextBatchId();

  // Generate a display name like "用户#NN" using the batch counter.
  // Extract the numeric tail of the batch id for the display number.
  std::string displayName = QStringLiteral("用户#%1")
    .arg(QString::fromStdString(batchId).mid(batchId.size() - 6).toInt())
    .toStdString();

  domain::BillingBatch batch;
  batch.batch_id = batchId;
  batch.display_name = displayName;
  batch.source_user = sourceUser;
  batch.state = domain::BatchState::Active;

  batches_.push_back(std::move(batch));
  std::size_t idx = batches_.size() - 1;
  batchIndex_[batches_[idx].batch_id] = idx;
  currentActiveBatchId_ = batches_[idx].batch_id;

  emit batchCreated(QString::fromStdString(batches_[idx].batch_id));
  emit batchesChanged();
  return batches_[idx];
}

domain::BillingBatch* BillingService::ensureActiveBatch(const core::JobSnapshot& snapshot)
{
  // If we have a current active batch, return it.
  if (!currentActiveBatchId_.empty())
  {
    auto it = batchIndex_.find(currentActiveBatchId_);
    if (it != batchIndex_.end() && batches_[it->second].is_active())
      return &batches_[it->second];
  }
  // Otherwise create a new one.
  return &createNewActiveBatch(snapshot.user);
}

void BillingService::updateAnchors(domain::BillingBatch& batch)
{
  // Anchors are based on the first and last jobs (by submit time).
  if (batch.job_ids.empty())
  {
    batch.first_doc_anchor = {};
    batch.last_doc_anchor = {};
    return;
  }

  std::uint32_t firstId = 0, lastId = 0;
  std::string firstTime, lastTime;
  for (auto jid : batch.job_ids)
  {
    auto it = jobRegistry_.find(jid);
    if (it == jobRegistry_.end()) continue;
    const auto& snap = it->second.snapshot;
    if (firstTime.empty() || snap.submit_time_utc < firstTime)
    {
      firstTime = snap.submit_time_utc;
      firstId = jid;
    }
    if (lastTime.empty() || snap.submit_time_utc > lastTime)
    {
      lastTime = snap.submit_time_utc;
      lastId = jid;
    }
  }

  if (firstId != 0)
  {
    const auto& rec = jobRegistry_[firstId];
    batch.first_doc_anchor.job_id = firstId;
    batch.first_doc_anchor.document_name = rec.snapshot.document_name;
    batch.first_doc_anchor.submit_time_utc = rec.snapshot.submit_time_utc;
    batch.first_doc_anchor.page_count = rec.pricing.total_pages;
  }
  if (lastId != 0)
  {
    const auto& rec = jobRegistry_[lastId];
    batch.last_doc_anchor.job_id = lastId;
    batch.last_doc_anchor.document_name = rec.snapshot.document_name;
    batch.last_doc_anchor.submit_time_utc = rec.snapshot.submit_time_utc;
    batch.last_doc_anchor.page_count = rec.pricing.total_pages;
  }
}

void BillingService::recomputeBatchTotals(domain::BillingBatch& batch)
{
  batch.total_pages = 0;
  batch.simplex_pages = 0;
  batch.duplex_pages = 0;
  batch.total_price = 0.0;
  batch.pending_count = 0;
  batch.document_count = 0;

  // Track distinct document_name + file_extension for document_count.
  std::vector<std::string> distinctDocs;
  distinctDocs.reserve(batch.job_ids.size());

  for (auto jid : batch.job_ids)
  {
    auto it = jobRegistry_.find(jid);
    if (it == jobRegistry_.end()) continue;
    const auto& rec = it->second;
    const auto& meta = rec.metadata;
    const auto& pricing = rec.pricing;

    const int pages = wpce::common::parse_total_pages(meta);
    const int copies = wpce::common::parse_copies(meta);
    const int logicalPages = pages * copies;
    batch.total_pages += logicalPages;

    if (wpce::common::is_duplex_job(meta))
      batch.duplex_pages += logicalPages;
    else
      batch.simplex_pages += logicalPages;

    batch.total_price += pricing.total_price;
    if (pricing.price_pending) ++batch.pending_count;

    std::string docKey = rec.snapshot.document_name;
    if (meta.file_extension.state == core::FieldState::Reported && !meta.file_extension.value.empty())
      docKey += "|" + meta.file_extension.value;
    if (std::find(distinctDocs.begin(), distinctDocs.end(), docKey) == distinctDocs.end())
    {
      distinctDocs.push_back(docKey);
      ++batch.document_count;
    }
  }

  updateAnchors(batch);
}

// ── WatcherBridge slots ───────────────────────────────────────────────

void BillingService::onJobSnapshot(core::JobSnapshot snapshot)
{
  if (snapshot.job_id == 0) return;

  const std::uint32_t jobId = snapshot.job_id;

  auto it = jobRegistry_.find(jobId);
  if (it != jobRegistry_.end())
  {
    // Existing job — update snapshot.
    it->second.snapshot = std::move(snapshot);
    it->second.last_updated_utc = utcNowIso8601();
    // Possibly move to a different batch if the user changed? No —
    // batch binding is sticky once established.
    // Recompute the batch totals if the document name changed.
    auto* batch = batchById(currentActiveBatchId_);
    if (batch != nullptr && batch->is_active())
    {
      // Check if this job is in the current active batch.
      bool inCurrent = false;
      for (auto jid : batch->job_ids) if (jid == jobId) { inCurrent = true; break; }
      if (inCurrent)
      {
        recomputeBatchTotals(*batch);
        emit batchUpdated(QString::fromStdString(batch->batch_id));
      }
    }
    return;
  }

  // New job — bind to currentActiveBatch (creating one if needed).
  domain::BillingBatch* batch = ensureActiveBatch(snapshot);
  if (batch == nullptr) return;

  domain::PrintJobRecord rec;
  rec.job_id = jobId;
  rec.batch_job_seq = static_cast<std::uint32_t>(batch->job_ids.size() + 1);
  rec.snapshot = std::move(snapshot);
  rec.last_updated_utc = utcNowIso8601();
  jobRegistry_[jobId] = std::move(rec);

  batch->job_ids.push_back(jobId);
  if (batch->period_start_utc.empty())
    batch->period_start_utc = jobRegistry_[jobId].snapshot.submit_time_utc;

  recomputeBatchTotals(*batch);
  scheduleSave();
  emit batchUpdated(QString::fromStdString(batch->batch_id));
}

void BillingService::onMetadata(core::MetadataSnapshot metadata)
{
  if (metadata.job_id == 0) return;

  auto it = jobRegistry_.find(metadata.job_id);
  if (it == jobRegistry_.end())
  {
    // Metadata arrived before snapshot — create a stub record so we
    // don't lose it.  The snapshot will fill in the rest.
    domain::PrintJobRecord rec;
    rec.job_id = metadata.job_id;
    rec.last_updated_utc = utcNowIso8601();
    it = jobRegistry_.emplace(metadata.job_id, std::move(rec)).first;
  }

  it->second.metadata = std::move(metadata);
  it->second.last_updated_utc = utcNowIso8601();

  // Recompute pricing — preserve operator-set color_pages.
  const int existing_color_pages = it->second.pricing.color_pages;
  const wpce::common::PricingConfig& pc = pricingConfig_ ? *pricingConfig_ : defaultPricing_;
  const wpce::common::PrinterProfileConfig& pp = printerProfiles_ ? *printerProfiles_ : defaultPrinters_;
  it->second.pricing = wpce::common::compute_job_pricing(it->second.metadata, pc, pp, existing_color_pages);

  // Recompute the owning batch's totals.
  for (auto& batch : batches_)
  {
    auto jidIt = std::find(batch.job_ids.begin(), batch.job_ids.end(), metadata.job_id);
    if (jidIt != batch.job_ids.end())
    {
      recomputeBatchTotals(batch);
      emit batchUpdated(QString::fromStdString(batch.batch_id));
      scheduleSave();
      return;
    }
  }
}

void BillingService::onLifecycleEvent(core::JobLifecycleEvent event)
{
  if (event.job_id == 0) return;

  auto it = jobRegistry_.find(event.job_id);
  if (it == jobRegistry_.end()) return;

  it->second.phase = event.phase;
  it->second.last_updated_utc = utcNowIso8601();
  if (event.total_pages > 0)
  {
    it->second.metadata.total_pages =
      core::FieldValue{std::to_string(event.total_pages), core::FieldState::Reported};
  }
  it->second.metadata.pages_printed =
    core::FieldValue{std::to_string(event.pages_printed), core::FieldState::Reported};

  // Recompute pricing in case page count changed — preserve operator-set color_pages.
  const int existing_color_pages = it->second.pricing.color_pages;
  const wpce::common::PricingConfig& pc = pricingConfig_ ? *pricingConfig_ : defaultPricing_;
  const wpce::common::PrinterProfileConfig& pp = printerProfiles_ ? *printerProfiles_ : defaultPrinters_;
  it->second.pricing = wpce::common::compute_job_pricing(it->second.metadata, pc, pp, existing_color_pages);

  for (auto& batch : batches_)
  {
    auto jidIt = std::find(batch.job_ids.begin(), batch.job_ids.end(), event.job_id);
    if (jidIt != batch.job_ids.end())
    {
      recomputeBatchTotals(batch);
      emit batchUpdated(QString::fromStdString(batch.batch_id));
      scheduleSave();
      return;
    }
  }
}

// ── Operator actions ──────────────────────────────────────────────────

QString BillingService::settle(const QString& batchId)
{
  const std::string id = batchId.toStdString();
  auto it = batchIndex_.find(id);
  if (it == batchIndex_.end()) return {};
  auto& batch = batches_[it->second];
  if (!batch.is_active()) return {};

  const std::string orderId = counterStore_.nextOrderId();
  batch.period_end_utc = utcNowIso8601();
  batch.settle_order_id = settleBatchInternal(batch, orderId);

  // Recompute totals (final pricing may have been adjusted by dialog).
  recomputeBatchTotals(batch);

  batch.state = domain::BatchState::Settled;

  // Clear currentActive if it was this batch.
  if (currentActiveBatchId_ == id) currentActiveBatchId_.clear();

  scheduleSave();
  emit batchUpdated(batchId);
  emit batchesChanged();
  emit orderSettled(QString::fromStdString(batch.settle_order_id), batchId);
  return QString::fromStdString(batch.settle_order_id);
}

std::string BillingService::settleBatchInternal(domain::BillingBatch& batch,
                                                 const std::string& orderId)
{
  // Collect job ids and pricing entries.
  std::vector<std::uint32_t> jobIds;
  std::vector<core::JobPricingEntry> pricingEntries;
  for (auto jid : batch.job_ids)
  {
    auto it = jobRegistry_.find(jid);
    if (it == jobRegistry_.end()) continue;
    jobIds.push_back(jid);
    pricingEntries.push_back(it->second.pricing);
  }

  const wpce::common::PricingConfig& pc = pricingConfig_ ? *pricingConfig_ : defaultPricing_;

  // Resolve period start.
  std::string periodStart = batch.period_start_utc;
  if (periodStart.empty() && !batch.job_ids.empty())
  {
    auto it = jobRegistry_.find(batch.job_ids.front());
    if (it != jobRegistry_.end()) periodStart = it->second.snapshot.submit_time_utc;
  }

  core::SettlementOrder order = bridge::SettlementBridge::createSettlementOrder(
    orderId,
    batch.batch_id,
    batch.display_name,
    jobIds,
    pricingEntries,
    pc,
    periodStart,
    utcNowIso8601(),
    "operator");

  order.batch_id = batch.batch_id;

  // Persist to legacy _state/settlement_orders/ directory.
  if (!stateDir_.isEmpty())
  {
    std::filesystem::path stateDir(stateDir_.toStdWString());
    bridge::SettlementBridge::saveToFile(order, stateDir);
  }

  // Persist to date-based history directory.
  if (!historyRoot_.isEmpty())
  {
    std::filesystem::path historyRoot(historyRoot_.toStdWString());
    bridge::SettlementBridge::saveToDateDir(
      order, historyRoot, todayDateString(),
      batch.first_doc_anchor.document_name,
      batch.last_doc_anchor.document_name);
  }

  return order.order_id;
}

bool BillingService::suspend(const QString& batchId)
{
  const std::string id = batchId.toStdString();
  auto it = batchIndex_.find(id);
  if (it == batchIndex_.end()) return false;
  auto& batch = batches_[it->second];
  if (!batch.is_active()) return false;

  batch.state = domain::BatchState::Suspended;
  batch.suspend_time_utc = utcNowIso8601();
  if (currentActiveBatchId_ == id) currentActiveBatchId_.clear();

  scheduleSave();
  emit batchUpdated(batchId);
  emit batchesChanged();
  return true;
}

bool BillingService::resume(const QString& batchId)
{
  const std::string id = batchId.toStdString();
  auto it = batchIndex_.find(id);
  if (it == batchIndex_.end()) return false;
  auto& batch = batches_[it->second];
  if (!batch.is_suspended()) return false;

  // If another batch is currently active, suspend it first.
  if (!currentActiveBatchId_.empty() && currentActiveBatchId_ != id)
  {
    auto curIt = batchIndex_.find(currentActiveBatchId_);
    if (curIt != batchIndex_.end() && batches_[curIt->second].is_active())
    {
      batches_[curIt->second].state = domain::BatchState::Suspended;
      batches_[curIt->second].suspend_time_utc = utcNowIso8601();
      emit batchUpdated(QString::fromStdString(batches_[curIt->second].batch_id));
    }
  }

  batch.state = domain::BatchState::Active;
  batch.suspend_time_utc.clear();
  currentActiveBatchId_ = id;

  scheduleSave();
  emit batchUpdated(batchId);
  emit batchesChanged();
  return true;
}

QString BillingService::mergeSettle(const QStringList& batchIds)
{
  if (batchIds.isEmpty()) return {};

  const std::string orderId = counterStore_.nextOrderId();
  const std::string now = utcNowIso8601();

  for (const auto& qbid : batchIds)
  {
    const std::string id = qbid.toStdString();
    auto it = batchIndex_.find(id);
    if (it == batchIndex_.end()) continue;
    auto& batch = batches_[it->second];
    if (!batch.is_suspended()) continue;

    batch.period_end_utc = now;
    batch.settle_order_id = orderId;
    batch.state = domain::BatchState::Settled;
    recomputeBatchTotals(batch);
    emit batchUpdated(qbid);
  }

  if (!currentActiveBatchId_.empty() &&
      batchIds.contains(QString::fromStdString(currentActiveBatchId_)))
  {
    currentActiveBatchId_.clear();
  }

  // Persist a single settlement order with all jobs from all batches.
  std::vector<std::uint32_t> allJobIds;
  std::vector<core::JobPricingEntry> allPricing;
  for (const auto& qbid : batchIds)
  {
    auto it = batchIndex_.find(qbid.toStdString());
    if (it == batchIndex_.end()) continue;
    for (auto jid : batches_[it->second].job_ids)
    {
      auto regIt = jobRegistry_.find(jid);
      if (regIt == jobRegistry_.end()) continue;
      allJobIds.push_back(jid);
      allPricing.push_back(regIt->second.pricing);
    }
  }

  const wpce::common::PricingConfig& pc = pricingConfig_ ? *pricingConfig_ : defaultPricing_;
  core::SettlementOrder order = bridge::SettlementBridge::createSettlementOrder(
    orderId, batchIds.first().toStdString(), batchIds.first().toStdString(),
    allJobIds, allPricing, pc, std::string{}, now, "operator");
  order.batch_id = batchIds.first().toStdString();
  if (!stateDir_.isEmpty())
  {
    std::filesystem::path stateDir(stateDir_.toStdWString());
    bridge::SettlementBridge::saveToFile(order, stateDir);
  }

  scheduleSave();
  emit batchesChanged();
  emit orderSettled(QString::fromStdString(orderId), batchIds.first());
  return QString::fromStdString(orderId);
}

bool BillingService::renameBatch(const QString& batchId, const QString& newName)
{
  const std::string id = batchId.toStdString();
  auto it = batchIndex_.find(id);
  if (it == batchIndex_.end()) return false;
  auto& batch = batches_[it->second];
  if (batch.is_settled()) return false;
  batch.display_name = newName.toStdString();
  scheduleSave();
  emit batchUpdated(batchId);
  return true;
}

bool BillingService::removeJobFromBatch(const QString& batchId, std::uint32_t jobId)
{
  const std::string id = batchId.toStdString();
  auto it = batchIndex_.find(id);
  if (it == batchIndex_.end()) return false;
  auto& batch = batches_[it->second];
  if (batch.is_settled()) return false;

  auto jidIt = std::find(batch.job_ids.begin(), batch.job_ids.end(), jobId);
  if (jidIt == batch.job_ids.end()) return false;
  batch.job_ids.erase(jidIt);

  // Remove from registry — the job is now unassigned.
  jobRegistry_.erase(jobId);

  recomputeBatchTotals(batch);
  scheduleSave();
  emit batchUpdated(batchId);
  return true;
}

bool BillingService::moveJobToBatch(const QString& srcBatchId, std::uint32_t jobId,
                                     const QString& dstBatchId)
{
  const std::string srcId = srcBatchId.toStdString();
  const std::string dstId = dstBatchId.toStdString();
  auto srcIt = batchIndex_.find(srcId);
  auto dstIt = batchIndex_.find(dstId);
  if (srcIt == batchIndex_.end() || dstIt == batchIndex_.end()) return false;
  auto& src = batches_[srcIt->second];
  auto& dst = batches_[dstIt->second];
  if (src.is_settled() || dst.is_settled()) return false;

  auto jidIt = std::find(src.job_ids.begin(), src.job_ids.end(), jobId);
  if (jidIt == src.job_ids.end()) return false;
  src.job_ids.erase(jidIt);
  dst.job_ids.push_back(jobId);

  recomputeBatchTotals(src);
  recomputeBatchTotals(dst);
  scheduleSave();
  emit batchUpdated(srcBatchId);
  emit batchUpdated(dstBatchId);
  return true;
}

void BillingService::setColorPages(std::uint32_t jobId, int colorPages)
{
  auto it = jobRegistry_.find(jobId);
  if (it == jobRegistry_.end()) return;

  if (colorPages < 0) colorPages = 0;

  // Clamp to total_pages (color pages can't exceed total pages).
  const int totalPages = common::parse_total_pages(it->second.metadata);
  if (colorPages > totalPages) colorPages = totalPages;

  it->second.pricing.color_pages = colorPages;

  // Recompute pricing with the new color_pages value.
  const common::PricingConfig& pc = pricingConfig_ ? *pricingConfig_ : defaultPricing_;
  const common::PrinterProfileConfig& pp = printerProfiles_ ? *printerProfiles_ : defaultPrinters_;
  it->second.pricing = common::compute_job_pricing(it->second.metadata, pc, pp, colorPages);
  it->second.last_updated_utc = utcNowIso8601();

  // Find the owning batch and recompute its totals.
  for (auto& batch : batches_)
  {
    auto jidIt = std::find(batch.job_ids.begin(), batch.job_ids.end(), jobId);
    if (jidIt != batch.job_ids.end())
    {
      recomputeBatchTotals(batch);
      emit batchUpdated(QString::fromStdString(batch.batch_id));
      scheduleSave();
      return;
    }
  }
}

bool BillingService::deleteJob(std::uint32_t jobId)
{
  auto it = jobRegistry_.find(jobId);
  if (it == jobRegistry_.end()) return false;

  // Remove from any batch that contains it.
  for (auto& batch : batches_)
  {
    if (batch.is_settled()) continue;
    auto jidIt = std::find(batch.job_ids.begin(), batch.job_ids.end(), jobId);
    if (jidIt != batch.job_ids.end())
    {
      batch.job_ids.erase(jidIt);
      recomputeBatchTotals(batch);
      emit batchUpdated(QString::fromStdString(batch.batch_id));
    }
  }
  jobRegistry_.erase(it);
  scheduleSave();
  return true;
}

std::string BillingService::todayDateString()
{
  const auto now = std::chrono::system_clock::now();
  const auto t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  return buf;
}

}  // namespace wpce::ui
