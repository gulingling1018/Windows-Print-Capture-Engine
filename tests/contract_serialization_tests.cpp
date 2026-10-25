#include "bridge/metadata_merger.h"
#include "collector/metadata_collector.h"
#include "common/app_config.h"
#include "common/document_name.h"
#include "core/data_contracts.h"
#include "domain/billing_batch_repository.h"
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
#include <unordered_map>

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

void write_file(const std::filesystem::path& path, const std::string& content)
{
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << content;
}

}  // namespace

TEST(Config, LegacyPricingFieldsAreSkippedAndEmptyColorBecomesMonochrome)
{
  const std::filesystem::path directory = make_test_directory("legacy-config");
  const std::filesystem::path path = directory / "wpce_config.json";
  write_file(path, R"json({
    "storage": { "output_root": "./data", "retention_days": 90 },
    "pricing": {
      "currency": "CNY",
      "color_price_pending": true,
      "volume_discounts": [
        { "min_pages": 100, "discount": 0.95, "note": { "ignored": true } }
      ],
      "rules": [
        {
          "rule_id": "legacy-empty-color",
          "paper_size": "A4",
          "duplex_mode": "simplex",
          "printer_name": "",
          "color_mode": "",
          "file_extension": "",
          "price_per_sheet": 0.15,
          "unknown_object": { "ignored": true }
        }
      ]
    }
  })json");

  wpce::common::AppConfig config = wpce::common::AppConfig::defaults();
  ASSERT_TRUE(config.load_from_json(path));
  ASSERT_EQ(config.pricing.rules.size(), 1u);
  EXPECT_EQ(config.pricing.rules[0].color_mode, "monochrome");

  std::filesystem::remove_all(directory);
}

TEST(DocumentName, SpoolerPlaceholderDetection)
{
  EXPECT_TRUE(wpce::common::is_spooler_placeholder_document(
      "\xE6\x9C\xAC\xE5\x9C\xB0\xE4\xB8\x8B\xE5\xB1\x82\xE6\x96\x87\xE6\xA1\xA3"));
  EXPECT_TRUE(wpce::common::is_spooler_placeholder_document(" Local Downlevel Document "));
  EXPECT_TRUE(wpce::common::is_spooler_placeholder_document("local downlevel document"));

  EXPECT_FALSE(wpce::common::is_spooler_placeholder_document("report.pdf"));
  EXPECT_FALSE(wpce::common::is_spooler_placeholder_document("Local Downlevel Document.pdf"));
  EXPECT_FALSE(wpce::common::is_spooler_placeholder_document(""));
}

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
  snapshot.printer_name = "Microsoft Print to PDF";
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
  expect_contains(json, "\"printer_name\": \"Microsoft Print to PDF\"");
  expect_contains(json, "\"total_pages\": { \"value\": \"5\", \"state\": \"reported\" }");
  expect_contains(json, "\"duplex\": { \"value\": \"\", \"state\": \"unsupported\" }");
  expect_contains(json, "\"n_up\": { \"value\": \"2-up\", \"state\": \"reported\" }");
  expect_contains(json, "\"collate\": { \"value\": \"collated\", \"state\": \"reported\" }");
  expect_contains(json, "\"dm_fields_hex\": \"0x0007ffff\"");

  const std::string xml = wpce::storage::to_xml(snapshot);
  expect_contains(xml, "<duplex state=\"unsupported\"></duplex>");
}

TEST(Serialization, PrintTicketMetadataLoadPreservesState)
{
  const std::filesystem::path directory = make_test_directory("pt-metadata");
  std::filesystem::create_directories(directory);
  const std::filesystem::path path = directory / "job-Test_Printer-42-pt-metadata.json";

  const std::string json =
    "{\n"
    "  \"schema_version\": 1,\n"
    "  \"job_id\": 42,\n"
    "  \"printer_name\": \"\\\\\\\\server\\\\Test Printer\",\n"
    "  \"total_pages\": { \"value\": \"\", \"state\": \"unknown\" },\n"
    "  \"pages_printed\": { \"value\": \"\", \"state\": \"unknown\" },\n"
    "  \"copies\": { \"value\": \"2\", \"state\": \"reported\" },\n"
    "  \"duplex\": { \"value\": \"long-edge (book)\", \"state\": \"reported\" },\n"
    "  \"color\": { \"value\": \"\", \"state\": \"unknown\" },\n"
    "  \"paper_size\": { \"value\": \"A4\", \"state\": \"reported\" },\n"
    "  \"orientation\": { \"value\": \"portrait\", \"state\": \"reported\" },\n"
    "  \"n_up\": { \"value\": \"\", \"state\": \"unknown\" },\n"
    "  \"collate\": { \"value\": \"collated\", \"state\": \"reported\" },\n"
    "  \"print_quality\": { \"value\": \"\", \"state\": \"unknown\" },\n"
    "  \"media_type\": { \"value\": \"\", \"state\": \"unknown\" },\n"
    "  \"dm_fields_hex\": \"PrintTicket\"\n"
    "}\n";

  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << json;
  }

  wpce::core::MetadataSnapshot loaded;
  ASSERT_TRUE(wpce::bridge::load_pt_metadata(path, loaded));
  EXPECT_EQ(42u, loaded.job_id);
  EXPECT_EQ("\\\\server\\Test Printer", loaded.printer_name);
  EXPECT_EQ(wpce::core::FieldState::Reported, loaded.duplex.state);
  EXPECT_EQ("long-edge (book)", loaded.duplex.value);
  EXPECT_EQ(wpce::core::FieldState::Unknown, loaded.color.state);
  EXPECT_EQ("PrintTicket", loaded.dm_fields_hex);

  std::filesystem::remove_all(directory);
}

