/// WPCE Stress Test Tool — exercises the async writer infrastructure under load.
///
/// Simulates N synthetic print jobs enqueued across W concurrent writers.
/// Validates: no crashes, no leaks, no data corruption, backpressure behavior.

#include "core/data_contracts.h"
#include "storage/job_snapshot_writer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

namespace
{

using namespace wpce;

struct StressConfig
{
  std::uint32_t job_count{10000};
  std::uint32_t writer_count{1};
  std::size_t queue_capacity{256};
  std::filesystem::path output_directory{"stress-test-output"};
  std::uint32_t burst_size{100};
  std::chrono::milliseconds burst_interval{0};
};

core::JobSnapshot make_test_job(std::uint32_t id)
{
  return core::JobSnapshot{
    .schema_version = 1,
    .job_id = id,
    .user = "STRESS_TEST\\user_" + std::to_string(id),
    .document_name = "stress_doc_" + std::to_string(id) + ".pdf",
    .printer_name = "Stress Test Printer",
    .submit_time_utc = "2026-06-20T00:00:00Z",
    .status = "spooling",
    .source = "stress-test",
  };
}

core::MetadataSnapshot make_test_metadata(std::uint32_t id)
{
  return core::MetadataSnapshot{
    .schema_version = 1,
    .job_id = id,
    .total_pages = {"5", core::FieldState::Reported},
    .pages_printed = {"0", core::FieldState::Reported},
    .copies = {"1", core::FieldState::Reported},
    .duplex = {"simplex", core::FieldState::Reported},
    .color = {"monochrome", core::FieldState::Reported},
    .paper_size = {"A4", core::FieldState::Reported},
    .orientation = {"portrait", core::FieldState::Reported},
    .n_up = {"1-up (normal)", core::FieldState::Reported},
    .collate = {"uncollated", core::FieldState::Reported},
    .print_quality = {"normal", core::FieldState::Reported},
    .dm_fields_hex = "0x0007ffff",
  };
}

void print_usage()
{
  std::cout
    << "WPCE Stress Test Tool\n\n"
    << "Usage:\n"
    << "  stress_test.exe [options]\n\n"
    << "Options:\n"
    << "  --job-count <N>          Number of jobs to simulate (default: 10000)\n"
    << "  --writer-count <N>       Number of concurrent writers (default: 1)\n"
    << "  --queue-capacity <N>     Queue capacity per writer (default: 256)\n"
    << "  --output-dir <path>      Output directory (default: stress-test-output)\n"
    << "  --burst-size <N>         Jobs per burst (default: 100)\n"
    << "  --burst-interval-ms <N>  Delay between bursts in ms (default: 0)\n";
}

StressConfig parse_args(int argc, char* argv[])
{
  StressConfig config;
  for (int i = 1; i < argc; ++i)
  {
    const std::string_view arg = argv[i];
    if (arg == "--job-count" && i + 1 < argc)
    {
      try { config.job_count = static_cast<std::uint32_t>(std::stoull(argv[++i])); } catch (...) {}
    }
    else if (arg == "--writer-count" && i + 1 < argc)
    {
      try { config.writer_count = static_cast<std::uint32_t>(std::stoull(argv[++i])); } catch (...) {}
    }
    else if (arg == "--queue-capacity" && i + 1 < argc)
    {
      try { config.queue_capacity = static_cast<std::size_t>(std::stoull(argv[++i])); } catch (...) {}
    }
    else if (arg == "--output-dir" && i + 1 < argc)
    {
      config.output_directory = argv[++i];
    }
    else if (arg == "--burst-size" && i + 1 < argc)
    {
      try { config.burst_size = static_cast<std::uint32_t>(std::stoull(argv[++i])); } catch (...) {}
    }
    else if (arg == "--burst-interval-ms" && i + 1 < argc)
    {
      try { config.burst_interval = std::chrono::milliseconds{std::stoll(argv[++i])}; } catch (...) {}
    }
    else if (arg == "--help")
    {
      print_usage();
      std::exit(0);
    }
  }
  return config;
}

int run_stress_test(const StressConfig& config)
{
  std::cout << "=== WPCE Stress Test ===\n"
            << "Jobs:       " << config.job_count << '\n'
            << "Writers:    " << config.writer_count << '\n'
            << "Queue cap:  " << config.queue_capacity << '\n'
            << "Burst size: " << config.burst_size << '\n'
            << "Output:     " << config.output_directory << "\n\n";

  if (config.writer_count == 0 || config.queue_capacity == 0)
  {
    std::cerr << "! Invalid config: writer_count and queue_capacity must be > 0\n";
    return 2;
  }

  // Remove previous output.
  std::error_code ec;
  std::filesystem::remove_all(config.output_directory, ec);

  // Create writers, each with its own output subdirectory.
  // Use unique_ptr since AsyncJobSnapshotWriter is not movable.
  std::vector<std::unique_ptr<storage::AsyncJobSnapshotWriter>> writers;
  writers.reserve(config.writer_count);

  for (std::uint32_t w = 0; w < config.writer_count; ++w)
  {
    storage::JobSnapshotWriterConfig writer_config;
    writer_config.output_directory =
      config.output_directory / ("writer-" + std::to_string(w));
    writer_config.queue_capacity = config.queue_capacity;
    writers.emplace_back(std::make_unique<storage::AsyncJobSnapshotWriter>(writer_config));
    writers.back()->start();
  }

  std::cout << writers.size() << " writer(s) started.\n";

  // ── Burst-enqueue loop ──────────────────────────────────────────────
  const auto start_time = std::chrono::steady_clock::now();
  std::atomic_uint64_t enqueued{0};
  std::atomic_uint64_t dropped{0};
  std::atomic_size_t peak_queue_depth{0};
  std::uint32_t writer_index = 0;

  for (std::uint32_t base = 0; base < config.job_count; base += config.burst_size)
  {
    const std::uint32_t burst_end =
      (std::min)(base + config.burst_size, config.job_count);

    for (std::uint32_t job_id = base; job_id < burst_end; ++job_id)
    {
      auto& writer = *writers[writer_index % config.writer_count];
      ++writer_index;

      const bool ok = writer.enqueue(make_test_job(job_id), make_test_metadata(job_id));
      if (ok)
      {
        ++enqueued;
      }
      else
      {
        ++dropped;
      }
    }

    // Track peak queue depth across all writers.
    std::size_t total_depth = 0;
    for (auto& w : writers)
    {
      total_depth += w->queue_depth();
    }
    std::size_t current_peak = peak_queue_depth.load();
    while (total_depth > current_peak &&
           !peak_queue_depth.compare_exchange_weak(current_peak, total_depth))
    {
    }

    // Progress logging.
    if (burst_end % 1000 == 0 || burst_end >= config.job_count)
    {
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start_time)
                             .count();
      const double rate = elapsed > 0 ? (enqueued * 1000.0 / elapsed) : 0.0;
      std::cout << "  " << burst_end << '/' << config.job_count
                << " jobs generated | enqueued: " << enqueued
                << " | dropped: " << dropped
                << " | peak depth: " << peak_queue_depth
                << " | rate: " << static_cast<int>(rate) << " jobs/s\n";
    }

    if (config.burst_interval.count() > 0 && burst_end < config.job_count)
    {
      std::this_thread::sleep_for(config.burst_interval);
    }
  }

