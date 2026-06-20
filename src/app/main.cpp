#include "collector/metadata_collector.h"
#include "common/build_info.h"
#include "core/data_contracts.h"
#include "storage/job_snapshot_writer.h"
#include "storage/json_serializer.h"
#include "storage/xml_serializer.h"
#include "watcher/print_watcher.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{

std::atomic_bool g_stop_requested{false};

// ── Phase display helpers ───────────────────────────────────────────────

const char* phase_icon(wpce::core::JobLifecyclePhase phase) noexcept
{
  switch (phase)
  {
    case wpce::core::JobLifecyclePhase::Accepted:
      return "\xe2\x9e\xa4";       // ➤
    case wpce::core::JobLifecyclePhase::Spooling:
      return "\xe2\x8f\xb3";       // ⏳
    case wpce::core::JobLifecyclePhase::Printing:
      return "\xf0\x9f\x96\xa8";   // 🖨
    case wpce::core::JobLifecyclePhase::Completed:
      return "\xe2\x9c\x93";       // ✓
    case wpce::core::JobLifecyclePhase::Failed:
      return "\xe2\x9c\x97";       // ✗
    case wpce::core::JobLifecyclePhase::Cancelled:
      return "\xe2\x9c\x82";       // ✂
    case wpce::core::JobLifecyclePhase::Stalled:
      return "\xe2\x9a\xa0";       // ⚠
    default:
      return "?";
  }
}

const char* phase_display_name(wpce::core::JobLifecyclePhase phase) noexcept
{
  switch (phase)
  {
    case wpce::core::JobLifecyclePhase::Accepted:
      return "accepted";
    case wpce::core::JobLifecyclePhase::Spooling:
      return "spooling";
    case wpce::core::JobLifecyclePhase::Printing:
      return "printing";
    case wpce::core::JobLifecyclePhase::Completed:
      return "completed";
    case wpce::core::JobLifecyclePhase::Failed:
      return "failed";
    case wpce::core::JobLifecyclePhase::Cancelled:
      return "cancelled";
    case wpce::core::JobLifecyclePhase::Stalled:
      return "stalled";
    default:
      return "unknown";
  }
}

// ── Helpers ─────────────────────────────────────────────────────────────

wpce::core::JobSnapshot make_sample_job()
{
  return wpce::core::JobSnapshot{
    .schema_version = 1,
    .job_id = 1,
    .user = "DOMAIN\\user",
    .document_name = "sample.pdf",
    .printer_name = "Microsoft Print to PDF",
    .submit_time_utc = "2026-06-19T00:00:00Z",
    .status = "spooling",
    .source = "winspool",
  };
}

wpce::core::MetadataSnapshot make_sample_metadata()
{
  return wpce::core::MetadataSnapshot{
    .schema_version = 1,
    .job_id = 1,
    .total_pages = {"3", wpce::core::FieldState::Reported},
    .pages_printed = {"0", wpce::core::FieldState::Reported},
    .copies = {"1", wpce::core::FieldState::Reported},
    .duplex = {"simplex", wpce::core::FieldState::Reported},
    .color = {"color", wpce::core::FieldState::Reported},
    .paper_size = {"A4", wpce::core::FieldState::Reported},
    .orientation = {"portrait", wpce::core::FieldState::Reported},
    .n_up = {"1-up (normal)", wpce::core::FieldState::Reported},
    .collate = {"uncollated", wpce::core::FieldState::Reported},
    .print_quality = {"high", wpce::core::FieldState::Reported},
    .dm_fields_hex = "0x0007e35f",
  };
}

void print_usage()
{
  std::cout << wpce::common::product_name() << " " << wpce::common::product_version() << '\n'
            << wpce::common::product_description() << "\n\n"
            << "Usage:\n"
            << "  wpce --version\n"
            << "  wpce --sample-json\n"
            << "  wpce --sample-xml\n"
            << "  wpce --write-sample-job <output-dir>\n"
            << "  wpce --list-jobs <printer-name>\n"
            << "  wpce --watch-printer <printer-name> [output-dir] [--stall-timeout <seconds>] [--perf-log <file>]\n";
}

#ifdef _WIN32
void configure_console_encoding()
{
  SetConsoleCP(CP_UTF8);
  SetConsoleOutputCP(CP_UTF8);
}

