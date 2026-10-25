#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace wpce::core
{

/// A single performance sample capturing resource usage at a point in time.
struct PerfSample
{
  /// Wall-clock timestamp of this sample.
  std::string timestamp_utc;

  /// CPU usage as percentage of one logical processor (0.0-100.0 * cores).
  double cpu_percent{0.0};

  /// Working set (physical memory) in bytes.
  std::uint64_t working_set_bytes{0};

  /// Number of open handles owned by the process.
  std::uint32_t handle_count{0};

  /// Number of threads in the process.
  std::uint32_t thread_count{0};

  // WPCE-specific metrics
  /// Depth of the job snapshot writer queue at sample time.
  std::size_t snapshot_writer_queue_depth{0};

  /// Depth of the page artifact writer queue at sample time.
  std::size_t page_writer_queue_depth{0};

  /// Milliseconds since the watcher last received a notification.
  /// -1 if never received.
  std::int64_t watcher_idle_ms{-1};

  /// Milliseconds spent in the most recent event-processing tick.
  double last_event_processing_ms{0.0};
};

/// Lightweight performance monitor using Windows process APIs.
///
/// Thread-safe for sampling from any thread. All system calls are made
/// from the calling thread (no internal worker thread).
class PerfMonitor
{
 public:
  PerfMonitor();
  ~PerfMonitor();

  PerfMonitor(const PerfMonitor&) = delete;
  PerfMonitor& operator=(const PerfMonitor&) = delete;

  /// Take a sample of current process resource usage.
  [[nodiscard]] PerfSample sample() const;

  // Setters for WPCE-specific metrics (lock-free)
  void set_snapshot_writer_queue_depth(std::size_t depth) noexcept;
  void set_page_writer_queue_depth(std::size_t depth) noexcept;
  void set_last_notification_time(std::chrono::steady_clock::time_point t) noexcept;
  void set_last_event_processing_ms(double ms) noexcept;

 private:
#ifdef _WIN32
  void* process_handle_{nullptr};
  std::uint32_t processor_count_{1};

  // Previous sample for CPU delta calculation.
  mutable std::mutex prev_mutex_;
  mutable std::uint64_t prev_cpu_time_{0};
  mutable std::chrono::steady_clock::time_point prev_sample_time_;
#endif

  // Atomic counters for lock-free writers.
  std::atomic_size_t snapshot_writer_queue_depth_{0};
  std::atomic_size_t page_writer_queue_depth_{0};

  mutable std::mutex notif_mutex_;
  std::chrono::steady_clock::time_point last_notification_time_{};

  std::atomic<double> last_event_processing_ms_{-1.0};
};

}  // namespace wpce::core