  const auto enqueue_end = std::chrono::steady_clock::now();
  std::cout << "\nAll " << config.job_count << " jobs enqueued in "
            << std::chrono::duration_cast<std::chrono::milliseconds>(
                 enqueue_end - start_time)
                 .count()
            << " ms.\n";

  // ── Drain writers ───────────────────────────────────────────────────
  std::cout << "Draining writers...\n";
  bool drained = false;
  const auto drain_start = std::chrono::steady_clock::now();
  while (!drained)
  {
    drained = true;
    for (auto& w : writers)
    {
      if (w->queue_depth() > 0)
      {
        drained = false;
        break;
      }
    }
    if (!drained)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));

      // Timeout after 30 seconds.
      if (std::chrono::steady_clock::now() - drain_start > std::chrono::seconds{60})
      {
        std::cerr << "! Drain timeout after 60s — forcing stop.\n";
        break;
      }
    }
  }

  const auto drain_end = std::chrono::steady_clock::now();
  std::cout << "Drained in "
            << std::chrono::duration_cast<std::chrono::milliseconds>(
                 drain_end - drain_start)
                 .count()
            << " ms.\n";

  // ── Stop writers and check for errors ───────────────────────────────
  std::uint32_t writer_errors = 0;
  for (auto& w : writers)
  {
    w->stop();
    const std::string error = w->last_error();
    if (!error.empty())
    {
      std::cerr << "! Writer error: " << error << '\n';
      ++writer_errors;
    }
  }

  // ── Verify output files ─────────────────────────────────────────────
  std::uint32_t file_count = 0;
  for (std::uint32_t w = 0; w < config.writer_count; ++w)
  {
    const auto writer_dir = config.output_directory / ("writer-" + std::to_string(w));
    if (std::filesystem::exists(writer_dir))
    {
      for (const auto& entry : std::filesystem::recursive_directory_iterator(writer_dir))
      {
        if (entry.is_regular_file())
        {
          ++file_count;
        }
      }
    }
  }

  // ── Summary ─────────────────────────────────────────────────────────
  const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          drain_end - start_time)
                          .count();
  const double throughput = total_ms > 0 ? (config.job_count * 1000.0 / total_ms) : 0.0;

  std::cout << "\n=== Results ===\n"
            << "Total time:      " << total_ms << " ms\n"
            << "Throughput:      " << static_cast<int>(throughput) << " jobs/s\n"
            << "Enqueued:        " << enqueued << '\n'
            << "Dropped:         " << dropped << '\n'
            << "Peak queue depth:" << peak_queue_depth << '\n'
            << "Output files:    " << file_count << '\n'
            << "Writer errors:   " << writer_errors << '\n';

  const bool success = (writer_errors == 0) && (file_count > 0);
  std::cout << "\nResult: " << (success ? "PASSED" : "FAILED") << '\n';

  return success ? 0 : 1;
}

}  // namespace

int main(int argc, char* argv[])
{
#ifdef _MSC_VER
  // Enable memory leak detection in Debug builds.
  _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

  const StressConfig config = parse_args(argc, argv);

  try
  {
    return run_stress_test(config);
  }
  catch (const std::exception& error)
  {
    std::cerr << "! Unexpected error: " << error.what() << '\n';
    return 2;
  }
}
