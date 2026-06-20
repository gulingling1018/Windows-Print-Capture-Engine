#pragma once

#include "core/data_contracts.h"
#include "core/perf_monitor.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wpce::watcher
{

struct WatcherConfig
{
  std::wstring printer_name;

  /// Timeout for WaitForSingleObject on the change notification handle.
  std::chrono::milliseconds wait_timeout{250};

  /// Max time a job may remain in spooling or printing before it is
  /// reported as stalled.  Zero disables stall detection.
  std::chrono::seconds stall_timeout{0};
};

using JobSnapshotCallback = std::function<void(const core::JobSnapshot&)>;
using MetadataSnapshotCallback = std::function<void(const core::MetadataSnapshot&)>;
using LifecycleEventCallback = std::function<void(const core::JobLifecycleEvent&)>;

/// A job snapshot paired with its extracted metadata.
struct JobSnapshotWithMetadata
{
  core::JobSnapshot job;
  core::MetadataSnapshot metadata;
};

// ── Public helpers ─────────────────────────────────────────────────────

std::string describe_job_status(std::uint32_t status_flags);

/// Map raw JOB_STATUS flags to a high-level lifecycle phase.
core::JobLifecyclePhase classify_phase(std::uint32_t status_flags,
                                       const std::string& status_string) noexcept;

/// Enumerate current print jobs with both job-level and metadata snapshots.
std::vector<JobSnapshotWithMetadata> enumerate_printer_jobs(const std::wstring& printer_name);

// ── PrintWatcher ────────────────────────────────────────────────────────

class PrintWatcher
{
 public:
  PrintWatcher(WatcherConfig config,
               JobSnapshotCallback job_callback,
               MetadataSnapshotCallback metadata_callback = {},
               LifecycleEventCallback lifecycle_callback = {});
  ~PrintWatcher();

  PrintWatcher(const PrintWatcher&) = delete;
  PrintWatcher& operator=(const PrintWatcher&) = delete;
  PrintWatcher(PrintWatcher&&) = delete;
  PrintWatcher& operator=(PrintWatcher&&) = delete;

  void start();
  void stop() noexcept;

  [[nodiscard]] bool running() const noexcept;
  [[nodiscard]] std::string last_error() const;

  /// Thread-safe copy of the current per-job lifecycle summaries.
  [[nodiscard]] std::vector<core::JobLifecycleSummary> lifecycle_summaries() const;

  /// Access the performance monitor for sampling.
  [[nodiscard]] const core::PerfMonitor& perf_monitor() const noexcept { return perf_monitor_; }
  [[nodiscard]] core::PerfMonitor& perf_monitor() noexcept { return perf_monitor_; }

 private:
  void run();
  void set_last_error(std::string error);
  void tick_stall_detection();

  WatcherConfig config_;
  JobSnapshotCallback job_callback_;
  MetadataSnapshotCallback metadata_callback_;
  LifecycleEventCallback lifecycle_callback_;

  std::atomic_bool stop_requested_{false};
  std::atomic_bool running_{false};
  std::thread worker_;

  // Lifecycle state (only touched on watcher thread, read by summaries()).
  struct InternalJobState
  {
    core::JobLifecyclePhase current_phase{core::JobLifecyclePhase::Unknown};
    std::uint32_t status_flags{0};
    int next_sequence{0};
    std::chrono::steady_clock::time_point last_progress_time;

    // Phase timing (all steady_clock).
    std::chrono::steady_clock::time_point first_event_time;    // When first event was emitted
    std::chrono::steady_clock::time_point phase_entry_time;   // When current phase started
    std::chrono::steady_clock::time_point last_event_time;     // When last event was emitted
    std::int64_t cumulative_phase_ms{0};                       // Accumulated ms in current phase

    core::JobLifecycleSummary summary;
  };
  mutable std::mutex summaries_mutex_;
  std::unordered_map<std::uint32_t, InternalJobState> job_states_;

  core::PerfMonitor perf_monitor_;

  mutable std::mutex lifecycle_mutex_;
  mutable std::mutex error_mutex_;
  std::string last_error_;
};

}  // namespace wpce::watcher
