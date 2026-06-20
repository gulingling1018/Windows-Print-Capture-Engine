#include "collector/metadata_collector.h"
#include "core/data_contracts.h"
#include "storage/job_snapshot_writer.h"
#include "storage/json_serializer.h"
#include "storage/xml_serializer.h"
#include "watcher/print_watcher.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{

void expect_contains(const std::string& text, const std::string& expected)
{
  EXPECT_NE(std::string::npos, text.find(expected))
    << "Expected text to contain \"" << expected << "\"";
}

std::filesystem::path make_test_directory(const std::string& name)
{
  const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
  std::ostringstream path;
  path << "wpce-" << name << '-' << tick;
  return std::filesystem::temp_directory_path() / path.str();
}

std::string read_file(const std::filesystem::path& path)
{
  std::ifstream input(path, std::ios::binary);
  std::ostringstream content;
  content << input.rdbuf();
  return content.str();
}

}  // namespace

// ── Serialization tests ──────────────────────────────────────────────────

TEST(Serialization, JobSnapshot)
{
  const wpce::core::JobSnapshot snapshot{
    .schema_version = 1,
    .job_id = 42,
    .user = "DOMAIN\\alice",
    .document_name = "report \"Q2\".pdf",
    .printer_name = "Microsoft Print to PDF",
    .submit_time_utc = "2026-06-19T11:30:00Z",
    .status = "spooling",
    .source = "winspool",
  };

  const std::string json = wpce::storage::to_json(snapshot);
  expect_contains(json, "\"schema_version\": 1");
  expect_contains(json, "\"job_id\": 42");
  expect_contains(json, "report \\\"Q2\\\".pdf");
  expect_contains(json, "DOMAIN\\\\alice");

  const std::string xml = wpce::storage::to_xml(snapshot);
  expect_contains(xml, "<job_id>42</job_id>");
  expect_contains(xml, "report &quot;Q2&quot;.pdf");
}

TEST(Serialization, MetadataState)
{
  wpce::core::MetadataSnapshot snapshot;
  snapshot.job_id = 42;
  snapshot.total_pages = {.value = "5", .state = wpce::core::FieldState::Reported};
  snapshot.pages_printed = {.value = "0", .state = wpce::core::FieldState::Reported};
  snapshot.copies = {.value = "1", .state = wpce::core::FieldState::Reported};
  snapshot.duplex = {.value = "", .state = wpce::core::FieldState::Unsupported};
  snapshot.color = {.value = "color", .state = wpce::core::FieldState::Reported};
  snapshot.paper_size = {.value = "A4", .state = wpce::core::FieldState::Reported};
  snapshot.orientation = {.value = "portrait", .state = wpce::core::FieldState::Reported};
  snapshot.n_up = {.value = "2-up", .state = wpce::core::FieldState::Reported};
  snapshot.collate = {.value = "collated", .state = wpce::core::FieldState::Reported};
  snapshot.print_quality = {.value = "600", .state = wpce::core::FieldState::Unknown};
  snapshot.dm_fields_hex = "0x0007ffff";

  const std::string json = wpce::storage::to_json(snapshot);
  expect_contains(json, "\"total_pages\": { \"value\": \"5\", \"state\": \"reported\" }");
  expect_contains(json, "\"duplex\": { \"value\": \"\", \"state\": \"unsupported\" }");
  expect_contains(json, "\"n_up\": { \"value\": \"2-up\", \"state\": \"reported\" }");
  expect_contains(json, "\"collate\": { \"value\": \"collated\", \"state\": \"reported\" }");
  expect_contains(json, "\"dm_fields_hex\": \"0x0007ffff\"");

  const std::string xml = wpce::storage::to_xml(snapshot);
  expect_contains(xml, "<duplex state=\"unsupported\"></duplex>");
}

TEST(Serialization, PageArtifactIndex)
{
  const wpce::core::PageArtifactIndex index{
    .schema_version = 1,
    .job_id = 42,
    .document_sequence = 1,
    .page_sequence = 2,
    .job_print_ticket = "job.pt.xml",
    .document_print_ticket = "document-001.pt.xml",
    .page_print_ticket = "page-002.pt.xml",
    .fixed_page = "page-002.fpage",
    .source = "xps-filter",
  };

  const std::string json = wpce::storage::to_json(index);
  expect_contains(json, "\"page_sequence\": 2");
  expect_contains(json, "\"fixed_page\": \"page-002.fpage\"");

  const std::string xml = wpce::storage::to_xml(index);
  expect_contains(xml, "<source>xps-filter</source>");
}

