#include "watcher/print_watcher.h"
#include "collector/metadata_collector.h"

#ifdef _WIN32
#include <windows.h>
#include <winspool.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace wpce::watcher
{
namespace
{

// ── UTC timestamp generator ────────────────────────────────────────────

std::string utc_timestamp_now()
{
  SYSTEMTIME st{};
  GetSystemTime(&st);

  std::ostringstream output;
  output << std::setfill('0')
         << std::setw(4) << st.wYear << '-'
         << std::setw(2) << st.wMonth << '-'
         << std::setw(2) << st.wDay << 'T'
         << std::setw(2) << st.wHour << ':'
         << std::setw(2) << st.wMinute << ':'
         << std::setw(2) << st.wSecond << 'Z';
  return output.str();
}

// ── Phase classification ───────────────────────────────────────────────

// Status flags that map to each lifecycle phase.
// Priority: terminal phases checked first → Failed > Cancelled > Completed > Printing > Spooling > Accepted > Unknown.

bool has_all_status_bits(std::uint32_t value, std::uint32_t mask) noexcept
{
  return (value & mask) == mask;
}

// ── Status-name lookup (unchanged from Phase 1) ────────────────────────

struct StatusName
{
  std::uint32_t flag;
  std::string_view name;
};

#ifdef _WIN32
constexpr StatusName kStatusNames[] = {
  {JOB_STATUS_PAUSED, "paused"},
  {JOB_STATUS_ERROR, "error"},
  {JOB_STATUS_DELETING, "deleting"},
  {JOB_STATUS_SPOOLING, "spooling"},
  {JOB_STATUS_PRINTING, "printing"},
  {JOB_STATUS_OFFLINE, "offline"},
  {JOB_STATUS_PAPEROUT, "paperout"},
  {JOB_STATUS_PRINTED, "printed"},
  {JOB_STATUS_DELETED, "deleted"},
  {JOB_STATUS_BLOCKED_DEVQ, "blocked_devq"},
  {JOB_STATUS_USER_INTERVENTION, "user_intervention"},
  {JOB_STATUS_RESTART, "restart"},
#ifdef JOB_STATUS_COMPLETE
  {JOB_STATUS_COMPLETE, "complete"},
#endif
#ifdef JOB_STATUS_RETAINED
  {JOB_STATUS_RETAINED, "retained"},
#endif
#ifdef JOB_STATUS_RENDERING_LOCALLY
  {JOB_STATUS_RENDERING_LOCALLY, "rendering_locally"},
#endif
};
#else
constexpr StatusName kStatusNames[] = {
  {0x00000001, "paused"},
  {0x00000002, "error"},
  {0x00000004, "deleting"},
  {0x00000008, "spooling"},
  {0x00000010, "printing"},
  {0x00000020, "offline"},
  {0x00000040, "paperout"},
  {0x00000080, "printed"},
  {0x00000100, "deleted"},
  {0x00000200, "blocked_devq"},
  {0x00000400, "user_intervention"},
  {0x00000800, "restart"},
  {0x00001000, "complete"},
  {0x00002000, "retained"},
  {0x00004000, "rendering_locally"},
};
#endif

std::string make_signature(const core::JobSnapshot& snapshot)
{
  std::ostringstream signature;
  signature << snapshot.job_id << '\n'
            << snapshot.user << '\n'
            << snapshot.document_name << '\n'
            << snapshot.printer_name << '\n'
            << snapshot.submit_time_utc << '\n'
            << snapshot.status;
  return signature.str();
}

#ifdef _WIN32

std::string format_win32_error(const std::string_view operation, const DWORD error)
{
  LPSTR message = nullptr;
  const DWORD size = FormatMessageA(
    FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
    nullptr,
    error,
    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
    reinterpret_cast<LPSTR>(&message),
    0,
    nullptr);

  std::ostringstream output;
  output << operation << " failed with Win32 error " << error;
  if (size > 0 && message != nullptr)
  {
    std::string text(message, size);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == '.'))
    {
      text.pop_back();
    }
    output << ": " << text;
  }

  if (message != nullptr)
  {
    LocalFree(message);
  }

  return output.str();
}

[[noreturn]] void throw_last_error(const std::string_view operation)
{
  throw std::runtime_error(format_win32_error(operation, GetLastError()));
}

class PrinterHandle
{
 public:
  explicit PrinterHandle(const std::wstring& printer_name)
  {
    if (!OpenPrinterW(const_cast<LPWSTR>(printer_name.c_str()), &handle_, nullptr))
    {
      throw_last_error("OpenPrinterW");
    }
  }

  ~PrinterHandle()
  {
    if (handle_ != nullptr)
    {
      ClosePrinter(handle_);
    }
  }

  PrinterHandle(const PrinterHandle&) = delete;
  PrinterHandle& operator=(const PrinterHandle&) = delete;

  [[nodiscard]] HANDLE get() const noexcept
  {
    return handle_;
  }

 private:
  HANDLE handle_{nullptr};
};

class ChangeNotificationHandle
{
 public:
  explicit ChangeNotificationHandle(HANDLE printer)
  {
    job_notify_type_.Type = JOB_NOTIFY_TYPE;
    job_notify_type_.Count = static_cast<DWORD>(job_fields_.size());
    job_notify_type_.pFields = job_fields_.data();

    options_.Version = 2;
    options_.Count = 1;
    options_.pTypes = &job_notify_type_;

    handle_ = FindFirstPrinterChangeNotification(
      printer,
      PRINTER_CHANGE_ADD_JOB | PRINTER_CHANGE_SET_JOB | PRINTER_CHANGE_WRITE_JOB | PRINTER_CHANGE_DELETE_JOB,
      0,
      &options_);

    if (handle_ == nullptr || handle_ == INVALID_HANDLE_VALUE)
    {
      throw_last_error("FindFirstPrinterChangeNotification");
    }
  }

  ~ChangeNotificationHandle()
  {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE)
    {
      FindClosePrinterChangeNotification(handle_);
    }
  }

  ChangeNotificationHandle(const ChangeNotificationHandle&) = delete;
  ChangeNotificationHandle& operator=(const ChangeNotificationHandle&) = delete;

  [[nodiscard]] HANDLE get() const noexcept
  {
    return handle_;
  }

 private:
  std::array<WORD, 9> job_fields_{
    JOB_NOTIFY_FIELD_PRINTER_NAME,
    JOB_NOTIFY_FIELD_USER_NAME,
    JOB_NOTIFY_FIELD_STATUS,
    JOB_NOTIFY_FIELD_DOCUMENT,
    JOB_NOTIFY_FIELD_SUBMITTED,
    JOB_NOTIFY_FIELD_TOTAL_PAGES,
    JOB_NOTIFY_FIELD_PAGES_PRINTED,
    JOB_NOTIFY_FIELD_TOTAL_BYTES,
    JOB_NOTIFY_FIELD_TIME,
  };
  PRINTER_NOTIFY_OPTIONS_TYPE job_notify_type_{};
  PRINTER_NOTIFY_OPTIONS options_{};
  HANDLE handle_{nullptr};
};

class NotifyInfoHandle
{
 public:
  explicit NotifyInfoHandle(PPRINTER_NOTIFY_INFO info) noexcept : info_(info) {}

