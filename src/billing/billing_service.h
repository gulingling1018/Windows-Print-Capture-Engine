/// @file billing_service.h
/// Single source of truth for billing state.
///
/// BillingService sits between the WatcherBridge (print job events)
/// and the UI models.  It owns:
///   - the list of BillingBatch entities (active, suspended, settled)
///   - a registry of PrintJobRecord (one per observed job_id)
///   - a CounterStore for global unique IDs
///   - references to the PricingConfig and PrinterProfileConfig
///
/// State transitions are triggered by operator actions:
///   settle(batchId)      - Active -> Settled; currentActive cleared
///   suspend(batchId)     - Active -> Suspended; currentActive cleared
///   resume(batchId)      - Suspended -> Active; becomes currentActive
///   mergeSettle(batchIds)- multiple Suspended -> Settled under one order
///
/// New jobs from WatcherBridge are routed to currentActiveBatch_;
/// if none exists, a new Active batch is created automatically.
///
/// The service exposes a QAbstractItemModel (BillingBatchModel) for
/// the UI to render the batch tree.  UI never mutates batches
/// directly - it calls BillingService methods.

#pragma once

#include "common/app_config.h"
#include "common/counter_store.h"
#include "common/business_clock.h"
#include "common/pricing_engine.h"
#include "core/data_contracts.h"
#include "domain/billing_batch.h"
#include "domain/billing_batch_repository.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <deque>
#include <variant>

namespace wpce::ui
{

class BillingBatchModel;

class BillingService : public QObject
{
  Q_OBJECT

 public:
  explicit BillingService(QObject* parent = nullptr);
  ~BillingService() override;

  /// Configure paths for state files.  Must be called before start.
  ///   stateDir = <output_root>/_state
  void setStateDirectory(const QString& stateDir);

  /// Set the authoritative daily order bucket path:
  /// <storage-root>/YYYY-MM-DD/orders.json.
  void setOrderBucketPath(const QString& orderBucketPath);
  void setBusinessDateProvider(common::BusinessDayCoordinator::DateProvider provider);
  bool synchronizeBusinessDay();
  [[nodiscard]] bool storageReady() const noexcept { return storage_ready_; }
  [[nodiscard]] bool hasPendingSave() const noexcept { return dirty_; }
  [[nodiscard]] const QString& storageErrorMessage() const { return storage_error_; }
  [[nodiscard]] QString boundBusinessDate() const { return QString::fromStdString(business_day_.boundDate()); }

  /// Configure pricing & printer profiles (live references; the caller
  /// must keep them alive for the lifetime of the service).
  void setPricingConfig(const wpce::common::PricingConfig* pricing);
  void setPrinterProfiles(const wpce::common::PrinterProfileConfig* printers);

  /// Load persisted state (counters + batches).  Called on session start.
  void load();

  /// Flush pending saves (called on stop / shutdown).
  void flushSave();
  struct ShutdownResult
  {
    bool ready{false};
    std::size_t deferred_events{0};
    QString message;
  };
  // Call after producers stop and their queued callbacks have been delivered.
  [[nodiscard]] ShutdownResult prepareShutdown();
  [[nodiscard]] bool exportRecovery(const QString& path, QString* error = nullptr,
      const QStringList& captureErrors = {}) const;
  [[nodiscard]] std::size_t deferredEventCount() const noexcept { return deferred_events_.size(); }

  /// Clear all in-memory state and persisted state files.  Called when
  /// the operator starts a fresh monitoring session.
  void clear();

  // Query
  [[nodiscard]] const std::vector<domain::BillingBatch>& batches() const { return batches_; }
  [[nodiscard]] const domain::BillingBatch* batchById(const std::string& batchId) const;
  [[nodiscard]] domain::BillingBatch* batchById(const std::string& batchId);
  [[nodiscard]] const domain::PrintJobRecord* jobRecord(std::uint32_t jobId) const;
  [[nodiscard]] bool areBatchJobsLoaded(const QString& batchId) const;
  bool ensureBatchJobsLoaded(const QString& batchId);
  bool batchMatchesSearch(const QString& batchId, const QString& query);