TEST(Serialization, LifecycleEvent)
{
  const wpce::core::JobLifecycleEvent event{
    .event_id = "42-1",
    .job_id = 42,
    .sequence = 1,
    .timestamp_utc = "2026-06-19T12:00:00Z",
    .phase = wpce::core::JobLifecyclePhase::Spooling,
    .status_flags = "spooling",
    .detail = "Job 42 phase: spooling, status: spooling",
    .pages_printed = 0,
    .total_pages = 5,
  };

  const std::string json = wpce::storage::to_json(event);
  expect_contains(json, "\"event_id\": \"42-1\"");
  expect_contains(json, "\"phase\": \"spooling\"");
  expect_contains(json, "\"total_pages\": 5");
}

TEST(Serialization, LifecycleSummary)
{
  wpce::core::JobLifecycleSummary summary{
    .schema_version = 1,
    .job_id = 99,
    .user = "DOMAIN\\test",
    .document_name = "lifecycle-test.pdf",
    .printer_name = "Microsoft Print to PDF",
    .submit_time_utc = "2026-06-19T12:00:00Z",
    .final_phase = wpce::core::JobLifecyclePhase::Completed,
    .progress_percent = 100,
    .last_status = "retained",
    .source = "winspool",
  };

  // Add a few events.
  summary.events.push_back(wpce::core::JobLifecycleEvent{
    .event_id = "99-0",
    .job_id = 99,
    .sequence = 0,
    .timestamp_utc = "2026-06-19T12:00:00Z",
    .phase = wpce::core::JobLifecyclePhase::Spooling,
    .status_flags = "spooling",
    .detail = "Job 99 phase: spooling",
    .pages_printed = 0,
    .total_pages = 1,
  });
  summary.events.push_back(wpce::core::JobLifecycleEvent{
    .event_id = "99-1",
    .job_id = 99,
    .sequence = 1,
    .timestamp_utc = "2026-06-19T12:00:01Z",
    .phase = wpce::core::JobLifecyclePhase::Printing,
    .status_flags = "printing",
    .detail = "Job 99 phase: printing",
    .pages_printed = 1,
    .total_pages = 1,
  });
  summary.events.push_back(wpce::core::JobLifecycleEvent{
    .event_id = "99-2",
    .job_id = 99,
    .sequence = 2,
    .timestamp_utc = "2026-06-19T12:00:02Z",
    .phase = wpce::core::JobLifecyclePhase::Completed,
    .status_flags = "retained",
    .detail = "Job 99 phase: completed",
    .pages_printed = 1,
    .total_pages = 1,
  });

  const std::string json = wpce::storage::to_json(summary);
  expect_contains(json, "\"job_id\": 99");
  expect_contains(json, "\"final_phase\": \"completed\"");
  expect_contains(json, "\"progress_percent\": 100");
  expect_contains(json, "\"event_id\": \"99-0\"");
  expect_contains(json, "\"event_id\": \"99-2\"");
}

// ── Watcher tests ────────────────────────────────────────────────────────

TEST(Watcher, DescribeJobStatus)
{
  EXPECT_EQ("unknown", wpce::watcher::describe_job_status(0));
  EXPECT_EQ("spooling", wpce::watcher::describe_job_status(0x00000008));
  EXPECT_EQ("printing|offline", wpce::watcher::describe_job_status(0x00000010 | 0x00000020));
  EXPECT_EQ("unknown(2147483648)", wpce::watcher::describe_job_status(0x80000000));
}