  ~NotifyInfoHandle()
  {
    if (info_ != nullptr)
    {
      FreePrinterNotifyInfo(info_);
    }
  }

  NotifyInfoHandle(const NotifyInfoHandle&) = delete;
  NotifyInfoHandle& operator=(const NotifyInfoHandle&) = delete;

  [[nodiscard]] PPRINTER_NOTIFY_INFO get() const noexcept
  {
    return info_;
  }

 private:
  PPRINTER_NOTIFY_INFO info_{nullptr};
};

std::string to_utf8(const wchar_t* value)
{
  if (value == nullptr || value[0] == L'\0')
  {
    return {};
  }

  const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1)
  {
    return {};
  }

  std::string result(static_cast<std::size_t>(size), '\0');
  const int written = WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
  if (written <= 0)
  {
    throw_last_error("WideCharToMultiByte");
  }

  if (!result.empty() && result.back() == '\0')
  {
    result.pop_back();
  }
  return result;
}

std::string to_utf8_buffer(const void* value, const DWORD size_bytes)
{
  if (value == nullptr || size_bytes < sizeof(wchar_t))
  {
    return {};
  }

  const auto* text = static_cast<const wchar_t*>(value);
  const std::size_t length = size_bytes / sizeof(wchar_t);
  if (length == 0 || text[0] == L'\0')
  {
    return {};
  }

  std::wstring bounded(text, text + length);
  while (!bounded.empty() && bounded.back() == L'\0')
  {
    bounded.pop_back();
  }
  return to_utf8(bounded.c_str());
}

std::string format_system_time_utc(const SYSTEMTIME& submitted)
{
  if (submitted.wYear == 0)
  {
    return {};
  }

  std::ostringstream output;
  output << std::setfill('0') << std::setw(4) << submitted.wYear << '-' << std::setw(2) << submitted.wMonth << '-'
         << std::setw(2) << submitted.wDay << 'T' << std::setw(2) << submitted.wHour << ':' << std::setw(2)
         << submitted.wMinute << ':' << std::setw(2) << submitted.wSecond << 'Z';
  return output.str();
}

core::JobSnapshot to_snapshot(const JOB_INFO_2W& job)
{
  return core::JobSnapshot{
    .schema_version = 1,
    .job_id = job.JobId,
    .user = to_utf8(job.pUserName),
    .document_name = to_utf8(job.pDocument),
    .printer_name = to_utf8(job.pPrinterName),
    .submit_time_utc = format_system_time_utc(job.Submitted),
    .status = describe_job_status(job.Status),
    .raw_status_flags = job.Status,
    .source = "winspool",
  };
}

std::optional<JobSnapshotWithMetadata> get_job_snapshot_by_id(HANDLE printer, const DWORD job_id)
{
  DWORD bytes_needed = 0;
  if (GetJobW(printer, job_id, 2, nullptr, 0, &bytes_needed) != FALSE)
  {
    return std::nullopt;
  }

  const DWORD probe_error = GetLastError();
  if (probe_error != ERROR_INSUFFICIENT_BUFFER || bytes_needed == 0)
  {
    return std::nullopt;
  }

  std::vector<BYTE> buffer(bytes_needed);
  if (!GetJobW(printer, job_id, 2, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_needed))
  {
    return std::nullopt;
  }

  const auto* job = reinterpret_cast<const JOB_INFO_2W*>(buffer.data());
  core::MetadataSnapshot metadata = collector::extract_metadata(*job);

  // NOTE: We intentionally do NOT call DocumentPropertiesW here.
  // DocumentPropertiesW returns the printer's CURRENT DEFAULT DEVMODE,
  // NOT the per-job settings that the application sent.  For v4 drivers
  // (Windows 10/11 default), pDevMode is always NULL and all settings
  // are carried via PrintTicket XML — which is captured by the XPS
  // filter DLL (wpce_xps_filter) in the pipeline, not here.
  //
  // For GDI (Type 3) drivers, JOB_INFO_2W::pDevMode IS the correct
  // per-job DEVMODE and extract_metadata() uses it directly.
  //
  // The only reliable approach for v4 drivers:
  //   1. wpce_xps_filter.dll intercepts PrintTicket XML per job
  //   2. The PrintTicket parser converts it to MetadataSnapshot
  //   3. MetadataMerger combines filter data with spooler lifecycle data
  // See src/parser/print_ticket_parser.h for details.

  return JobSnapshotWithMetadata{.job = to_snapshot(*job), .metadata = std::move(metadata)};
}

struct JobNotificationSnapshot
{
  std::uint32_t job_id{0};
  std::string user;
  std::string document_name;
  std::string printer_name;
  std::string submit_time_utc;
  std::string status;
  bool has_field_data{false};
  std::int32_t pages_printed{-1};
  std::int32_t total_pages{-1};
  std::uint32_t total_bytes{0};
};

bool has_job_change_flags(const DWORD change_flags) noexcept
{
  return (change_flags &
          (PRINTER_CHANGE_ADD_JOB | PRINTER_CHANGE_SET_JOB | PRINTER_CHANGE_WRITE_JOB | PRINTER_CHANGE_DELETE_JOB)) !=
         0;
}

std::string fallback_status_from_change_flags(const DWORD change_flags)
{
  if ((change_flags & PRINTER_CHANGE_DELETE_JOB) != 0)
  {
    return "notification_deleted_or_unavailable";
  }
  if ((change_flags & PRINTER_CHANGE_ADD_JOB) != 0)
  {
    return "notification_added_or_unavailable";
  }
  if ((change_flags & PRINTER_CHANGE_WRITE_JOB) != 0)
  {
    return "notification_written_or_unavailable";
  }
  if ((change_flags & PRINTER_CHANGE_SET_JOB) != 0)
  {
    return "notification_changed_or_unavailable";
  }
  return "notification_only";
}

void apply_job_notification_field(JobNotificationSnapshot& snapshot, const PRINTER_NOTIFY_INFO_DATA& data)
{
  snapshot.has_field_data = true;

  switch (data.Field)
  {
    case JOB_NOTIFY_FIELD_PRINTER_NAME:
      snapshot.printer_name = to_utf8_buffer(data.NotifyData.Data.pBuf, data.NotifyData.Data.cbBuf);
      break;
    case JOB_NOTIFY_FIELD_USER_NAME:
      snapshot.user = to_utf8_buffer(data.NotifyData.Data.pBuf, data.NotifyData.Data.cbBuf);
      break;
    case JOB_NOTIFY_FIELD_STATUS:
      snapshot.status = describe_job_status(data.NotifyData.adwData[0]);
      break;
    case JOB_NOTIFY_FIELD_DOCUMENT:
      snapshot.document_name = to_utf8_buffer(data.NotifyData.Data.pBuf, data.NotifyData.Data.cbBuf);
      break;
    case JOB_NOTIFY_FIELD_SUBMITTED:
      if (data.NotifyData.Data.pBuf != nullptr && data.NotifyData.Data.cbBuf >= sizeof(SYSTEMTIME))
      {
        const auto* submitted = static_cast<const SYSTEMTIME*>(data.NotifyData.Data.pBuf);
        snapshot.submit_time_utc = format_system_time_utc(*submitted);
      }
      break;
    case JOB_NOTIFY_FIELD_TOTAL_PAGES:
      snapshot.total_pages = static_cast<std::int32_t>(data.NotifyData.adwData[0]);
      break;
    case JOB_NOTIFY_FIELD_PAGES_PRINTED:
      snapshot.pages_printed = static_cast<std::int32_t>(data.NotifyData.adwData[0]);
      break;
    case JOB_NOTIFY_FIELD_TOTAL_BYTES:
      snapshot.total_bytes = data.NotifyData.adwData[0];
      break;
    case JOB_NOTIFY_FIELD_TIME:
      // JOB_NOTIFY_FIELD_TIME — stored in adwData as a SYSTEMTIME or FILETIME.
      // We capture this information via the submitted field above; time is kept
      // for diagnostic purposes only.
      break;
    default:
      break;
  }
}