BOOL WINAPI handle_console_control(const DWORD control_type)
{
  switch (control_type)
  {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
      g_stop_requested = true;
      return TRUE;
    default:
      return FALSE;
  }
}

std::wstring widen_utf8(const std::string_view value)
{
  if (value.empty())
  {
    return {};
  }

  const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0)
  {
    return {};
  }

  std::wstring result(static_cast<std::size_t>(size), L'\0');
  const int written =
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
  if (written <= 0)
  {
    return {};
  }

  return result;
}
#else
void configure_console_encoding() {}

std::wstring widen_utf8(const std::string_view value)
{
  return std::wstring(value.begin(), value.end());
}
#endif

// ── Commands ─────────────────────────────────────────────────────────────

int list_jobs(const std::string_view printer_name)
{
  try
  {
    const std::vector<wpce::watcher::JobSnapshotWithMetadata> snapshots =
      wpce::watcher::enumerate_printer_jobs(widen_utf8(printer_name));
    if (snapshots.empty())
    {
      std::cout << "No jobs in queue for printer \"" << printer_name << "\".\n";
      return 0;
    }
    for (const wpce::watcher::JobSnapshotWithMetadata& entry : snapshots)
    {
      std::cout << wpce::storage::to_json(entry.job);
      std::cout << wpce::storage::to_json(entry.metadata);
    }
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << "Failed to list print jobs: " << error.what() << '\n';
    return 2;
  }
}

int write_sample_job(const std::filesystem::path& output_directory)
{
  try
  {
    wpce::storage::AsyncJobSnapshotWriter writer(
      wpce::storage::JobSnapshotWriterConfig{.output_directory = output_directory});
    writer.start();
    if (!writer.enqueue(make_sample_job(), make_sample_metadata()))
    {
      std::cerr << "Failed to queue sample job snapshot\n";
      return 2;
    }
    writer.stop();

    const std::string error = writer.last_error();
    if (!error.empty())
    {
      std::cerr << "Job snapshot writer stopped with an error: " << error << '\n';
      return 2;
    }

    std::cout << "Wrote sample job snapshot to " << wpce::storage::job_snapshot_path({.output_directory = output_directory})
              << '\n';
    std::cout << "Wrote sample metadata snapshot to " << wpce::storage::metadata_snapshot_path({.output_directory = output_directory})
              << '\n';
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << "Failed to write sample job snapshot: " << error.what() << '\n';
    return 2;
  }
}

void print_lifecycle_event(const wpce::core::JobLifecycleEvent& event)
{
  // Format:  [JobId N] phase_name  detail
  const char* icon = phase_icon(event.phase);
  const char* name = phase_display_name(event.phase);

  std::cout << icon << " [Job " << event.job_id << "] " << name;

  // Add progress info if available.
  if (event.total_pages > 0)
  {
    const int pct = (event.pages_printed * 100) / event.total_pages;
    std::cout << "  page " << event.pages_printed << '/' << event.total_pages
              << " (" << pct << "%)";
  }
  else if (event.phase == wpce::core::JobLifecyclePhase::Printing)
  {
    std::cout << "  page " << event.pages_printed;
  }

  std::cout << '\n' << std::flush;
}

void print_lifecycle_summary_line(const wpce::core::JobLifecycleSummary& summary)
{
  const char* icon = phase_icon(summary.final_phase);
  const char* final_name = phase_display_name(summary.final_phase);

  std::cout << "  " << icon << " Job " << summary.job_id
            << ": \"" << summary.document_name << "\""
            << " by " << summary.user
            << " -> " << final_name;

  if (summary.progress_percent >= 0 && summary.final_phase == wpce::core::JobLifecyclePhase::Completed)
  {
    std::cout << " (" << summary.progress_percent << "% printed)";
  }

  std::cout << " [" << summary.events.size() << " event(s)]\n";
}

