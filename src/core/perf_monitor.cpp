#include "core/perf_monitor.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#else
#include <unistd.h>
#endif

#include <chrono>
#include <cstdio>
#include <ctime>
#include <sstream>

namespace wpce::core
{

namespace
{

/// Format a UTC time_point as an ISO 8601 string.
std::string format_utc(std::chrono::system_clock::time_point tp)
{
  const auto time_t = std::chrono::system_clock::to_time_t(tp);
  std::tm utc_tm{};
#ifdef _WIN32
  gmtime_s(&utc_tm, &time_t);
#else
  gmtime_r(&time_t, &utc_tm);
#endif

  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    tp.time_since_epoch()) %
                  1000;

  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                utc_tm.tm_year + 1900, utc_tm.tm_mon + 1, utc_tm.tm_mday,
                utc_tm.tm_hour, utc_tm.tm_min, utc_tm.tm_sec,
                static_cast<long long>(ms.count()));
  return buf;
}

}  // namespace

PerfMonitor::PerfMonitor()
{
#ifdef _WIN32
  process_handle_ = GetCurrentProcess();

  SYSTEM_INFO sys_info{};
  GetSystemInfo(&sys_info);
  processor_count_ = static_cast<std::uint32_t>(sys_info.dwNumberOfProcessors);
  if (processor_count_ == 0) processor_count_ = 1;

  // Seed the baseline.
  FILETIME ft_create{}, ft_exit{}, ft_kernel{}, ft_user{};
  if (GetProcessTimes(process_handle_, &ft_create, &ft_exit, &ft_kernel, &ft_user))
  {
    ULARGE_INTEGER k{};
    k.LowPart = ft_kernel.dwLowDateTime;
    k.HighPart = ft_kernel.dwHighDateTime;
    ULARGE_INTEGER u{};
    u.LowPart = ft_user.dwLowDateTime;
    u.HighPart = ft_user.dwHighDateTime;
    prev_cpu_time_ = k.QuadPart + u.QuadPart;
  }
  prev_sample_time_ = std::chrono::steady_clock::now();
#endif
}

PerfMonitor::~PerfMonitor() = default;

PerfSample PerfMonitor::sample() const
{
  PerfSample s;
  s.timestamp_utc = format_utc(std::chrono::system_clock::now());

#ifdef _WIN32
  // ── CPU % ──────────────────────────────────────────────────────────
  FILETIME ft_create{}, ft_exit{}, ft_kernel{}, ft_user{};
  if (GetProcessTimes(process_handle_, &ft_create, &ft_exit, &ft_kernel, &ft_user))
  {
    ULARGE_INTEGER k{};
    k.LowPart = ft_kernel.dwLowDateTime;
    k.HighPart = ft_kernel.dwHighDateTime;
    ULARGE_INTEGER u{};
    u.LowPart = ft_user.dwLowDateTime;
    u.HighPart = ft_user.dwHighDateTime;
    const std::uint64_t total = k.QuadPart + u.QuadPart;

    const auto now = std::chrono::steady_clock::now();

    {
      std::scoped_lock lock(prev_mutex_);
      const auto elapsed_cpu = total - prev_cpu_time_;
      const auto elapsed_wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     now - prev_sample_time_)
                                     .count();
      if (elapsed_wall_ns > 0)
      {
        // CPU time is in 100-ns units.
        s.cpu_percent = (static_cast<double>(elapsed_cpu) * 100.0) /
                        (static_cast<double>(elapsed_wall_ns) / 100.0);
        // Normalise per logical processor.
        s.cpu_percent /= static_cast<double>(processor_count_);
      }

      prev_cpu_time_ = total;
      prev_sample_time_ = now;
    }
  }

  // ── Working set ────────────────────────────────────────────────────
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  pmc.cb = sizeof(pmc);
  if (GetProcessMemoryInfo(process_handle_,
                            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                            sizeof(pmc)))
  {
    s.working_set_bytes = pmc.WorkingSetSize;
  }

  // ── Handle count ───────────────────────────────────────────────────
  DWORD handle_count = 0;
  if (GetProcessHandleCount(process_handle_, &handle_count))
  {
    s.handle_count = static_cast<std::uint32_t>(handle_count);
  }

  // ── Thread count (via toolhelp snapshot) ───────────────────────────
  // We count threads in the current process only.
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot != INVALID_HANDLE_VALUE)
  {
    const DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    std::uint32_t thread_count = 0;
    if (Thread32First(snapshot, &te))
    {
      do
      {
        if (te.th32OwnerProcessID == pid)
        {
          ++thread_count;
        }
      } while (Thread32Next(snapshot, &te));
    }
    s.thread_count = thread_count;
    CloseHandle(snapshot);
  }
#endif

  // ── WPCE-specific metrics ──────────────────────────────────────────
  s.snapshot_writer_queue_depth = snapshot_writer_queue_depth_.load();
  s.page_writer_queue_depth = page_writer_queue_depth_.load();
  s.last_event_processing_ms = last_event_processing_ms_.load();

  {
    std::scoped_lock lock(notif_mutex_);
    if (last_notification_time_.time_since_epoch().count() > 0)
    {
      s.watcher_idle_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - last_notification_time_)
          .count();
    }
  }

  return s;
}

void PerfMonitor::set_snapshot_writer_queue_depth(std::size_t depth) noexcept
{
  snapshot_writer_queue_depth_.store(depth);
}

void PerfMonitor::set_page_writer_queue_depth(std::size_t depth) noexcept
{
  page_writer_queue_depth_.store(depth);
}

void PerfMonitor::set_last_notification_time(std::chrono::steady_clock::time_point t) noexcept
{
  std::scoped_lock lock(notif_mutex_);
  last_notification_time_ = t;
}

void PerfMonitor::set_last_event_processing_ms(double ms) noexcept
{
  last_event_processing_ms_.store(ms);
}

}  // namespace wpce::core