std::vector<JobNotificationSnapshot> parse_job_notifications(
  const PRINTER_NOTIFY_INFO* info,
  const DWORD change_flags)
{
  std::unordered_map<std::uint32_t, JobNotificationSnapshot> snapshots;

  if (info != nullptr)
  {
    for (DWORD index = 0; index < info->Count; ++index)
    {
      const PRINTER_NOTIFY_INFO_DATA& data = info->aData[index];
      if (data.Type != JOB_NOTIFY_TYPE || data.Id == 0)
      {
        continue;
      }

      auto& snapshot = snapshots[data.Id];
      snapshot.job_id = data.Id;
      apply_job_notification_field(snapshot, data);
    }
  }

  std::vector<JobNotificationSnapshot> result;
  result.reserve(snapshots.size());
  for (auto& entry : snapshots)
  {
    if (entry.second.status.empty())
    {
      entry.second.status = fallback_status_from_change_flags(change_flags);
    }
    result.push_back(std::move(entry.second));
  }

  std::sort(result.begin(), result.end(), [](const JobNotificationSnapshot& left, const JobNotificationSnapshot& right) {
    return left.job_id < right.job_id;
  });
  return result;
}

core::JobSnapshot make_notification_only_snapshot(
  const std::string& configured_printer_name,
  const JobNotificationSnapshot& notification,
  const std::unordered_map<std::uint32_t, core::JobSnapshot>& known_snapshots)
{
  core::JobSnapshot snapshot;
  const auto existing = known_snapshots.find(notification.job_id);
  if (existing != known_snapshots.end())
  {
    snapshot = existing->second;
  }

  snapshot.schema_version = 1;
  snapshot.job_id = notification.job_id;
  snapshot.source = "winspool-notify";

  if (!notification.user.empty())
  {
    snapshot.user = notification.user;
  }
  if (!notification.document_name.empty())
  {
    snapshot.document_name = notification.document_name;
  }
  if (!notification.printer_name.empty())
  {
    snapshot.printer_name = notification.printer_name;
  }
  if (snapshot.printer_name.empty())
  {
    snapshot.printer_name = configured_printer_name;
  }
  if (!notification.submit_time_utc.empty())
  {
    snapshot.submit_time_utc = notification.submit_time_utc;
  }
  if (!notification.status.empty())
  {
    snapshot.status = notification.status;
  }
  if (snapshot.status.empty())
  {
    snapshot.status = "notification_only";
  }

  return snapshot;
}

// ── Lifecycle event builder helper ─────────────────────────────────────

core::JobLifecycleEvent make_lifecycle_event(std::uint32_t job_id,
                                             int sequence,
                                             core::JobLifecyclePhase phase,
                                             const std::string& status_string,
                                             int pages_printed,
                                             int total_pages)
{
  core::JobLifecycleEvent event;
  event.event_id = std::to_string(job_id) + "-" + std::to_string(sequence);
  event.job_id = job_id;
  event.sequence = sequence;
  event.timestamp_utc = utc_timestamp_now();
  event.phase = phase;
  event.status_flags = status_string;
  event.pages_printed = pages_printed;
  event.total_pages = total_pages;

  // Build human-readable detail.
  std::ostringstream detail;
  detail << "Job " << job_id << " phase: " << core::to_string(phase);
  if (total_pages > 0)
  {
    detail << ", page " << pages_printed << '/' << total_pages
           << " (" << (pages_printed * 100 / total_pages) << "%)";
  }
  detail << ", status: " << status_string;
  event.detail = detail.str();

  return event;
}

#endif

}  // namespace

// ── Public phase classifier ─────────────────────────────────────────────

core::JobLifecyclePhase classify_phase(const std::uint32_t status_flags,
                                       const std::string& /*status_string*/) noexcept
{
  if (status_flags == 0)
  {
    return core::JobLifecyclePhase::Unknown;
  }

  // Terminal / error states first (priority order).
#ifdef JOB_STATUS_ERROR
  if (has_all_status_bits(status_flags, JOB_STATUS_ERROR))
    return core::JobLifecyclePhase::Failed;
#endif
#ifdef JOB_STATUS_PAPEROUT
  if (has_all_status_bits(status_flags, JOB_STATUS_PAPEROUT))
    return core::JobLifecyclePhase::Failed;
#endif
#ifdef JOB_STATUS_OFFLINE
  if (has_all_status_bits(status_flags, JOB_STATUS_OFFLINE))
    return core::JobLifecyclePhase::Failed;
#endif
#ifdef JOB_STATUS_USER_INTERVENTION
  if (has_all_status_bits(status_flags, JOB_STATUS_USER_INTERVENTION))
    return core::JobLifecyclePhase::Failed;
#endif

#ifdef JOB_STATUS_DELETED
  if (has_all_status_bits(status_flags, JOB_STATUS_DELETED))
    return core::JobLifecyclePhase::Cancelled;
#endif
#ifdef JOB_STATUS_DELETING
  if (has_all_status_bits(status_flags, JOB_STATUS_DELETING) &&
      !has_all_status_bits(status_flags, JOB_STATUS_DELETED))
    return core::JobLifecyclePhase::Cancelled;
#endif

#ifdef JOB_STATUS_COMPLETE
  if (has_all_status_bits(status_flags, JOB_STATUS_COMPLETE))
    return core::JobLifecyclePhase::Completed;
#endif
#ifdef JOB_STATUS_RETAINED
  if (has_all_status_bits(status_flags, JOB_STATUS_RETAINED))
    return core::JobLifecyclePhase::Completed;
#endif
#ifdef JOB_STATUS_PRINTED
  if (has_all_status_bits(status_flags, JOB_STATUS_PRINTED) &&
      !has_all_status_bits(status_flags, JOB_STATUS_RETAINED))
    return core::JobLifecyclePhase::Completed;
#endif

  // ── Multi-flag completion: some brands report PRINTING|PRINTED ─────────
  // HP LaserJet & many PCL6 drivers emit PRINTING | PRINTED simultaneously.
  // Treat this as Completed to avoid missing the terminal transition on
  // fast printers where the job disappears before the next notification cycle.
#ifdef JOB_STATUS_PRINTING
#ifdef JOB_STATUS_PRINTED
  if (has_all_status_bits(status_flags, JOB_STATUS_PRINTING) &&
      has_all_status_bits(status_flags, JOB_STATUS_PRINTED))
    return core::JobLifecyclePhase::Completed;
#endif
#endif

  // Active / in-progress states.
#ifdef JOB_STATUS_PRINTING
  if (has_all_status_bits(status_flags, JOB_STATUS_PRINTING))
    return core::JobLifecyclePhase::Printing;
#endif

#ifdef JOB_STATUS_SPOOLING
  if (has_all_status_bits(status_flags, JOB_STATUS_SPOOLING))
    return core::JobLifecyclePhase::Spooling;
#endif

#ifdef JOB_STATUS_RENDERING_LOCALLY
  // Epson ESC/P-R and Canon UFR II v4 drivers use RENDERING_LOCALLY
  // instead of SPOOLING.  Treat as spooling-equivalent.
  if (has_all_status_bits(status_flags, JOB_STATUS_RENDERING_LOCALLY))
    return core::JobLifecyclePhase::Spooling;
#endif

  // Paused / blocked — still active but not progressing.
#ifdef JOB_STATUS_PAUSED
  if (has_all_status_bits(status_flags, JOB_STATUS_PAUSED))
    return core::JobLifecyclePhase::Spooling;
#endif
#ifdef JOB_STATUS_BLOCKED_DEVQ
  if (has_all_status_bits(status_flags, JOB_STATUS_BLOCKED_DEVQ))
    return core::JobLifecyclePhase::Spooling;
#endif
#ifdef JOB_STATUS_RESTART
  if (has_all_status_bits(status_flags, JOB_STATUS_RESTART))
    return core::JobLifecyclePhase::Spooling;
#endif

  return core::JobLifecyclePhase::Unknown;
}