int watch_printer(const std::string_view printer_name,
                  const std::filesystem::path& output_directory,
                  const std::chrono::seconds stall_timeout,
                  const std::string& perf_log_path = {})
{
  try
  {
    g_stop_requested = false;
#ifdef _WIN32
    SetConsoleCtrlHandler(handle_console_control, TRUE);
#endif

    std::mutex output_mutex;
    std::uint64_t queued_snapshots = 0;
    std::uint64_t dropped_snapshots = 0;
    std::uint32_t highest_observed_job_id = 0;
    std::unordered_map<std::uint32_t, wpce::core::JobLifecyclePhase> job_phase_cache;

    // Temporary holder: the job callback fires first, then the metadata
    // callback fires immediately after from the same emit_snapshot() call.
    wpce::core::JobSnapshot pending_job;
    std::mutex pending_mutex;

    wpce::storage::AsyncJobSnapshotWriter writer(
      wpce::storage::JobSnapshotWriterConfig{.output_directory = output_directory});
    writer.start();

    // ── Job callback: persist JSON to stdout ─────────────────────────────

    auto job_callback = [&](const wpce::core::JobSnapshot& snapshot) {
      std::scoped_lock lock(output_mutex);
      if (highest_observed_job_id != 0 && snapshot.job_id > highest_observed_job_id + 1)
      {
        std::cerr << "? [Gap] Missing JobId range " << (highest_observed_job_id + 1)
                  << '-' << (snapshot.job_id - 1) << '\n';
      }
      if (snapshot.job_id > highest_observed_job_id)
      {
        highest_observed_job_id = snapshot.job_id;
      }

      // Emit full JSON snapshot to stdout for piping / file capture.
      std::cout << wpce::storage::to_json(snapshot) << std::flush;

      {
        std::scoped_lock pending_lock(pending_mutex);
        pending_job = snapshot;
      }
    };

    // ── Metadata callback: persist to disk ──────────────────────────────

    auto metadata_callback = [&](const wpce::core::MetadataSnapshot& metadata) {
      std::scoped_lock lock(output_mutex);
      std::cout << wpce::storage::to_json(metadata) << std::flush;

      wpce::core::JobSnapshot job;
      {
        std::scoped_lock pending_lock(pending_mutex);
        job = pending_job;
      }

      if (writer.enqueue(std::move(job), metadata))
      {
        ++queued_snapshots;
      }
      else
      {
        ++dropped_snapshots;
        std::cerr << "! [Drop] Writer queue full/unavailable\n";
      }
    };

    // ── Lifecycle callback: structured progress display ─────────────────

    auto lifecycle_callback = [&](const wpce::core::JobLifecycleEvent& event) {
      std::scoped_lock lock(output_mutex);
      print_lifecycle_event(event);
    };

    // ── Build watcher config ────────────────────────────────────────────

    wpce::watcher::WatcherConfig watcher_config{
      .printer_name = widen_utf8(printer_name),
      .wait_timeout = std::chrono::milliseconds{250},
      .stall_timeout = stall_timeout,
    };

    wpce::watcher::PrintWatcher watcher(
      std::move(watcher_config),
      std::move(job_callback),
      std::move(metadata_callback),
      std::move(lifecycle_callback));

    watcher.start();

    std::cout << "=== Watching \"" << printer_name << "\" ===\n";
    std::cout << "Output: " << output_directory << '\n';
    if (stall_timeout.count() > 0)
    {
      std::cout << "Stall timeout: " << stall_timeout.count() << "s\n";
    }
    std::cout << "Press Ctrl+C to stop.\n\n";

    // ── Perf log setup ──────────────────────────────────────────────────
    std::ofstream perf_log;
    if (!perf_log_path.empty())
    {
      perf_log.open(perf_log_path, std::ios::out | std::ios::trunc);
      if (perf_log)
      {
        perf_log << "timestamp_utc,cpu_pct,working_set_mb,handle_count,thread_count,"
                    "snapshot_queue,page_queue,watcher_idle_ms,event_processing_ms\n";
      }
    }
    auto last_perf_sample = std::chrono::steady_clock::now();

    while (!g_stop_requested && watcher.running())
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));

      // Periodic performance sampling (~ every 5 seconds).
      const auto now = std::chrono::steady_clock::now();
      if (perf_log.is_open() && now - last_perf_sample > std::chrono::seconds{5})
      {
        auto& monitor = watcher.perf_monitor();
        monitor.set_snapshot_writer_queue_depth(writer.queue_depth());
        const wpce::core::PerfSample sample = monitor.sample();
        perf_log << sample.timestamp_utc << ','
                 << sample.cpu_percent << ','
                 << (sample.working_set_bytes / (1024.0 * 1024.0)) << ','
                 << sample.handle_count << ','
                 << sample.thread_count << ','
                 << sample.snapshot_writer_queue_depth << ','
                 << sample.page_writer_queue_depth << ','
                 << sample.watcher_idle_ms << ','
                 << sample.last_event_processing_ms << '\n';
        perf_log.flush();
        last_perf_sample = now;
      }
    }
    watcher.stop();
    writer.stop();

    // ── Error reporting ─────────────────────────────────────────────────

    const std::string error = watcher.last_error();
    if (!error.empty())
    {
      std::cerr << "\n! Print watcher stopped with an error: " << error << '\n';
      return 2;
    }

    const std::string writer_error = writer.last_error();
    if (!writer_error.empty())
    {
      std::cerr << "! Job snapshot writer stopped with an error: " << writer_error << '\n';
      return 2;
    }

    // ── Per-session summary ─────────────────────────────────────────────

    const std::vector<wpce::core::JobLifecycleSummary> summaries = watcher.lifecycle_summaries();

    std::cout << "\n=== Session Summary ===\n";
    std::cout << "Observed " << summaries.size() << " job(s) across "
              << queued_snapshots << " snapshot(s).";

    if (dropped_snapshots > 0)
    {
      std::cout << " Dropped " << dropped_snapshots << ".";
    }
    std::cout << "\n\n";

    if (!summaries.empty())
    {
      for (const wpce::core::JobLifecycleSummary& summary : summaries)
      {
        print_lifecycle_summary_line(summary);
      }
      std::cout << '\n';
    }

    // ── Persist lifecycle summaries ─────────────────────────────────────

    if (!summaries.empty())
    {
      try
      {
        wpce::storage::write_lifecycle_summaries_atomic(
          summaries,
          wpce::storage::JobSnapshotWriterConfig{.output_directory = output_directory});
        std::cout << "Lifecycle summaries written to "
                  << wpce::storage::lifecycle_summary_path({.output_directory = output_directory})
                  << '\n';
      }
      catch (const std::exception& persist_error)
      {
        std::cerr << "! Failed to write lifecycle summaries: " << persist_error.what() << '\n';
      }
    }

    std::cout << "\nQueued " << queued_snapshots << " snapshot(s). Dropped " << dropped_snapshots << ".\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << "Failed to watch printer: " << error.what() << '\n';
    return 2;
  }
}

}  // namespace

