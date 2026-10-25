/// @file billing_service.cpp
/// BillingService implementation.

#include "billing/billing_service.h"

#include "bridge/settlement_bridge.h"
#include "common/app_config.h"
#include "common/business_clock.h"
#include "common/document_name.h"
#include "common/logger.h"
#include "common/pricing_engine.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <charconv>
#include <type_traits>
#include <ctime>
#include <sstream>
#include <unordered_set>

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

std::string physicalJobFingerprint(const core::JobSnapshot& snapshot)
{
  if (!snapshot.idempotency_key.empty()) return snapshot.idempotency_key;
  if (snapshot.printer_name.empty() ||
      snapshot.document_name.empty() ||
      snapshot.submit_time_utc.empty())
  {
    return {};
  }

  return snapshot.printer_name + "\x1f" +
         snapshot.user + "\x1f" +
         snapshot.document_name + "\x1f" +
         snapshot.submit_time_utc;
}

void preserve_manual_price(core::JobPricingEntry& computed,
                           const core::JobPricingEntry& previous)
{
  if (!previous.manual_price_override) return;
  computed.manual_price_override = true;
  computed.manual_total_price = previous.manual_total_price;
  computed.total_price = previous.manual_total_price;
  computed.price_pending = false;
  computed.pending_reason.clear();
  computed.note = "manual_price_override";
}

}  // namespace

BillingService::BillingService(QObject* parent)
  : QObject(parent), defaultPricing_(wpce::common::AppConfig::defaults().pricing)
{
  pricingConfig_ = &defaultPricing_;

  saveTimer_.setSingleShot(true);
  saveTimer_.setInterval(1500);
  connect(&saveTimer_, &QTimer::timeout, this, &BillingService::flushSave);

  // Check day changes every second; retain the 30-second safety flush.
  periodicSaveTimer_.setInterval(1000);
  periodicSaveTimer_.setSingleShot(false);
  connect(&periodicSaveTimer_, &QTimer::timeout, this, [this, ticks = 0]() mutable {
    if (!synchronizeBusinessDay()) return;
    if (!replayDeferredEvents()) return;
    if (!synchronizeBusinessDay()) return;
    if (++ticks % 30 == 0) flushSave();
  });
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

}

void BillingService::setOrderBucketPath(const QString& orderBucketPath)
{
  if (dirty_)
  {
    flushSave();
    if (dirty_) { reportStorageError("Cannot switch buckets while the previous bucket has unsaved changes."); return; }
  }
  orderBucketPath_ = orderBucketPath;
  const QFileInfo info(orderBucketPath_);
  repository_.setFilePath(orderBucketPath_.toStdWString());
  const auto date = std::filesystem::path(orderBucketPath_.toStdWString()).parent_path().filename().string();
  business_day_.bind(common::parse_business_date(date) ? date : business_day_.today());
  storage_ready_ = false;
  load();
}

void BillingService::setBusinessDateProvider(common::BusinessDayCoordinator::DateProvider provider)
{
  business_day_.setDateProvider(std::move(provider));
}

void BillingService::reportStorageError(const std::string& message)
{
  const auto value = QStringLiteral("账务数据未能安全读写，已保留原文件。请检查数据目录后重新加载，暂缓结算。");
  if (storage_error_ != value)
  { common::log_warn("billing", message); storage_error_ = value; emit storageError(value); }
}

std::string BillingService::compactBoundDate() const
{
  auto date = business_day_.boundDate();
  date.erase(std::remove(date.begin(), date.end(), '-'), date.end());
  return date;
}

bool BillingService::synchronizeBusinessDay()
{
  if (switching_day_ || orderBucketPath_.isEmpty()) return false;
  const auto today = processing_date_.empty() ? business_day_.today() : processing_date_;
  if (today == business_day_.boundDate()) return storage_ready_;
  if (!storage_ready_) return false;
  flushSave();
  if (dirty_) return false;
  const auto nextPath = repository_.filePath().parent_path().parent_path() / today / "orders.json";
  domain::BillingBatchRepository next;
  next.setFilePath(nextPath);
  std::vector<domain::BillingBatch> tasks;
  std::unordered_map<std::uint32_t, domain::PrintJobRecord> jobs;
  std::vector<core::SettlementOrder> orders;
  std::string date;
  const auto result = next.loadDaily(tasks, jobs, orders, date);
  if (!result.ok() || (!date.empty() && date != today))
  { reportStorageError("Cannot open the new business-day bucket: " + result.message); return false; }
  if (!prepareCounters(tasks, orders, today)) return false;
  for (const auto& [id, record] : jobRegistry_)
  { previous_day_job_ids_.insert(id); previous_day_fingerprints_[id] = physicalJobFingerprint(record.snapshot); }
  for (const auto& [id, canonical] : duplicateJobAliases_)
  {
    previous_day_job_ids_.insert(id);
    if (jobRegistry_.contains(canonical)) previous_day_fingerprints_[id] = physicalJobFingerprint(jobRegistry_.at(canonical).snapshot);
  }
  switching_day_ = true;
  saveTimer_.stop();
  business_day_.bind(today);
  orderBucketPath_ = QString::fromStdWString(nextPath.wstring());
  repository_ = std::move(next);
  storage_ready_ = true; storage_error_.clear();
  adoptLoaded(std::move(tasks), std::move(jobs), std::move(orders));
  switching_day_ = false;
  if (!storage_ready_) return false;
  emit businessDayChanged(QString::fromStdString(today));
  return true;
}

void BillingService::setPricingConfig(const wpce::common::PricingConfig* pricing)
{
  pricingConfig_ = (pricing != nullptr) ? pricing : &defaultPricing_;
  if (!jobRegistry_.empty()) repriceUnsettledJobs();
}

void BillingService::setPrinterProfiles(const wpce::common::PrinterProfileConfig* printers)
{
  printerProfiles_ = (printers != nullptr) ? printers : &defaultPrinters_;
  if (!jobRegistry_.empty()) repriceUnsettledJobs();
}

void BillingService::load()
{
  if (dirty_) { flushSave(); if (dirty_) return; }
  std::vector<domain::BillingBatch> loaded;
  std::unordered_map<std::uint32_t, domain::PrintJobRecord> loadedJobs;
  std::vector<core::SettlementOrder> loadedOrders;
  std::string savedDate;
  const auto result = repository_.loadDaily(loaded, loadedJobs, loadedOrders, savedDate);
  storage_ready_ = result.ok() && (savedDate.empty() || savedDate == business_day_.boundDate());
  if (!storage_ready_)
  { reportStorageError(result.message.empty() ? "Bucket business date mismatch; writes blocked." : result.message); return; }
  if (!prepareCounters(loaded, loadedOrders, business_day_.boundDate()))
  { storage_ready_ = false; return; }
  storage_error_.clear();
  dirty_ = false;
  {
    adoptLoaded(std::move(loaded), std::move(loadedJobs), std::move(loadedOrders));
  }
}