bool should_preserve_transient_unknown_status(
  const core::JobLifecyclePhase current_phase,
  const bool has_prior_events) noexcept
{
  if (!has_prior_events)
  {
    return false;
  }

  return current_phase == core::JobLifecyclePhase::Accepted ||
         current_phase == core::JobLifecyclePhase::Spooling ||
         current_phase == core::JobLifecyclePhase::Printing ||
         current_phase == core::JobLifecyclePhase::Stalled;
}

std::string describe_job_status(const std::uint32_t status_flags)
{
  if (status_flags == 0)
  {
    return "unknown";
  }

  std::string result;
  for (const StatusName& status : kStatusNames)
  {
    if ((status_flags & status.flag) == status.flag)
    {
      if (!result.empty())
      {
        result += '|';
      }
      result += status.name;
    }
  }

  if (result.empty())
  {
    result = "unknown(" + std::to_string(status_flags) + ")";
  }

  return result;
}

std::vector<JobSnapshotWithMetadata> enumerate_printer_jobs(const std::wstring& printer_name)
{
#ifdef _WIN32
  PrinterHandle printer(printer_name);

  DWORD bytes_needed = 0;
  DWORD jobs_returned = 0;
  constexpr DWORD kMaxJobsPerSnapshot = 1024;

  const BOOL probe_result =
    EnumJobsW(printer.get(), 0, kMaxJobsPerSnapshot, 2, nullptr, 0, &bytes_needed, &jobs_returned);
  if (probe_result != FALSE)
  {
    return {};
  }

  const DWORD probe_error = GetLastError();
  if (probe_error != ERROR_INSUFFICIENT_BUFFER)
  {
    if (probe_error == ERROR_SUCCESS || bytes_needed == 0)
    {
      return {};
    }
    throw std::runtime_error(format_win32_error("EnumJobsW", probe_error));
  }

  // Add headroom — the spooler may add jobs between probe and fetch during
  // high-concurrency scenarios (e.g. simulator stress tests).
  std::vector<BYTE> buffer(bytes_needed + (sizeof(JOB_INFO_2W) * 64));
  DWORD buffer_size = static_cast<DWORD>(buffer.size());

  for (int attempt = 0; attempt < 2; ++attempt)
  {
    if (EnumJobsW(
          printer.get(),
          0,
          kMaxJobsPerSnapshot,
          2,
          buffer.data(),
          buffer_size,
          &bytes_needed,
          &jobs_returned))
    {
      break;
    }

    const DWORD err = GetLastError();
    if (err == ERROR_INSUFFICIENT_BUFFER && attempt == 0)
    {
      // Retry once with the new size reported by the failed call.
      buffer.resize(bytes_needed + (sizeof(JOB_INFO_2W) * 64));
      buffer_size = static_cast<DWORD>(buffer.size());
      continue;
    }

    throw std::runtime_error(format_win32_error("EnumJobsW", err));
  }

  const auto* jobs = reinterpret_cast<const JOB_INFO_2W*>(buffer.data());
  std::vector<JobSnapshotWithMetadata> snapshots;
  snapshots.reserve(jobs_returned);
  for (DWORD index = 0; index < jobs_returned; ++index)
  {
    snapshots.push_back(JobSnapshotWithMetadata{
      .job = to_snapshot(jobs[index]),
      .metadata = collector::extract_metadata(jobs[index]),
    });
  }

  std::sort(snapshots.begin(), snapshots.end(), [](const JobSnapshotWithMetadata& left, const JobSnapshotWithMetadata& right) {
    return left.job.job_id < right.job.job_id;
  });

  return snapshots;
#else
  (void)printer_name;
  throw std::runtime_error("Win32 print job enumeration is only supported on Windows");
#endif
}

// ── PrintWatcher implementation ─────────────────────────────────────────

PrintWatcher::PrintWatcher(WatcherConfig config,
                           JobSnapshotCallback job_callback,
                           MetadataSnapshotCallback metadata_callback,
                           LifecycleEventCallback lifecycle_callback)
  : config_(std::move(config))
  , job_callback_(std::move(job_callback))
  , metadata_callback_(std::move(metadata_callback))
  , lifecycle_callback_(std::move(lifecycle_callback))
{
  if (config_.printer_name.empty())
  {
    throw std::invalid_argument("printer_name is required");
  }
  if (!job_callback_)
  {
    throw std::invalid_argument("job_callback is required");
  }
}

PrintWatcher::~PrintWatcher()
{
  stop();
}

void PrintWatcher::start()
{
  std::scoped_lock lock(lifecycle_mutex_);
  if (running_)
  {
    return;
  }

  stop_requested_ = false;
  set_last_error({});
  running_ = true;
  worker_ = std::thread(&PrintWatcher::run, this);
}

void PrintWatcher::stop() noexcept
{
  std::thread worker;
  {
    std::scoped_lock lock(lifecycle_mutex_);
    stop_requested_ = true;
    if (worker_.joinable())
    {
      worker = std::move(worker_);
    }
  }

  if (worker.joinable())
  {
    worker.join();
  }
}

bool PrintWatcher::running() const noexcept
{
  return running_;
}

std::string PrintWatcher::last_error() const
{
  std::scoped_lock lock(error_mutex_);
  return last_error_;
}

