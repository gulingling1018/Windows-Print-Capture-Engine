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
///   settle(batchId)      — Active → Settled; currentActive cleared
///   suspend(batchId)     — Active → Suspended; currentActive cleared
///   resume(batchId)      — Suspended → Active; becomes currentActive
///   mergeSettle(batchIds)— multiple Suspended → Settled under one order
///
/// New jobs from WatcherBridge are routed to currentActiveBatch_;
/// if none exists, a new Active batch is created automatically.
///
/// The service exposes a QAbstractItemModel (BillingBatchModel) for
/// the UI to render the batch tree.  UI never mutates batches
/// directly — it calls BillingService methods.

#pragma once

#include "common/app_config.h"
#include "common/counter_store.h"
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
#include <vector>

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

  /// Configure pricing & printer profiles (live references; the caller
  /// must keep them alive for the lifetime of the service).
  void setPricingConfig(const wpce::common::PricingConfig* pricing);
  void setPrinterProfiles(const wpce::common::PrinterProfileConfig* printers);

  /// Load persisted state (counters + batches).  Called on session start.
  void load();

  /// Flush pending saves (called on stop / shutdown).
  void flushSave();

  /// Clear all in-memory state and persisted state files.  Called when
  /// the operator starts a fresh monitoring session.
  void clear();

  // ── Query ───────────────────────────────────────────────────────────

  [[nodiscard]] const std::vector<domain::BillingBatch>& batches() const { return batches_; }
  [[nodiscard]] const domain::BillingBatch* batchById(const std::string& batchId) const;
  [[nodiscard]] domain::BillingBatch* batchById(const std::string& batchId);
  [[nodiscard]] const domain::PrintJobRecord* jobRecord(std::uint32_t jobId) const;

  [[nodiscard]] int activeBatchCount() const;
  [[nodiscard]] int suspendedBatchCount() const;
  [[nodiscard]] int settledBatchCount() const;
  [[nodiscard]] int todayJobCount() const;
  [[nodiscard]] double todayRevenue() const;

  /// The batch that will receive the next incoming job.  May be null.
  [[nodiscard]] const domain::BillingBatch* currentActiveBatch() const;

  // ── Operator actions ────────────────────────────────────────────────

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

  /// Delete a job entirely from the registry (operator correction).
  bool deleteJob(std::uint32_t jobId);

  // ── WatcherBridge signal handlers ───────────────────────────────────

 public slots:
  void onJobSnapshot(core::JobSnapshot snapshot);
  void onMetadata(core::MetadataSnapshot metadata);
  void onLifecycleEvent(core::JobLifecycleEvent event);

 signals:
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
  domain::BillingBatch& createNewActiveBatch(const std::string& sourceUser);

  /// Recompute aggregate totals for a batch from its job records.
  void recomputeBatchTotals(domain::BillingBatch& batch);

  /// Update the first/last doc anchor for a batch based on its jobs.
  void updateAnchors(domain::BillingBatch& batch);

  /// Schedule a debounced save.
  void scheduleSave();

  /// Build a SettlementOrder and persist it via SettlementBridge.
  std::string settleBatchInternal(domain::BillingBatch& batch,
                                   const std::string& orderId);

  std::vector<domain::BillingBatch> batches_;
  std::unordered_map<std::uint32_t, domain::PrintJobRecord> jobRegistry_;
  std::string currentActiveBatchId_;

  // Batch lookup index (rebuilt on load / structural change).
  std::unordered_map<std::string, std::size_t> batchIndex_;

  const wpce::common::PricingConfig* pricingConfig_{nullptr};
  const wpce::common::PrinterProfileConfig* printerProfiles_{nullptr};
  wpce::common::PricingConfig defaultPricing_{};
  wpce::common::PrinterProfileConfig defaultPrinters_{};

  wpce::common::CounterStore counterStore_;
  domain::BillingBatchRepository repository_;
  QString stateDir_;

  QTimer saveTimer_;
  bool dirty_{false};
};

}  // namespace wpce::ui