// ── main ──────────────────────────────────────────────────────────────────

int main(const int argc, char* argv[])
{
  configure_console_encoding();

  const std::string_view command = argc > 1 ? argv[1] : "";

  if (command == "--version")
  {
    std::cout << wpce::common::product_version() << '\n';
    return 0;
  }

  if (command == "--sample-json")
  {
    std::cout << wpce::storage::to_json(make_sample_job());
    return 0;
  }

  if (command == "--sample-xml")
  {
    std::cout << wpce::storage::to_xml(make_sample_job());
    return 0;
  }

  if (command == "--write-sample-job")
  {
    if (argc < 3)
    {
      std::cerr << "--write-sample-job requires an output directory\n";
      return 2;
    }
    return write_sample_job(argv[2]);
  }

  if (command == "--list-jobs")
  {
    if (argc < 3)
    {
      std::cerr << "--list-jobs requires a printer name\n";
      return 2;
    }
    return list_jobs(argv[2]);
  }

  if (command == "--watch-printer")
  {
    if (argc < 3)
    {
      std::cerr << "--watch-printer requires a printer name\n";
      return 2;
    }

    std::filesystem::path output_directory = argc > 3 && argv[3][0] != '-' ? argv[3] : "wpce-output";
    std::chrono::seconds stall_timeout{0};
    std::string perf_log_path;

    // Parse optional flags.
    for (int i = 3; i < argc; ++i)
    {
      const std::string_view arg = argv[i];
      if (arg == "--stall-timeout" && i + 1 < argc)
      {
        try { stall_timeout = std::chrono::seconds{std::stoll(argv[i + 1])}; } catch (...) {}
      }
      if (arg == "--perf-log" && i + 1 < argc)
      {
        perf_log_path = argv[i + 1];
      }
    }

    return watch_printer(argv[2], output_directory, stall_timeout, perf_log_path);
  }

  print_usage();
  return 0;
}