std::vector<core::JobLifecycleSummary> PrintWatcher::lifecycle_summaries() const
{
  std::scoped_lock lock(summaries_mutex_);
  std::vector<core::JobLifecycleSummary> result;
  result.reserve(job_states_.size());
  for (const auto& [id, state] : job_states_)
  {
    auto summary = state.summary;

    // Compute total duration from first to last event.
    if (state.first_event_time.time_since_epoch().count() > 0)
    {
      const auto end = state.last_event_time.time_since_epoch().count() > 0
                         ? state.last_event_time
                         : state.first_event_time;
      summary.total_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end - state.first_event_time).count();
    }

    // Ensure phase_dwells is up-to-date: add current phase dwell if still active.
    if (state.phase_entry_time.time_since_epoch().count() > 0 &&
        !is_terminal_phase(state.current_phase))
    {
      const auto now = std::chrono::steady_clock::now();
      const auto current_dwell = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - state.phase_entry_time).count() + state.cumulative_phase_ms;
      // Only add if not already present.
      const std::string phase_name{core::to_string(state.current_phase)};
      bool found = false;
      for (auto& dwell : summary.phase_dwells)
      {
        if (dwell.phase_name == phase_name)
        {
          dwell.duration_ms = current_dwell;
          found = true;
          break;
        }
      }
      if (!found)
      {
        summary.phase_dwells.push_back(core::PhaseDwellEntry{
          .phase_name = phase_name,
          .duration_ms = current_dwell,
        });
      }
    }

    result.push_back(std::move(summary));
  }

  std::sort(result.begin(), result.end(), [](const core::JobLifecycleSummary& left, const core::JobLifecycleSummary& right) {
    return left.job_id < right.job_id;
  });
  return result;
}

