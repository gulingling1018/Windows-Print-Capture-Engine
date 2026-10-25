/// @file billing_batch_repository.h
/// Persistence for the authoritative daily billing bucket.
///
/// The repository serialises tasks, complete job records and immutable
/// settlement orders to `<storage-root>/YYYY-MM-DD/orders.json`.
/// Legacy split state files remain readable for migration only.
///
/// Atomic write (temp + rename) is used.  The repository is pure-data
/// (no Qt) so it can be reused by tests.

#pragma once

#include "domain/billing_batch.h"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace wpce::domain
{

enum class BucketLoadStatus { Missing, Loaded, Recovered, Corrupt, AccessDenied, UnsupportedSchema };
struct BucketLoadResult
{
  BucketLoadStatus status{BucketLoadStatus::Corrupt};
  std::string message;
  [[nodiscard]] bool ok() const noexcept
  { return status == BucketLoadStatus::Missing || status == BucketLoadStatus::Loaded || status == BucketLoadStatus::Recovered; }
};

class BillingBatchRepository
{
 public:
  /// Set the on-disk file path.  Required before save/load.
  void setFilePath(const std::filesystem::path& path);
  [[nodiscard]] std::filesystem::path filePath() const { return path_; }

  /// Load all batches from the file.  Returns true on success (or when
  /// the file does not exist - empty batch list is valid).
  /// Also populates saved_date with the date recorded in the file.
  bool load(std::vector<BillingBatch>& out);
  bool load(std::vector<BillingBatch>& out, std::string& saved_date);

  /// Load batch summaries from either a legacy split state or a v4 bucket.
  bool loadSummaries(std::vector<BillingBatch>& out, std::string& saved_date);

  /// Load batches plus per-batch job records (restores full registry).
  bool load(std::vector<BillingBatch>& out,
            std::unordered_map<std::uint32_t, PrintJobRecord>& jobs,
            std::string& saved_date);

  /// Load the complete daily bucket: tasks, job records, and immutable
  /// settlement orders.
  bool load(std::vector<BillingBatch>& out,
            std::unordered_map<std::uint32_t, PrintJobRecord>& jobs,
            std::vector<core::SettlementOrder>& orders,
            std::string& saved_date);

  /// Load only settlement orders from the configured daily bucket.
  bool loadOrders(std::vector<core::SettlementOrder>& orders);

  /// Strict transactional read. A readable backup may be restored after
  /// preserving damaged bytes; unknown future schemas are never downgraded.
  BucketLoadResult loadDaily(std::vector<BillingBatch>& batches,
      std::unordered_map<std::uint32_t, PrintJobRecord>& jobs,
      std::vector<core::SettlementOrder>& orders, std::string& saved_date);
  [[nodiscard]] const std::string& lastError() const noexcept { return last_error_; }

  /// Load job records for selected tasks. The output map is cleared and
  /// contains only records from the requested task ids.
  bool loadJobsForBatches(const std::vector<std::string>& batch_ids,
                          std::unordered_map<std::uint32_t, PrintJobRecord>& jobs);

  /// Save all batches to the file (atomic).
  /// Records today's date (YYYY-MM-DD) for cross-day detection.
  bool save(const std::vector<BillingBatch>& batches) const;
  bool save(const std::vector<BillingBatch>& batches,
            const std::string& date_yyyy_mm_dd) const;

  /// Save batches plus per-batch job records (full registry persistence).
  bool save(const std::vector<BillingBatch>& batches,
            const std::unordered_map<std::uint32_t, PrintJobRecord>& jobs,
            const std::string& date_yyyy_mm_dd) const;

  /// Save the authoritative daily bucket. Orders are deduplicated by both
  /// order_id and task_id/batch_id before the atomic replacement.
  bool save(const std::vector<BillingBatch>& batches,
            const std::unordered_map<std::uint32_t, PrintJobRecord>& jobs,
            const std::vector<core::SettlementOrder>& orders,
            const std::string& date_yyyy_mm_dd) const;

 private:
  std::filesystem::path path_;
  mutable std::string last_error_;
};

}  // namespace wpce::domain
