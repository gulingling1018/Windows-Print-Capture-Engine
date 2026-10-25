/// @file logger.h
/// Thread-safe ring-buffer logger for structured diagnostics.
///
/// All watcher / bridge / filter diagnostic messages flow through
/// the RingBufferLogger singleton so the UI (or a debug dump) can
/// inspect recent log entries without attaching a debugger.
///
/// Usage:
///   common::log_info("bridge", "Printer X opened");
///   auto recent = common::RingBufferLogger::instance().dump_recent(100);

#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace wpce::common
{

enum class LogSeverity
{
  Debug,
  Info,
  Warn,
  Error,
};

struct LogEntry
{
  /// Nanoseconds since steady_clock epoch - monotonic, suitable for
  /// ordering and delta calculation.  Not convertible to wall-clock
  /// directly; use the timestamp recorded at process start for that.
  std::chrono::nanoseconds timestamp;
  LogSeverity severity;
  std::string category;   // e.g. "init", "notify", "phase", "bridge", "ipc"
  std::string message;
};

class RingBufferLogger
{
 public:
  static RingBufferLogger& instance();

  /// Log a message.  Thread-safe, lock-free after the initial allocation
  /// (the ring buffer is pre-sized; push_back may reallocate once).
  void log(LogSeverity severity,
           std::string_view category,
           std::string message);

  /// Return the most recent `count` entries, newest first.
  [[nodiscard]] std::vector<LogEntry> dump_recent(std::size_t count) const;

  /// Return all entries in the ring buffer, newest first.
  [[nodiscard]] std::vector<LogEntry> dump_all() const;

  /// Current number of stored entries.
  [[nodiscard]] std::size_t size() const;

  /// Clear all entries.
  void clear();

  /// Resize the ring buffer.  If the new capacity is smaller than the
  /// current number of entries, the oldest entries are dropped.
  void set_capacity(std::size_t capacity);

  /// Human-readable severity label.
  static std::string_view to_string(LogSeverity sev);

 private:
  RingBufferLogger() = default;
  ~RingBufferLogger() = default;
  RingBufferLogger(const RingBufferLogger&) = delete;
  RingBufferLogger& operator=(const RingBufferLogger&) = delete;

  mutable std::mutex mutex_;
  std::vector<LogEntry> buffer_;
  std::size_t capacity_{2000};
};

// Convenience free functions
inline void log_debug(std::string_view category, std::string message)
{
  RingBufferLogger::instance().log(LogSeverity::Debug, category, std::move(message));
}

inline void log_info(std::string_view category, std::string message)
{
  RingBufferLogger::instance().log(LogSeverity::Info, category, std::move(message));
}

inline void log_warn(std::string_view category, std::string message)
{
  RingBufferLogger::instance().log(LogSeverity::Warn, category, std::move(message));
}

inline void log_error(std::string_view category, std::string message)
{
  RingBufferLogger::instance().log(LogSeverity::Error, category, std::move(message));
}

}  // namespace wpce::common