void PrintWatcher::run()
{
  std::unordered_map<std::uint32_t, std::string> emitted_signatures;
  std::unordered_map<std::uint32_t, core::JobSnapshot> known_snapshots;
  std::unordered_map<std::uint32_t, core::MetadataSnapshot> known_metadata;

  // ── Logging helper ────────────────────────────────────────────────────
  const auto log = [&](LogLevel level, std::string_view category, std::string message) {
    if (config_.log_callback)
    {
      config_.log_callback(level, category, std::move(message));
    }
  };

  // ── Lifecycle tracking helpers ───────────────────────────────────────

  const auto ensure_internal_state = [&](std::uint32_t job_id,
                                         const std::string& user,
                                         const std::string& document_name,
                                         const std::string& printer_name,
                                         const std::string& submit_time_utc) -> InternalJobState& {
    auto& state = job_states_[job_id];
    if (state.summary.job_id == 0)
    {
      state.summary.schema_version = 1;
      state.summary.job_id = job_id;
      state.summary.user = user;
      state.summary.document_name = document_name;
      state.summary.printer_name = printer_name;
      state.summary.submit_time_utc = submit_time_utc;
      state.summary.source = "winspool";
      state.last_progress_time = std::chrono::steady_clock::now();
    }
    return state;
  };

  const auto emit_lifecycle_event = [&](InternalJobState& state,
                                        core::JobLifecyclePhase phase,
                                        const std::string& status_string,
                                        int pages_printed,
                                        int total_pages) {
    // Skip if we already emitted a terminal event for this job.
    if (state.summary.events.empty() && phase == core::JobLifecyclePhase::Unknown)
    {
      return;  // Don't emit Unknown as the very first event.
    }

    core::JobLifecycleEvent event = make_lifecycle_event(
      state.summary.job_id,
      state.next_sequence,
      phase,
      status_string,
      pages_printed,
      total_pages);

    // ── Phase timing ──────────────────────────────────────────────────
    const auto now = std::chrono::steady_clock::now();

    // First event tracking.
    if (state.first_event_time.time_since_epoch().count() == 0)
    {
      state.first_event_time = now;
    }

    // Delta since previous event.
    if (state.last_event_time.time_since_epoch().count() > 0)
    {
      event.delta_ms_since_prev = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - state.last_event_time).count();
    }

    // Phase transition detection.
    if (phase != state.current_phase && state.current_phase != core::JobLifecyclePhase::Unknown)
    {
      // Finalize dwell time for the old phase.
      const auto old_phase_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - state.phase_entry_time).count();
      const auto total_old_ms = state.cumulative_phase_ms + old_phase_ms;
      state.summary.phase_dwells.push_back(core::PhaseDwellEntry{
        .phase_name = std::string(core::to_string(state.current_phase)),
        .duration_ms = total_old_ms,
      });
      // Reset for new phase.
      state.phase_entry_time = now;
      state.cumulative_phase_ms = 0;
    }
    else if (state.phase_entry_time.time_since_epoch().count() == 0)
    {
      // First event for this job — start timing.
      state.phase_entry_time = now;
    }

    event.cumulative_phase_ms = state.cumulative_phase_ms +
      std::chrono::duration_cast<std::chrono::milliseconds>(
        now - state.phase_entry_time).count();

    state.current_phase = phase;
    state.last_event_time = now;
    state.summary.events.push_back(event);
    if (!state.summary.events.empty())
    {
      state.summary.last_status = status_string;
    }
    state.summary.final_phase = phase;
    if (total_pages > 0)
    {
      state.summary.progress_percent = (pages_printed * 100) / total_pages;
    }
    else
    {
      state.summary.progress_percent = -1;
    }
    state.next_sequence++;

    if (is_terminal_phase(phase))
    {
      state.last_progress_time = std::chrono::steady_clock::now();  // stop stall clock

      // Remove from dedup maps so that recycled JobIds don't collide with stale entries.
      log(LogLevel::Debug, "cache",
          "Job " + std::to_string(state.summary.job_id) +
          ": terminal phase \"" + std::string(core::to_string(phase)) +
          "\", removed from dedup maps");
      emitted_signatures.erase(state.summary.job_id);
      known_snapshots.erase(state.summary.job_id);
      known_metadata.erase(state.summary.job_id);
    }
    else
    {
      state.last_progress_time = std::chrono::steady_clock::now();
    }

    if (lifecycle_callback_)
    {
      lifecycle_callback_(event);
    }
  };

  const auto process_phase_transition = [&](const core::JobSnapshot& snapshot,
                                            const core::MetadataSnapshot& metadata) {
    const std::uint32_t job_id = snapshot.job_id;
    if (job_id == 0) return;

    InternalJobState& state = ensure_internal_state(
      job_id, snapshot.user, snapshot.document_name,
      snapshot.printer_name, snapshot.submit_time_utc);

#ifdef _WIN32
    // Classify the phase using raw JOB_STATUS flags carried through the snapshot.
    // The string-based fallback below handles cases where flags are zero (e.g.
    // notification-only stubs from fast printers).
    core::JobLifecyclePhase new_phase = classify_phase(snapshot.raw_status_flags, snapshot.status);
    // Fallback: if classify_phase returned Unknown, try to infer from status string.
    if (new_phase == core::JobLifecyclePhase::Unknown &&
        !snapshot.status.empty() && snapshot.status != "unknown")
    {
      // Parse the status string for known substrings.
      const std::string& s = snapshot.status;
      if (s.find("error") != std::string::npos ||
          s.find("offline") != std::string::npos ||
          s.find("paperout") != std::string::npos)
      {
        new_phase = core::JobLifecyclePhase::Failed;
      }
      else if (s.find("deleted") != std::string::npos ||
               s.find("deleting") != std::string::npos)
      {
        new_phase = core::JobLifecyclePhase::Cancelled;
      }
      else if (s.find("printed") != std::string::npos ||
               s.find("complete") != std::string::npos ||
               s.find("retained") != std::string::npos)
      {
        new_phase = core::JobLifecyclePhase::Completed;
      }
      else if (s.find("printing") != std::string::npos)
      {
        new_phase = core::JobLifecyclePhase::Printing;
      }
      else if (s.find("spooling") != std::string::npos ||
               s.find("rendering") != std::string::npos ||
               s.find("blocked") != std::string::npos ||
               s.find("paused") != std::string::npos)
      {
        new_phase = core::JobLifecyclePhase::Spooling;
      }
      else if (s.find("notification_added") != std::string::npos)
      {
        new_phase = core::JobLifecyclePhase::Accepted;
      }
    }
#else
    core::JobLifecyclePhase new_phase = core::JobLifecyclePhase::Unknown;
#endif

    // ── Brand-specific: Kyocera / Ricoh / Sharp drivers ───────────────────
    // These drivers often report zero status flags on the very first
    // notification (or on the first EnumJobs scan).  Without this, the
    // job's first event is suppressed at line 996-998 and no Accepted
    // event fires — the job remains invisible until a later poll.
    if (new_phase == core::JobLifecyclePhase::Unknown &&
        state.summary.events.empty())
    {
      log(LogLevel::Info, "phase",
          "Job " + std::to_string(job_id) +
          ": new job with status_flags=0 → emitting Accepted (brand compat)");
      new_phase = core::JobLifecyclePhase::Accepted;
    }

    // Some virtual and network-like printers briefly report zero status flags
    // while the job is already running or stalled. Preserve the last observed
    // non-terminal phase so a weak "unknown" update does not erase the useful
    // lifecycle state.
    if (new_phase == core::JobLifecyclePhase::Unknown &&
        should_preserve_transient_unknown_status(
          state.current_phase, !state.summary.events.empty()))
    {
      log(LogLevel::Debug, "phase",
          "Job " + std::to_string(job_id) +
          ": preserving last observed phase \"" +
          std::string(core::to_string(state.current_phase)) +
          "\" (raw_status_flags=0)");
      new_phase = state.current_phase;
    }

    // If the phase hasn't changed (and it's not the first event), suppress duplicate.
    if (new_phase == state.current_phase && !state.summary.events.empty())
    {
      // If pages_printed changed, emit a progress update even without a phase change.
      int pages_printed_val = 0;
      int total_pages_val = 0;
      try { pages_printed_val = std::stoi(metadata.pages_printed.value); } catch (...) {}
      try { total_pages_val = std::stoi(metadata.total_pages.value); } catch (...) {}

      const int prev_total = state.summary.events.back().total_pages;
      const int prev_printed = state.summary.events.back().pages_printed;
      if (pages_printed_val != prev_printed || total_pages_val != prev_total)
      {
        // Progress changed — emit an update as the current phase.
        log(LogLevel::Debug, "phase",
            "Job " + std::to_string(job_id) +
            ": progress changed " + std::to_string(prev_printed) + "/" +
            std::to_string(prev_total) + " -> " +
            std::to_string(pages_printed_val) + "/" +
            std::to_string(total_pages_val) + ", emitting update");
        emit_lifecycle_event(state, new_phase, snapshot.status, pages_printed_val, total_pages_val);
      }
      return;
    }

    // If this job is in a true terminal phase, don't emit more events.
    // Stalled is NOT terminal — it can recover when real progress arrives.
    if (is_terminal_phase(state.current_phase) && !state.summary.events.empty())
    {
      log(LogLevel::Debug, "phase",
          "Job " + std::to_string(job_id) +
          ": already terminal \"" +
          std::string(core::to_string(state.current_phase)) +
          "\", suppressing new event");
      return;
    }

    // Log phase transition for non-duplicate, non-terminal events.
    {
      std::ostringstream phase_detail;
      phase_detail << "Job " << job_id << ": 0x"
                   << std::hex << snapshot.raw_status_flags << std::dec
                   << " -> \"" << core::to_string(new_phase) << "\"";
      if (new_phase != state.current_phase && state.current_phase != core::JobLifecyclePhase::Unknown)
      {
        phase_detail << " (transition from \"" << core::to_string(state.current_phase) << "\")";
      }
      log(new_phase == core::JobLifecyclePhase::Unknown ? LogLevel::Warn : LogLevel::Info,
          "phase", phase_detail.str());
    }

    int pages_printed_val = 0;
    int total_pages_val = 0;
    try { pages_printed_val = std::stoi(metadata.pages_printed.value); } catch (...) {}
    try { total_pages_val = std::stoi(metadata.total_pages.value); } catch (...) {}

    emit_lifecycle_event(state, new_phase, snapshot.status, pages_printed_val, total_pages_val);
  };

  // ── Snapshot emission ─────────────────────────────────────────────────

  const auto emit_snapshot = [&](const core::JobSnapshot& snapshot, const core::MetadataSnapshot& metadata) {
    if (snapshot.job_id == 0)
    {
      return;
    }

    // Lifecycle tracking — fires before dedup so every phase transition is recorded.
    process_phase_transition(snapshot, metadata);

    const std::string signature = make_signature(snapshot);
    auto existing = emitted_signatures.find(snapshot.job_id);
    if (existing == emitted_signatures.end() || existing->second != signature)
    {
      if (existing != emitted_signatures.end())
      {
        log(LogLevel::Debug, "dedup",
            "Job " + std::to_string(snapshot.job_id) +
            ": signature changed, emitting snapshot");
      }
      emitted_signatures[snapshot.job_id] = signature;
      known_snapshots[snapshot.job_id] = snapshot;
      known_metadata[snapshot.job_id] = metadata;   // cache for GetJobW-failure recovery
      job_callback_(snapshot);
      if (metadata_callback_)
      {
        metadata_callback_(metadata);
      }
    }
    else if (metadata_callback_)
    {
      // Metadata (pages_printed, copies, color, paper, etc.) can change
      // even when the job-level signature is unchanged.  Always forward
      // fresh metadata so the UI reflects the latest driver state.
      known_metadata[snapshot.job_id] = metadata;   // refresh cache
      metadata_callback_(metadata);
    }
  };

  // Build a minimal metadata snapshot for notification-only fallback paths.
  // When notification-carried page counts are available they are used;
  // otherwise fields are marked NotReported.
  const auto make_metadata_stub = [](std::uint32_t job_id,
                                      const JobNotificationSnapshot& notification) {
    core::MetadataSnapshot stub;
    stub.schema_version = 1;
    stub.job_id = job_id;
    if (notification.total_pages >= 0)
    {
      stub.total_pages = core::FieldValue{std::to_string(notification.total_pages), core::FieldState::Reported};
    }
    else
    {
      stub.total_pages = core::FieldValue{{}, core::FieldState::NotReported};
    }
    if (notification.pages_printed >= 0)
    {
      stub.pages_printed = core::FieldValue{std::to_string(notification.pages_printed), core::FieldState::Reported};
    }
    else
    {
      stub.pages_printed = core::FieldValue{{}, core::FieldState::NotReported};
    }
    stub.copies = core::FieldValue{{}, core::FieldState::NotReported};
    stub.duplex = core::FieldValue{{}, core::FieldState::NotReported};
    stub.color = core::FieldValue{{}, core::FieldState::NotReported};
    stub.paper_size = core::FieldValue{{}, core::FieldState::NotReported};
    stub.orientation = core::FieldValue{{}, core::FieldState::NotReported};
    stub.n_up = core::FieldValue{{}, core::FieldState::NotReported};
    stub.collate = core::FieldValue{{}, core::FieldState::NotReported};
    stub.print_quality = core::FieldValue{{}, core::FieldState::NotReported};
    stub.media_type = core::FieldValue{{}, core::FieldState::NotReported};
    stub.page_range = core::FieldValue{{}, core::FieldState::NotReported};
    return stub;
  };

  const auto emit_changed_jobs = [&]() {
    const auto jobs = enumerate_printer_jobs(config_.printer_name);
    if (!jobs.empty())
    {
      log(LogLevel::Debug, "rescan",
          "EnumJobs returned " + std::to_string(jobs.size()) + " job(s)");
    }
    for (const JobSnapshotWithMetadata& entry : jobs)
    {
      emit_snapshot(entry.job, entry.metadata);
    }
  };

  // ── Main loop ─────────────────────────────────────────────────────────

  try
  {
#ifdef _WIN32
    PrinterHandle printer(config_.printer_name);
    ChangeNotificationHandle notification(printer.get());
    const std::string configured_printer_name = to_utf8(const_cast<LPWSTR>(config_.printer_name.c_str()));

    log(LogLevel::Info, "init",
        "Printer \"" + configured_printer_name + "\" opened, change notification registered");

    emit_changed_jobs();

    auto last_stall_check = std::chrono::steady_clock::now();
    auto last_rescan = std::chrono::steady_clock::now();

    while (!stop_requested_)
    {
      // ── Periodic full rescan — safety net for spoolers that drop notifications ──
      if (config_.rescan_interval.count() > 0)
      {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_rescan > config_.rescan_interval)
        {
          log(LogLevel::Info, "rescan",
              "Periodic rescan triggered (interval=" +
              std::to_string(config_.rescan_interval.count()) + "s)");
          emit_changed_jobs();
          last_rescan = now;
        }
      }

      const DWORD wait_result =
        WaitForSingleObject(notification.get(), static_cast<DWORD>(config_.wait_timeout.count()));
      if (wait_result == WAIT_TIMEOUT)
      {
        // Periodic stall check on timeout.
        tick_stall_detection();
        continue;
      }
      if (wait_result != WAIT_OBJECT_0)
      {
        throw std::runtime_error(format_win32_error("WaitForSingleObject", GetLastError()));
      }

      perf_monitor_.set_last_notification_time(std::chrono::steady_clock::now());
      const auto processing_start = std::chrono::steady_clock::now();

      DWORD change_flags = 0;
      PPRINTER_NOTIFY_INFO raw_info = nullptr;
      if (!FindNextPrinterChangeNotification(
            notification.get(),
            &change_flags,
            nullptr,
            reinterpret_cast<LPVOID*>(&raw_info)))
      {
        throw_last_error("FindNextPrinterChangeNotification");
      }

      NotifyInfoHandle info(raw_info);
      const std::vector<JobNotificationSnapshot> job_notifications =
        parse_job_notifications(info.get(), change_flags);

      if (!job_notifications.empty())
      {
        std::ostringstream notify_detail;
        notify_detail << "Notification fired, change_flags=0x"
                      << std::hex << change_flags << std::dec
                      << ", " << job_notifications.size() << " job(s)";
        log(LogLevel::Debug, "notify", notify_detail.str());
      }

      for (const JobNotificationSnapshot& job_notification : job_notifications)
      {
        if (const std::optional<JobSnapshotWithMetadata> current =
              get_job_snapshot_by_id(printer.get(), job_notification.job_id);
            current.has_value())
        {
          log(LogLevel::Debug, "getjob",
              "Job " + std::to_string(job_notification.job_id) +
              ": fetched JOB_INFO_2W (status=" + current->job.status +
              ", pages=" + current->metadata.pages_printed.value + "/" +
              current->metadata.total_pages.value + ")");
          emit_snapshot(current->job, current->metadata);
        }
        else
        {
          const bool had_known = known_snapshots.find(job_notification.job_id) != known_snapshots.end();
          log(LogLevel::Debug, "getjob",
              "Job " + std::to_string(job_notification.job_id) +
              ": GetJobW failed, using notification-only stub (known_snapshot=" +
              (had_known ? "yes" : "no") + ")");

          // ── Recover from metadata cache ─────────────────────────────
          // When GetJobW fails (job already deleted from spooler), we
          // lose pDevMode.  Recover previously-cached DEVMODE fields so
          // the UI does not flash all-N/A then recover on the next
          // PrintTicket merge.
          auto metadata = make_metadata_stub(job_notification.job_id, job_notification);
          {
            auto cached = known_metadata.find(job_notification.job_id);
            if (cached != known_metadata.end())
            {
              // Overlay cached field values — notification carries
              // pages_printed / total_pages which are fresher.
              auto copy_if_reported = [&](core::FieldValue& dst,
                                          const core::FieldValue& src) {
                if (src.state == core::FieldState::Reported &&
                    dst.state != core::FieldState::Reported)
                  dst = src;
              };
              copy_if_reported(metadata.copies,         cached->second.copies);
              copy_if_reported(metadata.duplex,         cached->second.duplex);
              copy_if_reported(metadata.color,          cached->second.color);
              copy_if_reported(metadata.paper_size,     cached->second.paper_size);
              copy_if_reported(metadata.orientation,    cached->second.orientation);
              copy_if_reported(metadata.n_up,           cached->second.n_up);
              copy_if_reported(metadata.collate,        cached->second.collate);
              copy_if_reported(metadata.print_quality,  cached->second.print_quality);
              copy_if_reported(metadata.media_type,     cached->second.media_type);
              copy_if_reported(metadata.page_range,      cached->second.page_range);
              if (!cached->second.dm_fields_hex.empty())
                metadata.dm_fields_hex = cached->second.dm_fields_hex;

              log(LogLevel::Debug, "getjob",
                  "Job " + std::to_string(job_notification.job_id) +
                  ": recovered " + std::to_string(
                    (metadata.copies.state == core::FieldState::Reported ? 1 : 0) +
                    (metadata.duplex.state == core::FieldState::Reported ? 1 : 0) +
                    (metadata.color.state  == core::FieldState::Reported ? 1 : 0)) +
                  " field(s) from metadata cache");
            }
          }

          const core::JobSnapshot stub_job = make_notification_only_snapshot(
            configured_printer_name, job_notification, known_snapshots);
          emit_snapshot(stub_job, std::move(metadata));
        }
      }

      if (info.get() != nullptr && (info.get()->Flags & PRINTER_NOTIFY_INFO_DISCARDED) != 0)
      {
        log(LogLevel::Warn, "notify",
            "PRINTER_NOTIFY_INFO_DISCARDED flag set — requesting full refresh");
        PRINTER_NOTIFY_OPTIONS refresh_options{};
        refresh_options.Version = 2;
        refresh_options.Flags = PRINTER_NOTIFY_OPTIONS_REFRESH;
        PPRINTER_NOTIFY_INFO refresh_raw_info = nullptr;
        if (!FindNextPrinterChangeNotification(
              notification.get(),
              nullptr,
              &refresh_options,
              reinterpret_cast<LPVOID*>(&refresh_raw_info)))
        {
          throw_last_error("FindNextPrinterChangeNotification refresh");
        }
        NotifyInfoHandle refresh_info(refresh_raw_info);
        const std::vector<JobNotificationSnapshot> refresh_notifications =
          parse_job_notifications(refresh_info.get(), change_flags);
        for (const JobNotificationSnapshot& job_notification : refresh_notifications)
        {
          if (const std::optional<JobSnapshotWithMetadata> current =
                get_job_snapshot_by_id(printer.get(), job_notification.job_id);
              current.has_value())
          {
            emit_snapshot(current->job, current->metadata);
          }
          else
          {
            auto refresh_meta = make_metadata_stub(job_notification.job_id, job_notification);
            // Recover from metadata cache (same logic as the main path above).
            if (auto cached = known_metadata.find(job_notification.job_id);
                cached != known_metadata.end())
            {
              if (refresh_meta.copies.state != core::FieldState::Reported && cached->second.copies.state == core::FieldState::Reported) refresh_meta.copies = cached->second.copies;
              if (refresh_meta.duplex.state != core::FieldState::Reported && cached->second.duplex.state == core::FieldState::Reported) refresh_meta.duplex = cached->second.duplex;
              if (refresh_meta.color.state  != core::FieldState::Reported && cached->second.color.state  == core::FieldState::Reported) refresh_meta.color  = cached->second.color;
              if (refresh_meta.paper_size.state != core::FieldState::Reported && cached->second.paper_size.state == core::FieldState::Reported) refresh_meta.paper_size = cached->second.paper_size;
              if (refresh_meta.orientation.state != core::FieldState::Reported && cached->second.orientation.state == core::FieldState::Reported) refresh_meta.orientation = cached->second.orientation;
              if (refresh_meta.n_up.state     != core::FieldState::Reported && cached->second.n_up.state     == core::FieldState::Reported) refresh_meta.n_up     = cached->second.n_up;
              if (refresh_meta.collate.state  != core::FieldState::Reported && cached->second.collate.state  == core::FieldState::Reported) refresh_meta.collate  = cached->second.collate;
              if (refresh_meta.print_quality.state != core::FieldState::Reported && cached->second.print_quality.state == core::FieldState::Reported) refresh_meta.print_quality = cached->second.print_quality;
              if (refresh_meta.media_type.state != core::FieldState::Reported && cached->second.media_type.state == core::FieldState::Reported) refresh_meta.media_type = cached->second.media_type;
              if (refresh_meta.page_range.state  != core::FieldState::Reported && cached->second.page_range.state  == core::FieldState::Reported) refresh_meta.page_range  = cached->second.page_range;
              if (!cached->second.dm_fields_hex.empty()) refresh_meta.dm_fields_hex = cached->second.dm_fields_hex;
            }
            const core::JobSnapshot stub_job = make_notification_only_snapshot(
              configured_printer_name, job_notification, known_snapshots);
            emit_snapshot(stub_job, std::move(refresh_meta));
          }
        }
      }

      if (has_job_change_flags(change_flags))
      {
        emit_changed_jobs();
      }

      // Periodic stall check after processing notifications.
      const auto now = std::chrono::steady_clock::now();
      if (now - last_stall_check > std::chrono::seconds{5})
      {
        tick_stall_detection();
        last_stall_check = now;
      }

      // Record event processing duration for performance monitoring.
      const auto processing_end = std::chrono::steady_clock::now();
      const double processing_ms =
        std::chrono::duration<double, std::milli>(processing_end - processing_start).count();
      perf_monitor_.set_last_event_processing_ms(processing_ms);
    }