bool BillingService::prepareCounters(const std::vector<domain::BillingBatch>& tasks,
    const std::vector<core::SettlementOrder>& orders, const std::string& date)
{
  auto compact = date;
  compact.erase(std::remove(compact.begin(), compact.end(), '-'), compact.end());
  long maxBatch = 0, maxOrder = 0;
  auto update = [](const std::string& id, const std::string& prefix, long& floor) {
    if (!id.starts_with(prefix)) return true; // historical aliases do not allocate numeric ids
    long value = 0;
    const auto first = id.data() + prefix.size();
    const auto result = std::from_chars(first, id.data() + id.size(), value);
    if (result.ec != std::errc{} || result.ptr != id.data() + id.size() || value < 0) return false;
    floor = std::max(floor, value); return true;
  };
  for (const auto& task : tasks)
    if (!update(task.batch_id, "B-" + compact + "-", maxBatch))
    { reportStorageError("Invalid task counter suffix."); return false; }
  for (const auto& order : orders)
    if (!update(order.order_id, "SETTLE-" + compact + "-", maxOrder))
    { reportStorageError("Invalid order counter suffix."); return false; }
  if (!counterStore_.ensureMinimum(compact, maxBatch, maxOrder))
  { reportStorageError("Counter state is unreadable or cannot be persisted; allocation blocked."); return false; }
  return true;
}

void BillingService::adoptLoaded(std::vector<domain::BillingBatch> loaded,
    std::unordered_map<std::uint32_t, domain::PrintJobRecord> loadedJobs,
    std::vector<core::SettlementOrder> loadedOrders)
{
  batches_ = std::move(loaded);
  jobRegistry_ = std::move(loadedJobs);
  settlementOrders_ = std::move(loadedOrders);
  for (const auto& [id, record] : jobRegistry_)
  { (void)record; previous_day_job_ids_.erase(id); previous_day_fingerprints_.erase(id); }
  jobLoadedBatchIds_.clear();
  batchIndex_.clear();
  jobToBatchId_.clear();
  jobFingerprintToJobId_.clear();
  duplicateJobAliases_.clear();
  ignoredPlaceholderJobIds_.clear();
  for (std::size_t i = 0; i < batches_.size(); ++i)
  {
    batchIndex_[batches_[i].batch_id] = i;
    for (auto jid : batches_[i].job_ids)
      jobToBatchId_[jid] = batches_[i].batch_id;
  }

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

  for (const auto& batch : batches_)
  {
    jobLoadedBatchIds_.insert(batch.batch_id);
    for (auto jid : batch.job_ids)
    {
      auto job = jobRegistry_.find(jid);
      if (job != jobRegistry_.end()) registerJobFingerprint(job->second);
    }
  }

  emit batchesChanged();
}

void BillingService::flushSave()
{
  if (!dirty_ || !storage_ready_) return;

  if (!ensureAllBatchJobsLoaded())
  {
    reportStorageError("Incomplete job registry; bucket overwrite refused.");
    return;
  }

  for (auto& batch : batches_)
    if (!batch.is_settled()) (void)normalizeBatchJobIds(batch);

  // Only persist jobs that belong to a known batch (excludes unassigned jobs).
  std::unordered_map<std::uint32_t, domain::PrintJobRecord> batchJobs;
  for (const auto& batch : batches_)
  {
    std::unordered_set<std::uint32_t> seen;
    for (auto jid : batch.job_ids)
    {
      jid = canonicalJobId(jid);
      if (jid == 0 || !seen.insert(jid).second) continue;
      auto it = jobRegistry_.find(jid);
      if (it != jobRegistry_.end())
        batchJobs[jid] = it->second;
    }
  }

  dirty_ = !repository_.save(batches_, batchJobs, settlementOrders_, business_day_.boundDate());
  if (dirty_) reportStorageError(repository_.lastError());
  else storage_error_.clear();
}

void BillingService::clear()
{
  if (!synchronizeBusinessDay()) return;
  saveTimer_.stop();
  for (const auto& batch : batches_)
    if (!batch.is_settled())
      for (auto id : batch.job_ids) { forgetJobIndexes(id); jobRegistry_.erase(id); }
  std::erase_if(batches_, [](const auto& batch) { return !batch.is_settled(); });
  batchIndex_.clear(); jobToBatchId_.clear(); jobLoadedBatchIds_.clear();
  for (std::size_t i = 0; i < batches_.size(); ++i)
  {
    batchIndex_[batches_[i].batch_id] = i;
    jobLoadedBatchIds_.insert(batches_[i].batch_id);
    for (auto id : batches_[i].job_ids) jobToBatchId_[id] = batches_[i].batch_id;
  }
  currentActiveBatchId_.clear();
  dirty_ = true; flushSave();
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
  jobId = canonicalJobId(jobId);
  auto it = jobRegistry_.find(jobId);
  if (it == jobRegistry_.end()) return nullptr;
  return &it->second;
}

bool BillingService::areBatchJobsLoaded(const QString& batchId) const
{
  return jobLoadedBatchIds_.find(batchId.toStdString()) != jobLoadedBatchIds_.end();
}

bool BillingService::ensureBatchJobsLoaded(const QString& batchId)
{
  if (batchId.isEmpty()) return false;
  return loadBatchJobs(batchId.toStdString(), true);
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
  std::unordered_set<std::uint32_t> seen;
  for (const auto& batch : batches_)
    for (auto jid : batch.job_ids)
    {
      jid = canonicalJobId(jid);
      if (jid != 0)
        seen.insert(jid);
    }
  return static_cast<int>(seen.size());
}

double BillingService::todayRevenue() const
{
  double total = 0.0;
  for (const auto& b : batches_)
    if (!b.is_settled()) total += b.total_price;
  return total;
}

std::uint32_t BillingService::maxStoredJobId() const
{
  std::uint32_t maxId = 0;
  for (const auto& batch : batches_)
    for (auto jid : batch.job_ids)
      if (jid > maxId) maxId = jid;
  return maxId;
}

const domain::BillingBatch* BillingService::currentActiveBatch() const
{
  if (currentActiveBatchId_.empty()) return nullptr;
  return batchById(currentActiveBatchId_);
}

domain::BillingBatch* BillingService::batchContainingJob(std::uint32_t jobId)
{
  jobId = canonicalJobId(jobId);
  auto it = jobToBatchId_.find(jobId);
  if (it == jobToBatchId_.end()) return nullptr;
  return batchById(it->second);
}

std::uint32_t BillingService::canonicalJobId(std::uint32_t jobId) const
{
  for (int depth = 0; depth < 8; ++depth)
  {
    auto it = duplicateJobAliases_.find(jobId);
    if (it == duplicateJobAliases_.end() || it->second == 0 || it->second == jobId)
      return jobId;
    jobId = it->second;
  }
  return jobId;
}

std::uint32_t BillingService::findDuplicateJobForSnapshot(
    const core::JobSnapshot& snapshot,
    std::uint32_t incomingJobId) const
{
  const std::string fingerprint = physicalJobFingerprint(snapshot);
  if (fingerprint.empty()) return 0;

  auto it = jobFingerprintToJobId_.find(fingerprint);
  if (it == jobFingerprintToJobId_.end()) return 0;

  const std::uint32_t canonical = canonicalJobId(it->second);
  if (canonical == 0 || canonical == incomingJobId) return 0;
  return canonical;
}