TEST(Serialization, PageArtifactIndex)
{
  const wpce::core::PageArtifactIndex index{
    .schema_version = 1,
    .job_id = 42,
    .printer_name = "Microsoft Print to PDF",
    .document_sequence = 1,
    .page_sequence = 2,
    .job_print_ticket = "job.pt.xml",
    .document_print_ticket = "document-001.pt.xml",
    .page_print_ticket = "page-002.pt.xml",
    .fixed_page = "page-002.fpage",
    .source = "xps-filter",
  };

  const std::string json = wpce::storage::to_json(index);
  expect_contains(json, "\"printer_name\": \"Microsoft Print to PDF\"");
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
  expect_contains(json, "\"phase\": \"Spooling\"");
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
  expect_contains(json, "\"final_phase\": \"Completed\"");
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

  // Terminal states — only genuine job errors are terminal.
  EXPECT_EQ(Phase::Failed, wpce::watcher::classify_phase(0x00000002, ""));     // JOB_STATUS_ERROR
  EXPECT_EQ(Phase::Cancelled, wpce::watcher::classify_phase(0x00000100, ""));  // JOB_STATUS_DELETED
  EXPECT_EQ(Phase::Completed, wpce::watcher::classify_phase(0x00001000, ""));  // JOB_STATUS_COMPLETE
  EXPECT_EQ(Phase::Completed, wpce::watcher::classify_phase(0x00002000, ""));  // JOB_STATUS_RETAINED

  // Active states.
  EXPECT_EQ(Phase::Printing, wpce::watcher::classify_phase(0x00000010, ""));   // JOB_STATUS_PRINTING
  EXPECT_EQ(Phase::Spooling, wpce::watcher::classify_phase(0x00000008, ""));   // JOB_STATUS_SPOOLING

  // Device-level status flags — waiting, not terminal.
  EXPECT_EQ(Phase::Spooling, wpce::watcher::classify_phase(0x00000020, ""));   // JOB_STATUS_OFFLINE
  EXPECT_EQ(Phase::Spooling, wpce::watcher::classify_phase(0x00000040, ""));   // JOB_STATUS_PAPEROUT
  EXPECT_EQ(Phase::Spooling, wpce::watcher::classify_phase(0x00000400, ""));   // JOB_STATUS_USER_INTERVENTION

  // Compound flags: active processing takes priority over device state.
  EXPECT_EQ(Phase::Spooling, wpce::watcher::classify_phase(0x00000008 | 0x00000020, ""));  // SPOOLING|OFFLINE
  EXPECT_EQ(Phase::Printing, wpce::watcher::classify_phase(0x00000010 | 0x00000020, ""));  // PRINTING|OFFLINE
  EXPECT_EQ(Phase::Failed,  wpce::watcher::classify_phase(0x00000002 | 0x00000020, ""));   // ERROR|OFFLINE → Failed

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

  // Phase name (capitalized for round-trip serialisation compatibility).
  EXPECT_EQ("Accepted", wpce::core::to_string(Phase::Accepted));
  EXPECT_EQ("Completed", wpce::core::to_string(Phase::Completed));
  EXPECT_EQ("Failed", wpce::core::to_string(Phase::Failed));
  EXPECT_EQ("Cancelled", wpce::core::to_string(Phase::Cancelled));
  EXPECT_EQ("Stalled", wpce::core::to_string(Phase::Stalled));
}