#else
    throw std::runtime_error("Win32 print watching is only supported on Windows");
#endif
  }
  catch (const std::exception& error)
  {
    set_last_error(error.what());
    if (config_.log_callback)
    {
      config_.log_callback(LogLevel::Error, "init",
        "Watcher error: " + std::string(error.what()));
    }
  }
  catch (...)
  {
    set_last_error("Unknown print watcher error");
    if (config_.log_callback)
    {
      config_.log_callback(LogLevel::Error, "init", "Watcher error: unknown exception");
    }
  }

  if (config_.log_callback)
  {
    std::string exit_msg = "Watcher thread exiting (stop_requested=";
    exit_msg += (stop_requested_ ? "yes" : "no");
    const std::string err = last_error();
    if (!err.empty())
    {
      exit_msg += ", error=" + err;
    }
    exit_msg += ")";
    config_.log_callback(LogLevel::Info, "init", exit_msg);
  }

  running_ = false;
}

void PrintWatcher::set_last_error(std::string error)
{
  std::scoped_lock lock(error_mutex_);
  last_error_ = std::move(error);
}

void PrintWatcher::tick_stall_detection()
{
  if (config_.stall_timeout.count() <= 0)
  {
    return;
  }

  const auto now = std::chrono::steady_clock::now();

  std::scoped_lock lock(summaries_mutex_);
  for (auto& [job_id, state] : job_states_)
  {
    // Only check active (non-terminal) jobs.
    if (is_terminal_phase(state.current_phase))
    {
      continue;
    }
    if (state.current_phase == core::JobLifecyclePhase::Stalled)
    {
      continue;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - state.last_progress_time);
    if (elapsed >= config_.stall_timeout && state.current_phase != core::JobLifecyclePhase::Unknown)
    {
      if (config_.log_callback)
      {
        config_.log_callback(LogLevel::Warn, "stall",
          "Job " + std::to_string(job_id) + ": stalled in phase \"" +
          std::string(core::to_string(state.current_phase)) + "\" after " +
          std::to_string(elapsed.count()) + "s (timeout=" +
          std::to_string(config_.stall_timeout.count()) + "s)");
      }

      const int pages_printed = state.summary.events.empty()
        ? 0
        : state.summary.events.back().pages_printed;
      const int total_pages = state.summary.events.empty()
        ? -1
        : state.summary.events.back().total_pages;

      core::JobLifecycleEvent event = make_lifecycle_event(
        job_id,
        state.next_sequence,
        core::JobLifecyclePhase::Stalled,
        state.summary.last_status,
        pages_printed,
        total_pages);

      if (state.last_event_time.time_since_epoch().count() > 0)
      {
        event.delta_ms_since_prev = std::chrono::duration_cast<std::chrono::milliseconds>(
          now - state.last_event_time).count();
      }
      if (state.phase_entry_time.time_since_epoch().count() > 0)
      {
        event.cumulative_phase_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          now - state.phase_entry_time).count();
      }

      state.summary.events.push_back(event);
      state.summary.final_phase = core::JobLifecyclePhase::Stalled;

      // Finalize the old phase's dwell time before transitioning.
      const auto old_phase_name = std::string(core::to_string(state.current_phase));
      const auto old_dwell = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - state.phase_entry_time).count() + state.cumulative_phase_ms;
      bool found_existing = false;
      for (auto& entry : state.summary.phase_dwells)
      {
        if (entry.phase_name == old_phase_name)
        {
          entry.duration_ms = old_dwell;
          found_existing = true;
          break;
        }
      }
      if (!found_existing && !old_phase_name.empty())
      {
        state.summary.phase_dwells.push_back(core::PhaseDwellEntry{
          .phase_name = old_phase_name,
          .duration_ms = old_dwell,
        });
      }

      state.current_phase = core::JobLifecyclePhase::Stalled;
      state.phase_entry_time = now;      // start timing the stalled phase
      state.cumulative_phase_ms = 0;
      state.last_event_time = now;
      state.next_sequence++;

      if (lifecycle_callback_)
      {
        lifecycle_callback_(event);
      }
    }
  }
}

}  // namespace wpce::watcher
