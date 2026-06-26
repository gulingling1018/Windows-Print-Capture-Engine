/// @file billing_batch_repository.h
/// Persistence for BillingBatch entities.
///
/// The repository serialises the full batch list (Active, Suspended,
/// Settled) to a single JSON file at `<output_root>/_state/today_active.json`.
/// This is decoupled from the timestamped capture subdirectories so
/// that batches survive across capture sessions and remain consistent
/// with the global CounterStore.
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

class BillingBatchRepository
{
 public:
  /// Set the on-disk file path.  Required before save/load.
  void setFilePath(const std::filesystem::path& path);
  [[nodiscard]] std::filesystem::path filePath() const { return path_; }

  /// Load all batches from the file.  Returns true on success (or when
  /// the file does not exist — empty batch list is valid).
  /// Also populates saved_date with the date recorded in the file.
  bool load(std::vector<BillingBatch>& out);
  bool load(std::vector<BillingBatch>& out, std::string& saved_date);

  /// Load batches plus per-batch job records (restores full registry).
  bool load(std::vector<BillingBatch>& out,
            std::unordered_map<std::uint32_t, PrintJobRecord>& jobs,
            std::string& saved_date);

  /// Save all batches to the file (atomic).
  /// Records today's date (YYYY-MM-DD) for cross-day detection.
  bool save(const std::vector<BillingBatch>& batches) const;
  bool save(const std::vector<BillingBatch>& batches,
            const std::string& date_yyyy_mm_dd) const;

  /// Save batches plus per-batch job records (full registry persistence).
  bool save(const std::vector<BillingBatch>& batches,
            const std::unordered_map<std::uint32_t, PrintJobRecord>& jobs,
            const std::string& date_yyyy_mm_dd) const;

 private:
  std::filesystem::path path_;
};

}  // namespace wpce::domain