TEST(Watcher, PhaseClassification)
{
  using Phase = wpce::core::JobLifecyclePhase;

  // Terminal states.
  EXPECT_EQ(Phase::Failed, wpce::watcher::classify_phase(0x00000002, ""));     // JOB_STATUS_ERROR
  EXPECT_EQ(Phase::Failed, wpce::watcher::classify_phase(0x00000040, ""));     // JOB_STATUS_PAPEROUT
  EXPECT_EQ(Phase::Cancelled, wpce::watcher::classify_phase(0x00000100, ""));  // JOB_STATUS_DELETED
  EXPECT_EQ(Phase::Completed, wpce::watcher::classify_phase(0x00001000, ""));  // JOB_STATUS_COMPLETE
  EXPECT_EQ(Phase::Completed, wpce::watcher::classify_phase(0x00002000, ""));  // JOB_STATUS_RETAINED

  // Active states.
  EXPECT_EQ(Phase::Printing, wpce::watcher::classify_phase(0x00000010, ""));   // JOB_STATUS_PRINTING
  EXPECT_EQ(Phase::Spooling, wpce::watcher::classify_phase(0x00000008, ""));   // JOB_STATUS_SPOOLING

  // Unknown (empty flags).
  EXPECT_EQ(Phase::Unknown, wpce::watcher::classify_phase(0, ""));

  // Terminal phase check.
  EXPECT_TRUE(wpce::core::is_terminal_phase(Phase::Completed));
  EXPECT_TRUE(wpce::core::is_terminal_phase(Phase::Failed));
  EXPECT_TRUE(wpce::core::is_terminal_phase(Phase::Cancelled));
  EXPECT_FALSE(wpce::core::is_terminal_phase(Phase::Stalled));
  EXPECT_FALSE(wpce::core::is_terminal_phase(Phase::Printing));
  EXPECT_FALSE(wpce::core::is_terminal_phase(Phase::Spooling));
  EXPECT_FALSE(wpce::core::is_terminal_phase(Phase::Unknown));

  // Phase name.
  EXPECT_EQ("accepted", wpce::core::to_string(Phase::Accepted));
  EXPECT_EQ("completed", wpce::core::to_string(Phase::Completed));
  EXPECT_EQ("failed", wpce::core::to_string(Phase::Failed));
  EXPECT_EQ("cancelled", wpce::core::to_string(Phase::Cancelled));
  EXPECT_EQ("stalled", wpce::core::to_string(Phase::Stalled));
}

// ── Storage tests ────────────────────────────────────────────────────────

TEST(Storage, AtomicJobSnapshotWriter)
{
  const wpce::core::JobSnapshot snapshot{
    .schema_version = 1,
    .job_id = 42,
    .user = "DOMAIN\\alice",
    .document_name = "report.pdf",
    .printer_name = "Microsoft Print to PDF",
    .submit_time_utc = "2026-06-19T11:30:00Z",
    .status = "spooling",
    .source = "winspool",
  };

  const std::filesystem::path directory = make_test_directory("atomic-writer");
  const std::filesystem::path output_path = directory / "job.json";

  wpce::storage::write_job_snapshot_json_atomic(snapshot, output_path);

  const std::string content = read_file(output_path);
  expect_contains(content, "\"job_id\": 42");
  expect_contains(content, "\"source\": \"winspool\"");
  std::filesystem::remove_all(directory);
}

TEST(Storage, AsyncJobSnapshotWriter)
{
  const wpce::core::JobSnapshot job{
    .schema_version = 1,
    .job_id = 84,
    .user = "DOMAIN\\bob",
    .document_name = "invoice.pdf",
    .printer_name = "Microsoft Print to PDF",
    .submit_time_utc = "2026-06-19T11:31:00Z",
    .status = "printing",
    .source = "winspool",
  };

  const wpce::core::MetadataSnapshot metadata{
    .schema_version = 1,
    .job_id = 84,
    .total_pages = {"10", wpce::core::FieldState::Reported},
    .pages_printed = {"3", wpce::core::FieldState::Reported},
    .copies = {"1", wpce::core::FieldState::Reported},
    .duplex = {"simplex", wpce::core::FieldState::Reported},
    .color = {"monochrome", wpce::core::FieldState::Reported},
    .paper_size = {"A4", wpce::core::FieldState::Reported},
    .orientation = {"portrait", wpce::core::FieldState::Reported},
    .n_up = {"1-up (normal)", wpce::core::FieldState::Reported},
    .collate = {"uncollated", wpce::core::FieldState::Reported},
    .print_quality = {"high", wpce::core::FieldState::Reported},
    .dm_fields_hex = "0x0007e35f",
  };

  const std::filesystem::path directory = make_test_directory("async-writer");
  const wpce::storage::JobSnapshotWriterConfig config{.output_directory = directory, .queue_capacity = 4};
  wpce::storage::AsyncJobSnapshotWriter writer(config);

  writer.start();
  EXPECT_TRUE(writer.enqueue(job, metadata));
  writer.stop();

  EXPECT_TRUE(writer.last_error().empty());
  expect_contains(read_file(wpce::storage::job_snapshot_path(config)), "\"job_id\": 84");
  expect_contains(read_file(wpce::storage::job_snapshot_archive_path(config, job)), "\"status\": \"printing\"");
  expect_contains(read_file(wpce::storage::metadata_snapshot_path(config)), "\"total_pages\": { \"value\": \"10\"");
  expect_contains(read_file(wpce::storage::metadata_archive_path(config, metadata)), "\"paper_size\": { \"value\": \"A4\"");
  std::filesystem::remove_all(directory);
}

