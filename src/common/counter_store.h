/// @file counter_store.h
/// Global, persistent, monotonic counters for billing IDs.
///
/// Replaces the per-session, per-directory ID generation that caused
/// duplicate order/batch IDs across capture sessions.  Counters are
/// persisted to `<output_root>/_state/counters.json`, decoupled from
/// the timestamped capture subdirectories, so they survive restarts
/// and remain unique across the lifetime of the data root.
///
/// Format:
///   B-YYYYMMDD-NNNNNN      (batch id, 6-digit zero-padded)
///   SETTLE-YYYYMMDD-NNNNNN (settlement order id, 6-digit zero-padded)
///
/// The counter file is written atomically (temp + rename) and is safe
/// for single-process access (the GUI is single-process).

#pragma once

#include <filesystem>
#include <mutex>
#include <string>

namespace wpce::common
{

class CounterStore
{
 public:
  CounterStore() = default;

  /// Set the on-disk state file path.  Must be called before any
  /// next*() call.  Loads existing values from the file if present.
  void setStatePath(const std::filesystem::path& path);

  /// Force-load values from the state file.  Returns true on success
  /// (or when the file does not exist yet - counters stay at 0).
  bool load();

  /// Force-save current values to the state file (atomic).
  /// Returns true on success.
  bool save() const;
  [[nodiscard]] bool healthy() const;
  bool ensureMinimum(const std::string& date, long batch_counter, long order_counter);

  /// Atomically increment and return the next batch id.
  std::string nextBatchId(const std::string& business_date = {});

  /// Atomically increment and return the next settlement order id.
  std::string nextOrderId(const std::string& business_date = {});

  /// Current system-local date in YYYYMMDD form, used for business id prefixes.
  static std::string currentDateYyyymmdd();

  /// Backward-compatible alias for currentDateYyyymmdd().
  static std::string currentDateUtcYyyymmdd();

 private:
  struct Counters
  {
    std::string last_date_yyyymmdd;
    long batch_counter{0};     // per-day counter
    long order_counter{0};     // per-day counter
  };

  mutable std::mutex mutex_;
  std::filesystem::path path_;
  Counters counters_{};
  bool loaded_ok_{true};

  /// Roll per-day counters to zero when the date changes.
  void rollDateIfNeeded(const std::string& today);

  /// Persist counters while mutex_ is already held by the caller.
  bool saveLocked() const;
};

}  // namespace wpce::common