void BillingService::registerJobFingerprint(const domain::PrintJobRecord& record)
{
  const std::string fingerprint = physicalJobFingerprint(record.snapshot);
  if (fingerprint.empty() || record.job_id == 0) return;

  const std::uint32_t canonical = canonicalJobId(record.job_id);
  auto [it, inserted] = jobFingerprintToJobId_.try_emplace(fingerprint, canonical);
  if (!inserted)
  {
    const std::uint32_t existingCanonical = canonicalJobId(it->second);
    if (existingCanonical != 0 && existingCanonical != canonical)
    {
      duplicateJobAliases_[record.job_id] = existingCanonical;
      it->second = existingCanonical;
      return;
    }
    it->second = canonical;
  }
}

void BillingService::forgetJobIndexes(std::uint32_t jobId)
{
  jobToBatchId_.erase(jobId);
  duplicateJobAliases_.erase(jobId);
  for (auto it = duplicateJobAliases_.begin(); it != duplicateJobAliases_.end(); )
  {
    if (it->second == jobId)
      it = duplicateJobAliases_.erase(it);
    else
      ++it;
  }
  for (auto it = jobFingerprintToJobId_.begin(); it != jobFingerprintToJobId_.end(); )
  {
    if (it->second == jobId)
      it = jobFingerprintToJobId_.erase(it);
    else
      ++it;
  }
}

void BillingService::mergeDuplicateJob(std::uint32_t duplicateJobId,
                                       std::uint32_t canonicalId,
                                       core::JobSnapshot snapshot)
{
  canonicalId = canonicalJobId(canonicalId);
  if (duplicateJobId == 0 || canonicalId == 0 || duplicateJobId == canonicalId)
    return;

  domain::BillingBatch* duplicateBatch = nullptr;
  if (auto owner = jobToBatchId_.find(duplicateJobId); owner != jobToBatchId_.end())
  {
    duplicateBatch = batchById(owner->second);
  }

  duplicateJobAliases_[duplicateJobId] = canonicalId;

  if (duplicateBatch != nullptr)
  {
    auto& ids = duplicateBatch->job_ids;
    ids.erase(std::remove(ids.begin(), ids.end(), duplicateJobId), ids.end());
    jobToBatchId_.erase(duplicateJobId);
  }

  if (jobRegistry_.find(canonicalId) == jobRegistry_.end())
  {
    if (auto* existingBatch = batchContainingJob(canonicalId); existingBatch != nullptr)
      (void)loadBatchJobs(existingBatch->batch_id, false);
  }

  auto canonicalIt = jobRegistry_.find(canonicalId);
  if (canonicalIt == jobRegistry_.end()) return;

  auto* canonicalBatch = batchContainingJob(canonicalId);
  const bool frozen = canonicalBatch && canonicalBatch->is_settled();
  auto duplicateIt = jobRegistry_.find(duplicateJobId);
  if (duplicateIt != jobRegistry_.end())
  {
    if (!frozen && canonicalIt->second.metadata.job_id == 0 && duplicateIt->second.metadata.job_id != 0)
    {
      canonicalIt->second.metadata = duplicateIt->second.metadata;
      canonicalIt->second.metadata.job_id = canonicalId;
    }
    if (!frozen && canonicalIt->second.pricing.job_id == 0 && duplicateIt->second.pricing.job_id != 0)
    {
      canonicalIt->second.pricing = duplicateIt->second.pricing;
      canonicalIt->second.pricing.job_id = canonicalId;
    }
    jobRegistry_.erase(duplicateIt);
  }

  if (!frozen)
  {
    snapshot.job_id = canonicalId;
    canonicalIt->second.snapshot = std::move(snapshot);
    canonicalIt->second.last_updated_utc = utcNowIso8601();
    canonicalIt->second.metadata.job_id = canonicalId;
    const int existing_color_pages = canonicalIt->second.pricing.color_pages;
    const wpce::common::PricingConfig& pc = pricingConfig_ ? *pricingConfig_ : defaultPricing_;
    const wpce::common::PrinterProfileConfig& pp = printerProfiles_ ? *printerProfiles_ : defaultPrinters_;
    const auto previousPricing = canonicalIt->second.pricing;
    canonicalIt->second.pricing =
      wpce::common::compute_job_pricing(canonicalIt->second.metadata, pc, pp, existing_color_pages);
    preserve_manual_price(canonicalIt->second.pricing, previousPricing);
    registerJobFingerprint(canonicalIt->second);

  }
  if (canonicalBatch != nullptr)
  {
    recomputeBatchTotals(*canonicalBatch);
    emit batchUpdated(QString::fromStdString(canonicalBatch->batch_id));
    scheduleSave();
  }
  if (duplicateBatch != nullptr && duplicateBatch != canonicalBatch)
  {
    recomputeBatchTotals(*duplicateBatch);
    emit batchUpdated(QString::fromStdString(duplicateBatch->batch_id));
    scheduleSave();
  }
}

bool BillingService::loadBatchJobs(const std::string& batchId, bool emitSignals)
{
  if (batchId.empty()) return false;
  if (jobLoadedBatchIds_.find(batchId) != jobLoadedBatchIds_.end()) return true;

  auto* batch = batchById(batchId);
  if (batch == nullptr) return false;

  if (batch->job_ids.empty())
  {
    jobLoadedBatchIds_.insert(batchId);
    return true;
  }

  std::unordered_map<std::uint32_t, domain::PrintJobRecord> loadedJobs;
  if (!repository_.loadJobsForBatches({batchId}, loadedJobs)) return false;

  for (auto jid : batch->job_ids)
  {
    auto it = loadedJobs.find(jid);
    if (it != loadedJobs.end())
    {
      jobRegistry_[jid] = std::move(it->second);
      registerJobFingerprint(jobRegistry_[jid]);
    }
  }
  jobLoadedBatchIds_.insert(batchId);
  const bool normalized = normalizeBatchJobIds(*batch);
  if (normalized)
  {
    recomputeBatchTotals(*batch);
    scheduleSave();
  }

  if (emitSignals)
    emit batchUpdated(QString::fromStdString(batchId));
  return true;
}

void BillingService::preloadRecentBatchJobs(std::size_t batchCount)
{
  if (batches_.empty() || batchCount == 0) return;

  std::vector<std::string> batchIds;
  const std::size_t start =
    batches_.size() > batchCount ? batches_.size() - batchCount : 0;
  for (std::size_t i = start; i < batches_.size(); ++i)
    batchIds.push_back(batches_[i].batch_id);
  if (!currentActiveBatchId_.empty() &&
      std::find(batchIds.begin(), batchIds.end(), currentActiveBatchId_) == batchIds.end())
  {
    batchIds.push_back(currentActiveBatchId_);
  }

  std::unordered_map<std::uint32_t, domain::PrintJobRecord> loadedJobs;
  if (!repository_.loadJobsForBatches(batchIds, loadedJobs)) return;

  for (const std::string& batchId : batchIds)
  {
    auto* batch = batchById(batchId);
    if (batch == nullptr) continue;
    for (auto jid : batch->job_ids)
    {
      auto it = loadedJobs.find(jid);
      if (it != loadedJobs.end())
      {
        jobRegistry_[jid] = std::move(it->second);
        registerJobFingerprint(jobRegistry_[jid]);
      }
    }
    jobLoadedBatchIds_.insert(batchId);
    if (normalizeBatchJobIds(*batch))
    {
      recomputeBatchTotals(*batch);
      scheduleSave();
    }
  }
}