TEST(Storage, LifecycleSummaryPersistence)
{
  const std::filesystem::path directory = make_test_directory("lifecycle-persist");

  wpce::core::JobLifecycleSummary summary{
    .schema_version = 1,
    .job_id = 77,
    .user = "DOMAIN\\persist-test",
    .document_name = "persist.pdf",
    .printer_name = "Microsoft Print to PDF",
    .submit_time_utc = "2026-06-19T12:00:00Z",
    .final_phase = wpce::core::JobLifecyclePhase::Completed,
    .progress_percent = 100,
    .source = "winspool",
  };

  const wpce::storage::JobSnapshotWriterConfig config{.output_directory = directory};
  const std::vector<wpce::core::JobLifecycleSummary> summaries{summary};

  wpce::storage::write_lifecycle_summaries_atomic(summaries, config);

  // Verify per-job archive.
  const std::string archive = read_file(wpce::storage::lifecycle_summary_archive_path(config, summary));
  expect_contains(archive, "\"job_id\": 77");
  expect_contains(archive, "\"final_phase\": \"completed\"");

  // Verify manifest.
  const std::string manifest = read_file(wpce::storage::lifecycle_summary_path(config));
  expect_contains(manifest, "\"session_jobs\"");
  expect_contains(manifest, "\"job_id\": 77");

  std::filesystem::remove_all(directory);
}

TEST(Serialization, LifecycleEventTiming)
{
  wpce::core::JobLifecycleEvent event{
    .event_id = "42-1",
    .job_id = 42,
    .sequence = 1,
    .timestamp_utc = "2026-06-19T12:00:00Z",
    .phase = wpce::core::JobLifecyclePhase::Spooling,
    .status_flags = "spooling",
    .detail = "Job 42 phase: spooling",
    .pages_printed = 0,
    .total_pages = 5,
    .delta_ms_since_prev = 150,
    .cumulative_phase_ms = 350,
  };

  const std::string json = wpce::storage::to_json(event);
  expect_contains(json, "\"delta_ms_since_prev\": 150");
  expect_contains(json, "\"cumulative_phase_ms\": 350");
}

TEST(Serialization, LifecycleSummaryTiming)
{
  wpce::core::JobLifecycleSummary summary{
    .schema_version = 1,
    .job_id = 88,
    .user = "DOMAIN\\timing-test",
    .document_name = "timing.pdf",
    .printer_name = "Microsoft Print to PDF",
    .submit_time_utc = "2026-06-19T12:00:00Z",
    .final_phase = wpce::core::JobLifecyclePhase::Completed,
    .progress_percent = 100,
    .source = "winspool",
    .total_duration_ms = 5230,
  };
  summary.phase_dwells.push_back({"spooling", 120});
  summary.phase_dwells.push_back({"printing", 4800});
  summary.phase_dwells.push_back({"completed", -1});

  const std::string json = wpce::storage::to_json(summary);
  expect_contains(json, "\"total_duration_ms\": 5230");
  expect_contains(json, "\"phase_dwells\"");
  expect_contains(json, "\"phase\": \"spooling\"");
  expect_contains(json, "\"duration_ms\": 120");
  expect_contains(json, "\"duration_ms\": -1");
}

// ── Collector tests ──────────────────────────────────────────────────────

TEST(Collector, NameMappers)
{
  using namespace wpce::collector;

  // N-up.
  EXPECT_EQ("1-up (normal)", n_up_name(1));
  EXPECT_EQ("2-up", n_up_name(2));
  EXPECT_EQ("4-up", n_up_name(4));
  EXPECT_EQ("16-up", n_up_name(16));
  EXPECT_TRUE(n_up_name(3).empty());

  // Collate.
#ifdef DMCOLLATE_FALSE
  EXPECT_EQ("uncollated", collate_name(DMCOLLATE_FALSE));
  EXPECT_EQ("collated", collate_name(DMCOLLATE_TRUE));
#endif

  // Print quality.
#ifdef DMRES_HIGH
  EXPECT_EQ("high", print_quality_name(DMRES_HIGH));
  EXPECT_EQ("draft", print_quality_name(DMRES_DRAFT));
#endif
}