TEST(Watcher, PreservesTransientUnknownForNonTerminalPhases)
{
  using Phase = wpce::core::JobLifecyclePhase;
  using wpce::watcher::should_preserve_transient_unknown_status;

  EXPECT_FALSE(should_preserve_transient_unknown_status(Phase::Spooling, false));

  EXPECT_TRUE(should_preserve_transient_unknown_status(Phase::Accepted, true));
  EXPECT_TRUE(should_preserve_transient_unknown_status(Phase::Spooling, true));
  EXPECT_TRUE(should_preserve_transient_unknown_status(Phase::Printing, true));
  EXPECT_TRUE(should_preserve_transient_unknown_status(Phase::Stalled, true));

  EXPECT_FALSE(should_preserve_transient_unknown_status(Phase::Unknown, true));
  EXPECT_FALSE(should_preserve_transient_unknown_status(Phase::Completed, true));
  EXPECT_FALSE(should_preserve_transient_unknown_status(Phase::Failed, true));
  EXPECT_FALSE(should_preserve_transient_unknown_status(Phase::Cancelled, true));
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
  expect_contains(read_file(wpce::storage::job_snapshot_path(config, job)), "\"job_id\": 84");
  expect_contains(read_file(wpce::storage::job_snapshot_archive_path(config, job)), "\"status\": \"printing\"");
  expect_contains(read_file(wpce::storage::metadata_snapshot_path(config, metadata)), "\"total_pages\": { \"value\": \"10\"");
  expect_contains(read_file(wpce::storage::metadata_archive_path(config, metadata)), "\"paper_size\": { \"value\": \"A4\"");
  expect_contains(read_file(wpce::storage::latest_manifest_path(config)), "\"printer_name\": \"Microsoft Print to PDF\"");
  std::filesystem::remove_all(directory);
}

