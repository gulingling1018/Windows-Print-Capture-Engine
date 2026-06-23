/// @file billing_batch_repository.h
/// Persistence for BillingBatch entities.
///
/// The repository serialises the full batch list (Active, Suspended,
/// Settled) to a single JSON file at `<output_root>/_state/batches.json`.
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
  bool load(std::vector<BillingBatch>& out);

  /// Save all batches to the file (atomic).
  bool save(const std::vector<BillingBatch>& batches) const;

 private:
  std::filesystem::path path_;
};

}  // namespace wpce::domain
