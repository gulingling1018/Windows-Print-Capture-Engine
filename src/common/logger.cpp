/// @file logger.cpp
/// Ring-buffer logger implementation.

#include "common/logger.h"

#include <algorithm>

namespace wpce::common
{

RingBufferLogger& RingBufferLogger::instance()
{
  static RingBufferLogger s_instance;
  return s_instance;
}

void RingBufferLogger::log(LogSeverity severity,
                           std::string_view category,
                           std::string message)
{
  // Capture timestamp BEFORE acquiring the lock to keep the critical
  // section as short as possible.
  const auto timestamp = std::chrono::steady_clock::now().time_since_epoch();

  std::scoped_lock lock(mutex_);

  LogEntry entry;
  entry.timestamp = timestamp;
  entry.severity = severity;
  entry.category = category;
  entry.message = std::move(message);
  buffer_.push_back(std::move(entry));

  // Trim to capacity — erase oldest entries from the front.
  // O(N) memmove of up to ~160 KB (2000 entries × ~80 B) which is
  // < 50 µs on modern CPUs.  Acceptable for the logging hot path;
  // switch to true ring indexing if profiling shows it matters.
  while (buffer_.size() > capacity_)
  {
    buffer_.erase(buffer_.begin());
  }
}

std::vector<LogEntry> RingBufferLogger::dump_recent(std::size_t count) const
{
  std::scoped_lock lock(mutex_);

  if (buffer_.empty())
    return {};

  const std::size_t limit = std::min(count, buffer_.size());
  std::vector<LogEntry> result;
  result.reserve(limit);

  // Walk backwards from the end to return newest first.
  for (std::size_t i = 0; i < limit; ++i)
  {
    result.push_back(buffer_[buffer_.size() - 1 - i]);
  }
  return result;
}

std::vector<LogEntry> RingBufferLogger::dump_all() const
{
  return dump_recent(capacity_);
}

std::size_t RingBufferLogger::size() const
{
  std::scoped_lock lock(mutex_);
  return buffer_.size();
}

void RingBufferLogger::clear()
{
  std::scoped_lock lock(mutex_);
  buffer_.clear();
}

void RingBufferLogger::set_capacity(std::size_t capacity)
{
  if (capacity == 0) capacity = 1;

  std::scoped_lock lock(mutex_);
  capacity_ = capacity;

  // If we're over the new capacity, keep only the most recent entries.
  while (buffer_.size() > capacity_)
  {
    buffer_.erase(buffer_.begin());
  }
}

std::string_view RingBufferLogger::to_string(LogSeverity sev)
{
  switch (sev)
  {
    case LogSeverity::Debug: return "DEBUG";
    case LogSeverity::Info:  return "INFO";
    case LogSeverity::Warn:  return "WARN";
    case LogSeverity::Error: return "ERROR";
    case LogSeverity::Fatal: return "FATAL";
  }
  return "?";
}

}  // namespace wpce::common