TEST(Storage, BillingBatchRepositoryRoundTripsJobRecords)
{
  const std::filesystem::path directory = make_test_directory("billing-batch-repo");

  wpce::domain::BillingBatch batch;
  batch.batch_id = "B-20260626-000001";
  batch.display_name = "用户#1";
  batch.source_user = "DOMAIN\\alice";
  batch.state = wpce::domain::BatchState::Active;
  batch.period_start_utc = "2026-06-26T09:00:00Z";
  batch.total_pages = 5;
  batch.simplex_pages = 5;
  batch.document_count = 1;
  batch.total_price = 3.50;
  batch.pending_count = 1;
  batch.job_ids.push_back(100);

  wpce::domain::PrintJobRecord rec;
  rec.job_id = 100;
  rec.batch_job_seq = 1;
  rec.phase = wpce::core::JobLifecyclePhase::Completed;
  rec.last_updated_utc = "2026-06-26T09:01:00Z";
  rec.snapshot.job_id = 100;
  rec.snapshot.external_job_id = 42;
  rec.snapshot.idempotency_key = "Color Printer\\x1f42\\x1f2026-06-26T09:00:00Z";
  rec.snapshot.user = "DOMAIN\\alice";
  rec.snapshot.document_name = "color-plan.pdf";
  rec.snapshot.printer_name = "Color Printer";
  rec.snapshot.submit_time_utc = "2026-06-26T09:00:00Z";
  rec.metadata.job_id = 100;
  rec.metadata.total_pages = {"5", wpce::core::FieldState::Reported};
  rec.metadata.duplex = {"", wpce::core::FieldState::NotReported};
  rec.metadata.color = {"color", wpce::core::FieldState::Reported};
  rec.metadata.paper_size = {"A4", wpce::core::FieldState::Reported};
  rec.metadata.file_extension = {"dwt", wpce::core::FieldState::Reported};
  rec.metadata.file_extension_source = "shd_path";
  rec.metadata.file_extension_confidence = 3;
  rec.pricing.job_id = 100;
  rec.pricing.total_pages = 5;
  rec.pricing.copies = 1;
  rec.pricing.effective_sheets = 5;
  rec.pricing.matched_rule_id = "color-a4";
  rec.pricing.matched_printer_name = "Color Printer";
  rec.pricing.total_price = 3.50;
  rec.pricing.color_pages = 2;
  rec.pricing.mono_pages = 3;
  rec.pricing.color_subtotal = 2.00;
  rec.pricing.mono_subtotal = 1.50;
  rec.pricing.price_pending = true;
  rec.pricing.pending_reason = "awaiting_color_pages";
  rec.pricing.manual_price_override = true;
  rec.pricing.manual_total_price = 3.50;

  std::unordered_map<std::uint32_t, wpce::domain::PrintJobRecord> jobs;
  jobs.emplace(rec.job_id, rec);

  wpce::domain::BillingBatchRepository repo;
  repo.setFilePath(directory / "2026-06-26" / "orders.json");
  wpce::core::SettlementOrder order;
  order.order_id = "SETTLE-20260626-000001";
  order.task_id = batch.batch_id;
  order.batch_id = batch.batch_id;
  order.job_ids = {100};
  order.pricing_entries = {rec.pricing};
  order.total_price = 3.50;
  wpce::core::SettlementOrder duplicateTask = order;
  duplicateTask.order_id = "SETTLE-20260626-000002";
  ASSERT_TRUE(repo.save({batch}, jobs, {order, duplicateTask}, "2026-06-26"));
  EXPECT_TRUE(std::filesystem::exists(directory / "2026-06-26" / "orders.json"));
  expect_contains(read_file(directory / "2026-06-26" / "orders.json"), "\"tasks\"");
  expect_contains(read_file(directory / "2026-06-26" / "orders.json"), "\"jobs\"");

  std::vector<wpce::domain::BillingBatch> loadedBatches;
  std::unordered_map<std::uint32_t, wpce::domain::PrintJobRecord> loadedJobs;
  std::string savedDate;
  ASSERT_TRUE(repo.load(loadedBatches, loadedJobs, savedDate));

  ASSERT_EQ(savedDate, "2026-06-26");
  ASSERT_EQ(loadedBatches.size(), 1u);
  ASSERT_EQ(loadedJobs.size(), 1u);
  const auto& loaded = loadedJobs.at(100);
  EXPECT_EQ(loaded.metadata.duplex.state, wpce::core::FieldState::NotReported);
  EXPECT_EQ(loaded.pricing.color_pages, 2);
  EXPECT_EQ(loaded.pricing.mono_pages, 3);
  EXPECT_DOUBLE_EQ(loaded.pricing.color_subtotal, 2.00);
  EXPECT_EQ(loaded.pricing.matched_printer_name, "Color Printer");
  EXPECT_TRUE(loaded.pricing.price_pending);
  EXPECT_EQ(loaded.snapshot.external_job_id, 42u);
  EXPECT_FALSE(loaded.snapshot.idempotency_key.empty());
  EXPECT_EQ(loaded.metadata.file_extension.value, "dwt");
  EXPECT_EQ(loaded.metadata.file_extension_source, "shd_path");
  EXPECT_EQ(loaded.metadata.file_extension_confidence, 3);
  EXPECT_TRUE(loaded.pricing.manual_price_override);
  EXPECT_DOUBLE_EQ(loaded.pricing.manual_total_price, 3.50);

  std::vector<wpce::core::SettlementOrder> loadedOrders;
  ASSERT_TRUE(repo.loadOrders(loadedOrders));
  ASSERT_EQ(loadedOrders.size(), 1u);
  EXPECT_EQ(loadedOrders.front().order_id, order.order_id);
  EXPECT_EQ(loadedOrders.front().task_id, batch.batch_id);

  std::vector<wpce::domain::BillingBatch> summaryBatches;
  std::string summaryDate;
  ASSERT_TRUE(repo.loadSummaries(summaryBatches, summaryDate));
  ASSERT_EQ(summaryDate, "2026-06-26");
  ASSERT_EQ(summaryBatches.size(), 1u);
  EXPECT_EQ(summaryBatches[0].batch_id, batch.batch_id);

  std::unordered_map<std::uint32_t, wpce::domain::PrintJobRecord> selectedJobs;
  ASSERT_TRUE(repo.loadJobsForBatches({batch.batch_id}, selectedJobs));
  ASSERT_EQ(selectedJobs.size(), 1u);
  EXPECT_EQ(selectedJobs.at(100).snapshot.document_name, "color-plan.pdf");

  std::unordered_map<std::uint32_t, wpce::domain::PrintJobRecord> missingJobs;
  ASSERT_TRUE(repo.loadJobsForBatches({"missing-batch"}, missingJobs));
  EXPECT_TRUE(missingJobs.empty());

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
  expect_contains(archive, "\"final_phase\": \"Completed\"");

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
  summary.phase_dwells.push_back({"Spooling", 120});
  summary.phase_dwells.push_back({"Printing", 4800});
  summary.phase_dwells.push_back({"Completed", -1});

  const std::string json = wpce::storage::to_json(summary);
  expect_contains(json, "\"total_duration_ms\": 5230");
  expect_contains(json, "\"phase_dwells\"");
  expect_contains(json, "\"phase\": \"Spooling\"");
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