bool BillingService::ensureAllBatchJobsLoaded()
{
  std::vector<std::string> missingBatchIds;
  for (const auto& batch : batches_)
  {
    if (jobLoadedBatchIds_.find(batch.batch_id) == jobLoadedBatchIds_.end())
      missingBatchIds.push_back(batch.batch_id);
  }
  if (missingBatchIds.empty()) return true;

  std::unordered_map<std::uint32_t, domain::PrintJobRecord> loadedJobs;
  if (!repository_.loadJobsForBatches(missingBatchIds, loadedJobs)) return false;

  for (const std::string& batchId : missingBatchIds)
  {
    auto* batch = batchById(batchId);
    if (batch == nullptr) continue;
    for (auto jid : batch->job_ids)
    {
      if (jobRegistry_.find(jid) != jobRegistry_.end()) continue;
      auto it = loadedJobs.find(jid);
      if (it != loadedJobs.end())
      {
        jobRegistry_[jid] = std::move(it->second);
        registerJobFingerprint(jobRegistry_[jid]);
      }
    }
    jobLoadedBatchIds_.insert(batchId);
    if (normalizeBatchJobIds(*batch))
    {
      recomputeBatchTotals(*batch);
      scheduleSave();
    }
  }
  return true;
}

bool BillingService::batchMatchesSearch(const QString& batchId, const QString& query)
{
  const QString norm = query.trimmed();
  if (norm.isEmpty()) return true;

  const auto* batch = batchById(batchId.toStdString());
  if (batch == nullptr) return false;

  const auto contains = [&norm](const std::string& value) {
    return QString::fromStdString(value).contains(norm, Qt::CaseInsensitive);
  };

  if (contains(batch->display_name) ||
      contains(batch->first_doc_anchor.document_name) ||
      contains(batch->last_doc_anchor.document_name))
  {
    return true;
  }

  (void)loadBatchJobs(batch->batch_id, false);
  for (auto jid : batch->job_ids)
  {
    auto it = jobRegistry_.find(jid);
    if (it != jobRegistry_.end() && contains(it->second.snapshot.document_name))
      return true;
  }
  return false;
}