  [[nodiscard]] int activeBatchCount() const;
  [[nodiscard]] int suspendedBatchCount() const;
  [[nodiscard]] int settledBatchCount() const;
  [[nodiscard]] int todayJobCount() const;
  [[nodiscard]] double todayRevenue() const;

  /// Highest job ID stored across all loaded batches.  Used to seed the
  /// watcher bridge's synthetic-ID counter so restarted sessions never
  /// collide with previously persisted IDs.
  [[nodiscard]] std::uint32_t maxStoredJobId() const;

  /// The batch that will receive the next incoming job.  May be null.
  [[nodiscard]] const domain::BillingBatch* currentActiveBatch() const;


  // Operator actions
  /// Settle a batch: freeze its jobs into a SettlementOrder, mark
  /// Settled, clear currentActive.  Returns the generated order id,
  /// or empty string on failure.
  QString settle(const QString& batchId);

  /// Suspend an active batch (hang up).  Clears currentActive so the
  /// next job opens a new batch.
  bool suspend(const QString& batchId);

  /// Resume a suspended batch.  Makes it the currentActive.  If another
  /// batch is currently active, that one is automatically suspended.
  bool resume(const QString& batchId);

  /// Merge-settle multiple suspended batches into one SettlementOrder.
  /// All batches are marked Settled with the same order id.
  QString mergeSettle(const QStringList& batchIds);

  /// Rename a batch's display name.
  bool renameBatch(const QString& batchId, const QString& newName);

  /// Remove a job from a batch (operator correction).  The job becomes
  /// unassigned.  Only allowed before settlement.
  bool removeJobFromBatch(const QString& batchId, std::uint32_t jobId);

  /// Move a job from one batch to another (operator correction).
  /// Only allowed before settlement on both batches.
  bool moveJobToBatch(const QString& srcBatchId, std::uint32_t jobId,
                       const QString& dstBatchId);

  /// Set the operator-filled color page count for a job, then recompute
  /// pricing and batch totals.  Emits batchUpdated on success.
  void setColorPages(std::uint32_t jobId, int colorPages);

  /// Apply an operator-approved total price to a job. The override is stored
  /// in the same source used by every billing window and frozen at settlement.
  bool setManualPrice(std::uint32_t jobId, double totalPrice);

  /// Recompute every unsettled job after pricing/profile configuration changes.
  void repriceUnsettledJobs();

  /// Delete a job entirely from the registry (operator correction).
  bool deleteJob(std::uint32_t jobId);

  // WatcherBridge signal handlers
 public slots:
  void onJobSnapshot(core::JobSnapshot snapshot);
  void onMetadata(core::MetadataSnapshot metadata);
  void onLifecycleEvent(core::JobLifecycleEvent event);

 signals:
  void storageError(QString message);
  void businessDayChanged(QString date);
  /// Emitted when the batch list structure changes (insert/remove/state).
  void batchesChanged();
  /// Emitted when a batch's aggregate values change.
  void batchUpdated(const QString& batchId);
  /// Emitted when a settlement order is generated.
  void orderSettled(const QString& orderId, const QString& batchId);
  /// Emitted when a new batch is created.
  void batchCreated(const QString& batchId);

 private:
  /// Find or create the currentActiveBatch.  Creates a new batch if
  /// none exists, using the source_user from the snapshot.
  domain::BillingBatch* ensureActiveBatch(const core::JobSnapshot& snapshot);

  /// Create a new Active batch with a fresh id from CounterStore.
  domain::BillingBatch* createNewActiveBatch(const std::string& sourceUser);

  /// Recompute aggregate totals for a batch from its job records.
  void recomputeBatchTotals(domain::BillingBatch& batch);