domain::BillingBatch* BillingService::createNewActiveBatch(const std::string& sourceUser)
{
  const std::string batchId = counterStore_.nextBatchId(compactBoundDate());
  if (batchId.empty())
  {
    common::log_warn("billing", "Failed to allocate batch id; new print job was not bound.");
    return nullptr;
  }

  // Generate the default display name from the batch counter.
  // Extract the numeric tail of the batch id for the display number.
  std::string displayName = QStringLiteral("鐢ㄦ埛#%1")
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
  return &batches_[idx];
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
  return createNewActiveBatch(snapshot.user);
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

bool BillingService::normalizeBatchJobIds(domain::BillingBatch& batch)
{
  if (batch.job_ids.empty()) return false;

  std::vector<std::uint32_t> normalizedIds;
  normalizedIds.reserve(batch.job_ids.size());
  std::unordered_set<std::uint32_t> seen;
  bool changed = false;

  for (std::uint32_t rawId : batch.job_ids)
  {
    const std::uint32_t canonical = canonicalJobId(rawId);
    if (canonical == 0)
    {
      changed = true;
      continue;
    }

    if (auto owner = jobToBatchId_.find(canonical);
        owner != jobToBatchId_.end() && owner->second != batch.batch_id)
    {
      if (rawId != canonical)
        jobToBatchId_.erase(rawId);
      changed = true;
      continue;
    }

    if (!seen.insert(canonical).second)
    {
      if (rawId != canonical)
      {
        jobToBatchId_.erase(rawId);
        jobRegistry_.erase(rawId);
      }
      changed = true;
      continue;
    }

    auto currentIt = jobRegistry_.find(canonical);
    if (currentIt != jobRegistry_.end() &&
        common::is_spooler_placeholder_document(currentIt->second.snapshot.document_name))
    {
      ignoredPlaceholderJobIds_.insert(canonical);
      forgetJobIndexes(canonical);
      jobRegistry_.erase(currentIt);
      if (rawId != canonical)
      {
        jobToBatchId_.erase(rawId);
        duplicateJobAliases_.erase(rawId);
        jobRegistry_.erase(rawId);
      }
      changed = true;
      continue;
    }

    if (rawId != canonical)
    {
      auto canonicalIt = jobRegistry_.find(canonical);
      auto duplicateIt = jobRegistry_.find(rawId);
      if (canonicalIt != jobRegistry_.end() && duplicateIt != jobRegistry_.end())
      {
        const int canonicalPages = wpce::common::parse_total_pages(canonicalIt->second.metadata);
        const int duplicatePages = wpce::common::parse_total_pages(duplicateIt->second.metadata);
        if ((canonicalIt->second.metadata.job_id == 0 || canonicalPages <= 0) &&
            duplicateIt->second.metadata.job_id != 0 && duplicatePages > 0)
        {
          canonicalIt->second.metadata = duplicateIt->second.metadata;
          canonicalIt->second.metadata.job_id = canonical;
        }
        if ((canonicalIt->second.pricing.job_id == 0 ||
             (canonicalIt->second.pricing.total_price <= 0.0 &&
              !canonicalIt->second.pricing.price_pending)) &&
            duplicateIt->second.pricing.job_id != 0)
        {
          canonicalIt->second.pricing = duplicateIt->second.pricing;
          canonicalIt->second.pricing.job_id = canonical;
        }
        if (canonicalIt->second.snapshot.document_name.empty() &&
            !duplicateIt->second.snapshot.document_name.empty())
        {
          canonicalIt->second.snapshot = duplicateIt->second.snapshot;
          canonicalIt->second.snapshot.job_id = canonical;
        }
        jobRegistry_.erase(duplicateIt);
      }
      else if (canonicalIt == jobRegistry_.end() && duplicateIt != jobRegistry_.end())
      {
        domain::PrintJobRecord moved = std::move(duplicateIt->second);
        jobRegistry_.erase(duplicateIt);
        moved.job_id = canonical;
        moved.snapshot.job_id = canonical;
        moved.metadata.job_id = canonical;
        moved.pricing.job_id = canonical;
        jobRegistry_[canonical] = std::move(moved);
      }

      jobToBatchId_.erase(rawId);
      changed = true;
    }

    normalizedIds.push_back(canonical);
    jobToBatchId_[canonical] = batch.batch_id;
  }

  if (changed)
    batch.job_ids = std::move(normalizedIds);

  for (std::size_t i = 0; i < batch.job_ids.size(); ++i)
  {
    auto it = jobRegistry_.find(batch.job_ids[i]);
    if (it != jobRegistry_.end())
      it->second.batch_job_seq = static_cast<std::uint32_t>(i + 1);
  }

  return changed;
}

void BillingService::recomputeBatchTotals(domain::BillingBatch& batch)
{
  if (batch.is_settled()) return;
  (void)normalizeBatchJobIds(batch);

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

// WatcherBridge slots
void BillingService::onJobSnapshot(core::JobSnapshot snapshot)
{
  if (!synchronizeBusinessDay()) { deferred_events_.push_back({processing_date_.empty() ? business_day_.today() : processing_date_, std::move(snapshot)}); return; }
  if (previous_day_job_ids_.contains(snapshot.job_id))
  {
    const auto previous = previous_day_fingerprints_.find(snapshot.job_id);
    if (previous == previous_day_fingerprints_.end() || previous->second.empty() ||
        previous->second == physicalJobFingerprint(snapshot)) return;
    previous_day_job_ids_.erase(snapshot.job_id);
    previous_day_fingerprints_.erase(snapshot.job_id);
  }
  if (snapshot.job_id == 0) return;

  const std::uint32_t incomingJobId = snapshot.job_id;
  if (const auto* owner = batchContainingJob(canonicalJobId(snapshot.job_id)); owner && owner->is_settled()) return;
  if (common::is_spooler_placeholder_document(snapshot.document_name))
  {
    ignoredPlaceholderJobIds_.insert(incomingJobId);
    bool changed = false;
    if (auto owner = jobToBatchId_.find(incomingJobId); owner != jobToBatchId_.end())
    {
      auto* batch = batchById(owner->second);
      if (batch != nullptr)
      {
        batch->job_ids.erase(
          std::remove_if(batch->job_ids.begin(), batch->job_ids.end(),
                         [incomingJobId](std::uint32_t jid) {
                           return jid == incomingJobId;
                         }),
          batch->job_ids.end());
        recomputeBatchTotals(*batch);
        emit batchUpdated(QString::fromStdString(batch->batch_id));
        changed = true;
      }
    }
    forgetJobIndexes(incomingJobId);
    changed = jobRegistry_.erase(incomingJobId) > 0 || changed;
    if (changed)
      scheduleSave();
    return;
  }

  std::uint32_t jobId = canonicalJobId(incomingJobId);
  if (jobId != incomingJobId)
  {
    snapshot.job_id = jobId;
  }
  else if (const std::uint32_t duplicateOf =
               findDuplicateJobForSnapshot(snapshot, incomingJobId);
           duplicateOf != 0)
  {
    mergeDuplicateJob(incomingJobId, duplicateOf, std::move(snapshot));
    return;
  }

  if (jobRegistry_.find(jobId) == jobRegistry_.end())
  {
    if (auto* existingBatch = batchContainingJob(jobId); existingBatch != nullptr)
      (void)loadBatchJobs(existingBatch->batch_id, false);
  }

  auto it = jobRegistry_.find(jobId);
  if (it != jobRegistry_.end())
  {
    const std::string existingFingerprint = physicalJobFingerprint(it->second.snapshot);
    const std::string incomingFingerprint = physicalJobFingerprint(snapshot);
    const bool differentPhysicalJob =
        !existingFingerprint.empty() && !incomingFingerprint.empty()
            ? existingFingerprint != incomingFingerprint
            : (it->second.snapshot.printer_name != snapshot.printer_name ||
               it->second.snapshot.user != snapshot.user ||
               it->second.snapshot.document_name != snapshot.document_name ||
               it->second.snapshot.submit_time_utc != snapshot.submit_time_utc);

    // WatcherBridge should prevent this by assigning synthetic ids after
    // restart. If a raw-id collision still reaches billing, protect the
    // already-settled order instead of mutating its job list.
    auto* oldBatch = batchContainingJob(jobId);
    if (oldBatch != nullptr && oldBatch->is_settled() && differentPhysicalJob)
    {
      common::log_warn("billing",
                       "Dropped colliding print snapshot for settled job id " +
                       std::to_string(jobId) + "; old settlement preserved.");
      return;
    }
    else
    {
    // Existing job - update snapshot.
    it->second.snapshot = std::move(snapshot);
    it->second.snapshot.job_id = jobId;
    it->second.last_updated_utc = utcNowIso8601();
    registerJobFingerprint(it->second);
    // Metadata may have created this record before the job snapshot;
    // bind it to the active batch the first time the snapshot arrives.
    auto* batch = batchContainingJob(jobId);
    if (batch == nullptr)
    {
      batch = ensureActiveBatch(it->second.snapshot);
      if (batch == nullptr)
      { deferred_events_.push_back({processing_date_.empty() ? business_day_.today() : processing_date_, it->second.snapshot}); return; }
      if (std::find(batch->job_ids.begin(), batch->job_ids.end(), jobId) == batch->job_ids.end())
        batch->job_ids.push_back(jobId);
      it->second.batch_job_seq = static_cast<std::uint32_t>(batch->job_ids.size());
      jobToBatchId_[jobId] = batch->batch_id;
      jobLoadedBatchIds_.insert(batch->batch_id);
      if (batch->period_start_utc.empty())
        batch->period_start_utc = it->second.snapshot.submit_time_utc;
      recomputeBatchTotals(*batch);
      scheduleSave();
      emit batchUpdated(QString::fromStdString(batch->batch_id));
      emit batchesChanged();
      return;
    }

    // Batch binding is sticky once established.
    recomputeBatchTotals(*batch);
    scheduleSave();
    emit batchUpdated(QString::fromStdString(batch->batch_id));
    return;
    }  // else (existing job, no settled-batch collision)
  }

  // New job - bind to currentActiveBatch (creating one if needed).
  domain::BillingBatch* batch = ensureActiveBatch(snapshot);
  if (batch == nullptr)
  { deferred_events_.push_back({processing_date_.empty() ? business_day_.today() : processing_date_, std::move(snapshot)}); return; }

  domain::PrintJobRecord rec;
  rec.job_id = jobId;
  rec.batch_job_seq = static_cast<std::uint32_t>(batch->job_ids.size() + 1);
  rec.snapshot = std::move(snapshot);
  rec.snapshot.job_id = jobId;
  rec.last_updated_utc = utcNowIso8601();
  jobRegistry_[jobId] = std::move(rec);
  registerJobFingerprint(jobRegistry_[jobId]);

  if (std::find(batch->job_ids.begin(), batch->job_ids.end(), jobId) == batch->job_ids.end())
    batch->job_ids.push_back(jobId);
  jobToBatchId_[jobId] = batch->batch_id;
  jobLoadedBatchIds_.insert(batch->batch_id);
  if (batch->period_start_utc.empty())
    batch->period_start_utc = jobRegistry_[jobId].snapshot.submit_time_utc;

  recomputeBatchTotals(*batch);
  scheduleSave();
  emit batchUpdated(QString::fromStdString(batch->batch_id));
  emit batchesChanged();
}

void BillingService::onMetadata(core::MetadataSnapshot metadata)
{
  if (!synchronizeBusinessDay()) { deferred_events_.push_back({processing_date_.empty() ? business_day_.today() : processing_date_, std::move(metadata)}); return; }
  if (previous_day_job_ids_.contains(metadata.job_id)) return;
  if (metadata.job_id == 0) return;
  metadata.job_id = canonicalJobId(metadata.job_id);
  if (ignoredPlaceholderJobIds_.find(metadata.job_id) != ignoredPlaceholderJobIds_.end())
    return;

  if (jobRegistry_.find(metadata.job_id) == jobRegistry_.end())
  {
    if (auto* existingBatch = batchContainingJob(metadata.job_id); existingBatch != nullptr)
      (void)loadBatchJobs(existingBatch->batch_id, false);
  }

  auto it = jobRegistry_.find(metadata.job_id);
  if (it == jobRegistry_.end())
  {
    // Metadata arrived before snapshot - create a stub record so we
    // don't lose it.  The snapshot will fill in the rest.
    domain::PrintJobRecord rec;
    rec.job_id = metadata.job_id;
    rec.last_updated_utc = utcNowIso8601();
    it = jobRegistry_.emplace(metadata.job_id, std::move(rec)).first;
  }

  if (auto* owner = batchContainingJob(metadata.job_id); owner && owner->is_settled())
  {
    if (metadata.pages_printed.state == core::FieldState::Reported)
      it->second.metadata.pages_printed = metadata.pages_printed;
    it->second.last_updated_utc = utcNowIso8601();
    scheduleSave(); emit batchUpdated(QString::fromStdString(owner->batch_id)); return;
  }

  const int existing_total_pages = wpce::common::parse_total_pages(it->second.metadata);
  const int incoming_total_pages = wpce::common::parse_total_pages(metadata);
  if (existing_total_pages > 0 && incoming_total_pages <= 0)
  {
    metadata.total_pages = it->second.metadata.total_pages;
  }

  it->second.metadata = std::move(metadata);
  it->second.last_updated_utc = utcNowIso8601();

  // Recompute pricing - preserve operator-set color_pages.
  const int existing_color_pages = it->second.pricing.color_pages;
  const wpce::common::PricingConfig& pc = pricingConfig_ ? *pricingConfig_ : defaultPricing_;
  const wpce::common::PrinterProfileConfig& pp = printerProfiles_ ? *printerProfiles_ : defaultPrinters_;
  const auto previousPricing = it->second.pricing;
  it->second.pricing = wpce::common::compute_job_pricing(it->second.metadata, pc, pp, existing_color_pages);
  preserve_manual_price(it->second.pricing, previousPricing);

  // Recompute the owning batch's totals.
  if (auto* batch = batchContainingJob(metadata.job_id); batch != nullptr)
  {
    recomputeBatchTotals(*batch);
    emit batchUpdated(QString::fromStdString(batch->batch_id));
    scheduleSave();
  }
}

void BillingService::onLifecycleEvent(core::JobLifecycleEvent event)
{
  if (!synchronizeBusinessDay()) { deferred_events_.push_back({processing_date_.empty() ? business_day_.today() : processing_date_, std::move(event)}); return; }
  if (previous_day_job_ids_.contains(event.job_id)) return;
  if (event.job_id == 0) return;
  event.job_id = canonicalJobId(event.job_id);
  if (ignoredPlaceholderJobIds_.find(event.job_id) != ignoredPlaceholderJobIds_.end())
    return;

  if (jobRegistry_.find(event.job_id) == jobRegistry_.end())
  {
    if (auto* existingBatch = batchContainingJob(event.job_id); existingBatch != nullptr)
      (void)loadBatchJobs(existingBatch->batch_id, false);
  }

  auto it = jobRegistry_.find(event.job_id);
  if (it == jobRegistry_.end()) return;

  it->second.phase = event.phase;
  it->second.last_updated_utc = utcNowIso8601();
  if (auto* owner = batchContainingJob(event.job_id); owner && owner->is_settled())
  {
    it->second.metadata.pages_printed = {std::to_string(event.pages_printed), core::FieldState::Reported};
    scheduleSave(); emit batchUpdated(QString::fromStdString(owner->batch_id)); return;
  }
  if (event.total_pages > 0)
  {
    it->second.metadata.total_pages =
      core::FieldValue{std::to_string(event.total_pages), core::FieldState::Reported};
  }
  it->second.metadata.pages_printed =
    core::FieldValue{std::to_string(event.pages_printed), core::FieldState::Reported};

  // Recompute pricing in case page count changed - preserve operator-set color_pages.
  const int existing_color_pages = it->second.pricing.color_pages;
  const wpce::common::PricingConfig& pc = pricingConfig_ ? *pricingConfig_ : defaultPricing_;
  const wpce::common::PrinterProfileConfig& pp = printerProfiles_ ? *printerProfiles_ : defaultPrinters_;
  const auto previousPricing = it->second.pricing;
  it->second.pricing = wpce::common::compute_job_pricing(it->second.metadata, pc, pp, existing_color_pages);
  preserve_manual_price(it->second.pricing, previousPricing);

  if (auto* batch = batchContainingJob(event.job_id); batch != nullptr)
  {
    recomputeBatchTotals(*batch);
    emit batchUpdated(QString::fromStdString(batch->batch_id));
    scheduleSave();
  }
}

// Operator actions
QString BillingService::settle(const QString& batchId)
{
  if (!synchronizeBusinessDay()) return {};
  const std::string id = batchId.toStdString();
  auto it = batchIndex_.find(id);
  if (it == batchIndex_.end()) return {};
  if (!loadBatchJobs(id, true)) return {};
  auto& batch = batches_[it->second];
  if (batch.is_settled() && !batch.settle_order_id.empty())
    return QString::fromStdString(batch.settle_order_id);

  // Reconcile a partially recovered state before allocating a new counter ID.
  // The order bucket is authoritative: one task can own at most one order.
  for (const auto& existing : settlementOrders_)
  {
    const std::string taskId = existing.task_id.empty() ? existing.batch_id : existing.task_id;
    if (taskId != id) continue;
    batch.settle_order_id = existing.order_id;
    batch.state = domain::BatchState::Settled;
    if (batch.period_end_utc.empty()) batch.period_end_utc = existing.settled_at_utc;
    if (currentActiveBatchId_ == id) currentActiveBatchId_.clear();
    dirty_ = true;
    flushSave();
    emit batchUpdated(batchId);
    emit batchesChanged();
    return QString::fromStdString(existing.order_id);
  }
  if (!batch.is_active()) return {};

  recomputeBatchTotals(batch);
  if (batch.pending_count > 0)
  {
    common::log_warn("settle", "Refused settlement with pending prices for task: " + id);
    return {};
  }

  const std::string orderId = counterStore_.nextOrderId(compactBoundDate());
  if (orderId.empty())
  {
    common::log_warn("settle", "Failed to allocate settlement order id for batch: " + id);
    return {};
  }
  const domain::BillingBatch previousBatch = batch;
  const std::string previousActiveBatchId = currentActiveBatchId_;
  const std::size_t previousOrderCount = settlementOrders_.size();
  batch.period_end_utc = utcNowIso8601();
  batch.settle_order_id = settleBatchInternal(batch, orderId);
  if (batch.settle_order_id.empty())
  {
    batch.period_end_utc.clear();
    return {};
  }

  // Recompute totals (final pricing may have been adjusted by dialog).
  recomputeBatchTotals(batch);

  batch.state = domain::BatchState::Settled;

  // Clear currentActive if it was this batch.
  if (currentActiveBatchId_ == id) currentActiveBatchId_.clear();

  dirty_ = true;
  flushSave();
  if (dirty_)
  {
    batch = previousBatch;
    currentActiveBatchId_ = previousActiveBatchId;
    settlementOrders_.resize(previousOrderCount);
    common::log_warn("settle", "Daily order bucket write failed; settlement rolled back: " + id);
    return {};
  }
  emit batchUpdated(batchId);
  emit batchesChanged();
  emit orderSettled(QString::fromStdString(batch.settle_order_id), batchId);
  return QString::fromStdString(batch.settle_order_id);
}

std::string BillingService::settleBatchInternal(domain::BillingBatch& batch,
                                                 const std::string& orderId)
{
  if (orderId.empty()) return {};

  for (const auto& existing : settlementOrders_)
  {
    const std::string taskId = existing.task_id.empty() ? existing.batch_id : existing.task_id;
    if (taskId == batch.batch_id || existing.order_id == orderId)
      return existing.order_id;
  }

  (void)normalizeBatchJobIds(batch);

  // Collect job ids and pricing entries.
  std::vector<std::uint32_t> jobIds;
  std::vector<core::JobPricingEntry> pricingEntries;
  std::unordered_set<std::uint32_t> seen;
  for (auto jid : batch.job_ids)
  {
    jid = canonicalJobId(jid);
    if (jid == 0 || !seen.insert(jid).second) continue;
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
  order.task_id = batch.batch_id;
  order.first_document_name = batch.first_doc_anchor.document_name;
  order.last_document_name = batch.last_doc_anchor.document_name;
  const std::string persistedOrderId = order.order_id;
  settlementOrders_.push_back(std::move(order));
  return persistedOrderId;
}

bool BillingService::suspend(const QString& batchId)
{
  if (!synchronizeBusinessDay()) return false;
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
  if (!synchronizeBusinessDay()) return false;
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
  if (!synchronizeBusinessDay()) return {};
  if (batchIds.isEmpty()) return {};

  std::vector<std::string> validBatchIds;
  validBatchIds.reserve(static_cast<std::size_t>(batchIds.size()));
  std::unordered_set<std::string> seenBatchIds;
  std::string recoveredOrderId;
  bool hasUnsettledBatch = false;
  for (const auto& qbid : batchIds)
  {
    const std::string id = qbid.toStdString();
    if (!seenBatchIds.insert(id).second) continue;
    auto it = batchIndex_.find(id);
    if (it == batchIndex_.end()) continue;
    if (!loadBatchJobs(id, false))
    {
      common::log_warn("settle", "Failed to load batch jobs before merge settlement: " + id);
      return {};
    }
    auto& batch = batches_[it->second];
    if (batch.is_settled() && !batch.settle_order_id.empty())
    {
      if (recoveredOrderId.empty()) recoveredOrderId = batch.settle_order_id;
      else if (recoveredOrderId != batch.settle_order_id) return {};
      continue;
    }
    if (!batch.is_suspended()) continue;
    hasUnsettledBatch = true;
    recomputeBatchTotals(batch);
    if (batch.pending_count > 0)
    {
      common::log_warn("settle", "Refused merge settlement with pending prices: " + id);
      return {};
    }
    validBatchIds.push_back(id);
  }

  // A repeated merge command over the same already-settled tasks is a
  // successful retry. Never combine an old merged order with new tasks.
  if (!recoveredOrderId.empty())
    return hasUnsettledBatch ? QString{} : QString::fromStdString(recoveredOrderId);
  if (validBatchIds.empty()) return {};

  const std::string orderId = counterStore_.nextOrderId(compactBoundDate());
  if (orderId.empty())
  {
    common::log_warn("settle", "Failed to allocate settlement order id for merge settlement.");
    return {};
  }
  const std::string now = utcNowIso8601();
  const auto previousBatches = batches_;
  const auto previousOrders = settlementOrders_;
  const std::string previousActiveBatchId = currentActiveBatchId_;

  for (const auto& id : validBatchIds)
  {
    auto it = batchIndex_.find(id);
    if (it == batchIndex_.end()) continue;
    auto& batch = batches_[it->second];

    batch.period_end_utc = now;
    batch.settle_order_id = orderId;
    batch.state = domain::BatchState::Settled;
    recomputeBatchTotals(batch);
  }

  if (!currentActiveBatchId_.empty() &&
      std::find(validBatchIds.begin(), validBatchIds.end(), currentActiveBatchId_) != validBatchIds.end())
  {
    currentActiveBatchId_.clear();
  }

  // Persist a single settlement order with all jobs from all batches.
  std::vector<std::uint32_t> allJobIds;
  std::vector<core::JobPricingEntry> allPricing;
  std::unordered_set<std::uint32_t> seenMergedJobs;
  for (const auto& id : validBatchIds)
  {
    auto it = batchIndex_.find(id);
    if (it == batchIndex_.end()) continue;
    for (auto jid : batches_[it->second].job_ids)
    {
      jid = canonicalJobId(jid);
      if (jid == 0 || !seenMergedJobs.insert(jid).second) continue;
      auto regIt = jobRegistry_.find(jid);
      if (regIt == jobRegistry_.end()) continue;
      allJobIds.push_back(jid);
      allPricing.push_back(regIt->second.pricing);
    }
  }

  const wpce::common::PricingConfig& pc = pricingConfig_ ? *pricingConfig_ : defaultPricing_;
  // Resolve user-facing display info from the first batch.
  std::string mergeDisplayName;
  const std::string firstBatchId = validBatchIds.front();
  const std::string lastBatchId = validBatchIds.back();
  const auto* fb = batchById(firstBatchId);
  if (fb != nullptr) mergeDisplayName = fb->display_name;
  core::SettlementOrder order = bridge::SettlementBridge::createSettlementOrder(
    orderId, firstBatchId, mergeDisplayName,
    allJobIds, allPricing, pc, std::string{}, now, "operator");
  order.batch_id = firstBatchId;
  order.task_id = firstBatchId;
  const auto* firstBatch = batchById(firstBatchId);
  const auto* lastBatch = batchById(lastBatchId);
  if (firstBatch != nullptr) order.first_document_name = firstBatch->first_doc_anchor.document_name;
  if (lastBatch != nullptr) order.last_document_name = lastBatch->last_doc_anchor.document_name;
  settlementOrders_.push_back(std::move(order));
  dirty_ = true;
  flushSave();
  if (dirty_)
  {
    batches_ = previousBatches;
    settlementOrders_ = previousOrders;
    currentActiveBatchId_ = previousActiveBatchId;
    common::log_warn("settle", "Daily order bucket write failed; merge settlement rolled back.");
    return {};
  }
  for (const auto& id : validBatchIds)
    emit batchUpdated(QString::fromStdString(id));
  emit batchesChanged();
  emit orderSettled(QString::fromStdString(orderId), QString::fromStdString(firstBatchId));
  return QString::fromStdString(orderId);
}

bool BillingService::renameBatch(const QString& batchId, const QString& newName)
{
  if (!synchronizeBusinessDay()) return false;
  const std::string id = batchId.toStdString();
  auto it = batchIndex_.find(id);
  if (it == batchIndex_.end()) return false;
  if (!loadBatchJobs(id, true)) return false;
  auto& batch = batches_[it->second];
  if (batch.is_settled()) return false;
  batch.display_name = newName.toStdString();
  scheduleSave();
  emit batchUpdated(batchId);
  return true;
}

bool BillingService::removeJobFromBatch(const QString& batchId, std::uint32_t jobId)
{
  if (!synchronizeBusinessDay()) return false;
  jobId = canonicalJobId(jobId);
  const std::string id = batchId.toStdString();
  auto it = batchIndex_.find(id);
  if (it == batchIndex_.end()) return false;
  auto& batch = batches_[it->second];
  if (batch.is_settled()) return false;

  const bool ownsCanonical =
    std::find(batch.job_ids.begin(), batch.job_ids.end(), jobId) != batch.job_ids.end();
  std::vector<std::uint32_t> removedIds;
  const auto oldSize = batch.job_ids.size();
  batch.job_ids.erase(
    std::remove_if(batch.job_ids.begin(), batch.job_ids.end(),
                   [this, jobId, &removedIds](std::uint32_t jid) {
                     if (canonicalJobId(jid) != jobId) return false;
                     removedIds.push_back(jid);
                     return true;
                   }),
    batch.job_ids.end());
  if (batch.job_ids.size() == oldSize) return false;

  // Remove from registry and reverse index - the job is now unassigned.
  for (const auto removedId : removedIds)
  {
    if (removedId == jobId) continue;
    jobRegistry_.erase(removedId);
    jobToBatchId_.erase(removedId);
    duplicateJobAliases_.erase(removedId);
  }

  const auto owner = jobToBatchId_.find(jobId);
  const bool indexedToThisBatch = owner != jobToBatchId_.end() && owner->second == id;
  if (ownsCanonical || indexedToThisBatch)
  {
    jobRegistry_.erase(jobId);
    forgetJobIndexes(jobId);
  }

  recomputeBatchTotals(batch);
  scheduleSave();
  emit batchUpdated(batchId);
  return true;
}

bool BillingService::moveJobToBatch(const QString& srcBatchId, std::uint32_t jobId,
                                     const QString& dstBatchId)
{
  if (!synchronizeBusinessDay()) return false;
  jobId = canonicalJobId(jobId);
  const std::string srcId = srcBatchId.toStdString();
  const std::string dstId = dstBatchId.toStdString();
  auto srcIt = batchIndex_.find(srcId);
  auto dstIt = batchIndex_.find(dstId);
  if (srcIt == batchIndex_.end() || dstIt == batchIndex_.end()) return false;
  if (!loadBatchJobs(srcId, true) || !loadBatchJobs(dstId, true)) return false;
  auto& src = batches_[srcIt->second];
  auto& dst = batches_[dstIt->second];
  if (src.is_settled() || dst.is_settled()) return false;

  (void)normalizeBatchJobIds(src);
  (void)normalizeBatchJobIds(dst);
  auto jidIt = std::find(src.job_ids.begin(), src.job_ids.end(), jobId);
  if (jidIt == src.job_ids.end()) return false;
  src.job_ids.erase(jidIt);
  if (std::find(dst.job_ids.begin(), dst.job_ids.end(), jobId) == dst.job_ids.end())
    dst.job_ids.push_back(jobId);
  jobToBatchId_[jobId] = dstId;

  recomputeBatchTotals(src);
  recomputeBatchTotals(dst);
  scheduleSave();
  emit batchUpdated(srcBatchId);
  emit batchUpdated(dstBatchId);
  return true;
}

void BillingService::setColorPages(std::uint32_t jobId, int colorPages)
{
  if (!synchronizeBusinessDay()) return;
  jobId = canonicalJobId(jobId);
  if (jobRegistry_.find(jobId) == jobRegistry_.end())
  {
    if (auto* existingBatch = batchContainingJob(jobId); existingBatch != nullptr)
      (void)loadBatchJobs(existingBatch->batch_id, true);
  }

  auto it = jobRegistry_.find(jobId);
  if (it == jobRegistry_.end()) return;

  if (const auto* owner = batchContainingJob(jobId); owner && owner->is_settled()) return;
  if (colorPages < 0) colorPages = 0;

  // Clamp to total_pages (color pages can't exceed total pages).
  const int totalPages = common::parse_total_pages(it->second.metadata);
  if (colorPages > totalPages) colorPages = totalPages;

  it->second.pricing.color_pages = colorPages;

  // Recompute pricing with the new color_pages value.
  const common::PricingConfig& pc = pricingConfig_ ? *pricingConfig_ : defaultPricing_;
  const common::PrinterProfileConfig& pp = printerProfiles_ ? *printerProfiles_ : defaultPrinters_;
  const auto previousPricing = it->second.pricing;
  it->second.pricing = common::compute_job_pricing(it->second.metadata, pc, pp, colorPages);
  preserve_manual_price(it->second.pricing, previousPricing);
  it->second.last_updated_utc = utcNowIso8601();

  // Find the owning batch and recompute its totals.
  if (auto* batch = batchContainingJob(jobId); batch != nullptr)
  {
    recomputeBatchTotals(*batch);
    emit batchUpdated(QString::fromStdString(batch->batch_id));
    scheduleSave();
  }
}

bool BillingService::setManualPrice(std::uint32_t jobId, double totalPrice)
{
  if (!synchronizeBusinessDay()) return false;
  jobId = canonicalJobId(jobId);
  if (!std::isfinite(totalPrice) || totalPrice < 0.0) return false;
  auto it = jobRegistry_.find(jobId);
  if (it == jobRegistry_.end()) return false;
  auto* batch = batchContainingJob(jobId);
  if (batch == nullptr || batch->is_settled()) return false;

  it->second.pricing.manual_price_override = true;
  it->second.pricing.manual_total_price = totalPrice;
  it->second.pricing.total_price = totalPrice;
  it->second.pricing.price_pending = false;
  it->second.pricing.pending_reason.clear();
  it->second.pricing.note = "manual_price_override";
  it->second.last_updated_utc = utcNowIso8601();
  recomputeBatchTotals(*batch);
  scheduleSave();
  emit batchUpdated(QString::fromStdString(batch->batch_id));
  return true;
}

void BillingService::repriceUnsettledJobs()
{
  if (!synchronizeBusinessDay()) return;
  const common::PricingConfig& pc = pricingConfig_ ? *pricingConfig_ : defaultPricing_;
  const common::PrinterProfileConfig& pp = printerProfiles_ ? *printerProfiles_ : defaultPrinters_;
  bool changed = false;
  for (auto& batch : batches_)
  {
    if (batch.is_settled()) continue;
    for (auto jid : batch.job_ids)
    {
      auto job = jobRegistry_.find(canonicalJobId(jid));
      if (job == jobRegistry_.end()) continue;
      const auto previous = job->second.pricing;
      job->second.pricing = common::compute_job_pricing(
          job->second.metadata, pc, pp, previous.color_pages);
      preserve_manual_price(job->second.pricing, previous);
      changed = true;
    }
    recomputeBatchTotals(batch);
    emit batchUpdated(QString::fromStdString(batch.batch_id));
  }
  if (changed) scheduleSave();
}

bool BillingService::deleteJob(std::uint32_t jobId)
{
  if (!synchronizeBusinessDay()) return false;
  jobId = canonicalJobId(jobId);
  if (jobRegistry_.find(jobId) == jobRegistry_.end())
  {
    if (auto* existingBatch = batchContainingJob(jobId); existingBatch != nullptr)
      (void)loadBatchJobs(existingBatch->batch_id, true);
  }

  auto it = jobRegistry_.find(jobId);
  if (it == jobRegistry_.end()) return false;

  if (const auto* owner = batchContainingJob(jobId); owner && owner->is_settled()) return false;
  // Remove from the owning batch.
  if (auto* batch = batchContainingJob(jobId); batch != nullptr && !batch->is_settled())
  {
    batch->job_ids.erase(
      std::remove_if(batch->job_ids.begin(), batch->job_ids.end(),
                     [this, jobId](std::uint32_t jid) {
                       return canonicalJobId(jid) == jobId;
                     }),
      batch->job_ids.end());
    recomputeBatchTotals(*batch);
    emit batchUpdated(QString::fromStdString(batch->batch_id));
  }
  forgetJobIndexes(jobId);
  jobRegistry_.erase(it);
  scheduleSave();
  return true;
}


}  // namespace wpce::ui