  /// Collapse duplicate synthetic job ids in a batch before display,
  /// persistence, total calculation, and settlement.
  bool normalizeBatchJobIds(domain::BillingBatch& batch);

  /// Find the batch that currently owns a job id.
  domain::BillingBatch* batchContainingJob(std::uint32_t jobId);

  /// Resolve duplicate synthetic IDs back to their first observed physical job.
  [[nodiscard]] std::uint32_t canonicalJobId(std::uint32_t jobId) const;
  [[nodiscard]] std::uint32_t findDuplicateJobForSnapshot(
      const core::JobSnapshot& snapshot,
      std::uint32_t incomingJobId) const;
  void registerJobFingerprint(const domain::PrintJobRecord& record);
  void forgetJobIndexes(std::uint32_t jobId);
  void mergeDuplicateJob(std::uint32_t duplicateJobId,
                         std::uint32_t canonicalJobId,
                         core::JobSnapshot snapshot);

  bool loadBatchJobs(const std::string& batchId, bool emitSignals);
  void preloadRecentBatchJobs(std::size_t batchCount);
  bool ensureAllBatchJobsLoaded();

  /// Update the first/last doc anchor for a batch based on its jobs.
  void updateAnchors(domain::BillingBatch& batch);

  /// Schedule a debounced save.
  void scheduleSave();
  bool replayDeferredEvents();

  /// Build a SettlementOrder and persist it via SettlementBridge.
  std::string settleBatchInternal(domain::BillingBatch& batch,
                                   const std::string& orderId);

  /// Get today's date as YYYY-MM-DD (local time).
  std::string compactBoundDate() const;
  void reportStorageError(const std::string& message);
  void adoptLoaded(std::vector<domain::BillingBatch> tasks,
      std::unordered_map<std::uint32_t, domain::PrintJobRecord> jobs,
      std::vector<core::SettlementOrder> orders);
  bool prepareCounters(const std::vector<domain::BillingBatch>& tasks,
      const std::vector<core::SettlementOrder>& orders, const std::string& date);

  std::vector<domain::BillingBatch> batches_;
  std::vector<core::SettlementOrder> settlementOrders_;
  std::unordered_map<std::uint32_t, domain::PrintJobRecord> jobRegistry_;
  std::unordered_set<std::string> jobLoadedBatchIds_;
  std::string currentActiveBatchId_;

  // Batch lookup index (rebuilt on load / structural change).
  std::unordered_map<std::string, std::size_t> batchIndex_;
  // Reverse index: job_id -> batch_id for O(1) lookup.
  std::unordered_map<std::uint32_t, std::string> jobToBatchId_;
  std::unordered_map<std::string, std::uint32_t> jobFingerprintToJobId_;
  std::unordered_map<std::uint32_t, std::uint32_t> duplicateJobAliases_;
  std::unordered_set<std::uint32_t> ignoredPlaceholderJobIds_;

  const wpce::common::PricingConfig* pricingConfig_{nullptr};
  const wpce::common::PrinterProfileConfig* printerProfiles_{nullptr};
  wpce::common::PricingConfig defaultPricing_{};
  wpce::common::PrinterProfileConfig defaultPrinters_{};

  wpce::common::CounterStore counterStore_;
  domain::BillingBatchRepository repository_;
  QString stateDir_;
  QString orderBucketPath_;
  common::BusinessDayCoordinator business_day_;
  bool storage_ready_{false};
  bool switching_day_{false};
  QString storage_error_;
  std::unordered_set<std::uint32_t> previous_day_job_ids_;
  std::unordered_map<std::uint32_t, std::string> previous_day_fingerprints_;
  std::string processing_date_;
  struct DeferredEvent
  {
    std::string date;
    std::variant<core::JobSnapshot, core::MetadataSnapshot, core::JobLifecycleEvent> payload;
  };
  std::deque<DeferredEvent> deferred_events_;

  QTimer saveTimer_;
  QTimer periodicSaveTimer_;
  bool dirty_{false};
};

}  // namespace wpce::ui
