#include "domain/billing_batch_repository.h"
#include "domain/billing_migration.h"
#include "bridge/settlement_bridge.h"
#include "storage/capture_retention.h"
#include "storage/file_utils.h"
#include "storage/job_snapshot_writer.h"
#include "common/app_config.h"
#include "common/counter_store.h"
#include <gtest/gtest.h>
#include <chrono>
#include <fstream>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
namespace fs = std::filesystem;
using Repository = wpce::domain::BillingBatchRepository;
using Status = wpce::domain::BucketLoadStatus;
std::string read(const fs::path& path)
{ std::ifstream in(path, std::ios::binary); std::ostringstream text; text << in.rdbuf(); return text.str(); }
void write(const fs::path& path, const std::string& text)
{ fs::create_directories(path.parent_path()); std::ofstream out(path, std::ios::binary); out << text; }
class BillingStorageSafety : public testing::Test
{
 protected:
  fs::path root, bucket;
  Repository repo;
  std::vector<wpce::domain::BillingBatch> tasks;
  std::unordered_map<std::uint32_t, wpce::domain::PrintJobRecord> jobs;
  std::vector<wpce::core::SettlementOrder> orders;
  std::string date;
  void SetUp() override
  {
    root = fs::temp_directory_path() / ("wpce-safety-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    bucket = root / "2026-10-24" / "orders.json"; repo.setFilePath(bucket);
  }
  void TearDown() override { wpce::storage::set_atomic_write_hook({}); std::error_code ec; fs::remove_all(root, ec); }
  auto load() { return repo.loadDaily(tasks, jobs, orders, date); }
  bool save() { return repo.save(tasks, jobs, orders, "2026-10-24"); }
  wpce::core::SettlementOrder makeOrder(const std::string& id = "SETTLE-20261024-000001")
  {
    wpce::core::SettlementOrder order;
    order.order_id = id; order.task_id = "B-" + id.substr(7); order.batch_id = order.task_id;
    order.settled_at_utc = "2026-10-23T17:00:00Z"; order.total_price = 2.5;
    return order;
  }
};

TEST_F(BillingStorageSafety, RetentionProtectsOrdersStateAndUnknownData)
{
  const auto old = root / "2001-01-01";
  for (const auto* name : {"jobs/a.json", "latest/a.json", "pages/a.fpage", "orders.json", "orders.json.bak",
                            "billing-migration-v4.complete", "customer/note.txt", "unknown.tmp"})
    write(old / name, "preserve-or-capture");
  write(root / "_state/counters.json", "state"); write(root / "2026-10-24/jobs/a.json", "current");
  auto result = wpce::storage::cleanup_expired_capture(root, "2026-10-24", 90);
  EXPECT_TRUE(result.errors.empty()); EXPECT_EQ(result.removed_directories, 3u);
  EXPECT_FALSE(fs::exists(old / "jobs"));
  for (const auto* name : {"orders.json", "orders.json.bak", "billing-migration-v4.complete", "customer/note.txt", "unknown.tmp"})
    EXPECT_EQ(read(old / name), "preserve-or-capture");
  EXPECT_EQ(read(root / "_state/counters.json"), "state");
  EXPECT_EQ(read(root / "2026-10-24/jobs/a.json"), "current");
}

TEST_F(BillingStorageSafety, RetentionSkipsLinkedCaptureSubtree)
{
  const auto outside = root / "protected";
  write(outside / "important", "keep");
  fs::create_directories(root / "2001-01-01");
  std::error_code ec;
  fs::create_directory_symlink(outside, root / "2001-01-01/jobs", ec);
  if (ec) GTEST_SKIP() << "Directory symlinks unavailable: " << ec.message();
  wpce::storage::cleanup_expired_capture(root, "2026-10-24", 90);
  EXPECT_EQ(read(outside / "important"), "keep");
  EXPECT_TRUE(fs::is_symlink(root / "2001-01-01/jobs"));
}

TEST_F(BillingStorageSafety, MissingAndUnreadableAreDifferent)
{
  EXPECT_EQ(load().status, Status::Missing);
  fs::create_directories(bucket);
  EXPECT_EQ(load().status, Status::AccessDenied);
  EXPECT_FALSE(save());
}

TEST_F(BillingStorageSafety, ExclusiveFileLockDoesNotRestoreOrOverwriteBucket)
{
#ifdef _WIN32
  ASSERT_TRUE(save()); ASSERT_TRUE(save()); const auto original = read(bucket);
  struct Handle
  {
    HANDLE value;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
  };
  {
    Handle lock{CreateFileW(bucket.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr)};
    ASSERT_NE(lock.value, INVALID_HANDLE_VALUE);
    EXPECT_EQ(load().status, Status::AccessDenied);
    EXPECT_FALSE(save());
  }
  EXPECT_EQ(read(bucket), original);
#else
  GTEST_SKIP() << "Windows sharing semantics";
#endif
}

TEST_F(BillingStorageSafety, DamagedAndIncompleteBucketsCannotBeOverwritten)
{
  const std::vector<std::string> damaged = {
    "{", "{}", R"({"schema_version":4,"business_date":"2026-10-24","tasks":[]})",
    R"({"schema_version":4,"business_date":"2026-10-24","tasks":[],"orders":[{}]})",
    R"({"schema_version":4,"business_date":"2026-10-24","tasks":[],"orders":[]} trailing)",
    R"({"schema_version":4,"business_date":"2026-10-24","tasks":[],"tasks":[],"orders":[]})",
    R"({"schema_version":4,"business_date":"2026-10-25","tasks":[],"orders":[]})",
    R"({"schema_version":4,"business_date":"2026-10-24","tasks":[{"batch_id":"x","state":"settled","job_ids":[],"jobs":[]}],"orders":[]})"
  };
  for (const auto& text : damaged)
  {
    write(bucket, text);
    EXPECT_FALSE(load().ok()) << text;
    EXPECT_FALSE(save()) << text;
    EXPECT_EQ(read(bucket), text);
  }
}

TEST_F(BillingStorageSafety, FutureSchemaDoesNotFallBackToOlderBackup)
{
  ASSERT_TRUE(save()); ASSERT_TRUE(save());
  const std::string future = R"({"schema_version":99,"business_date":"2026-10-24","tasks":[],"orders":[]})";
  write(bucket, future);
  EXPECT_EQ(load().status, Status::UnsupportedSchema);
  EXPECT_FALSE(save()); EXPECT_EQ(read(bucket), future);
  const std::string futureLast = R"({"tasks":[{"future_fields":true}],"business_date":"2026-10-24","orders":[],"schema_version":99})";
  write(bucket, futureLast);
  EXPECT_EQ(load().status, Status::UnsupportedSchema);
  EXPECT_FALSE(save()); EXPECT_EQ(read(bucket), futureLast);
}

TEST_F(BillingStorageSafety, BackupRecoveryPreservesDamagedBytes)
{
  ASSERT_TRUE(save()); ASSERT_TRUE(save());
  write(bucket, "{damaged");
  EXPECT_EQ(load().status, Status::Recovered);
  EXPECT_EQ(date, "2026-10-24");
  bool preserved = false;
  for (const auto& entry : fs::directory_iterator(bucket.parent_path()))
    if (entry.path().extension() == ".invalid" && read(entry.path()) == "{damaged") preserved = true;
  EXPECT_TRUE(preserved); EXPECT_EQ(load().status, Status::Loaded);
}

TEST_F(BillingStorageSafety, AtomicFailureStagesPreservePreviousBucket)
{
  ASSERT_TRUE(save()); const auto original = read(bucket);
  using Stage = wpce::storage::AtomicWriteStage;
  for (auto stage : {Stage::Write, Stage::Flush, Stage::Close, Stage::Sync, Stage::Replace})
  {
    wpce::storage::set_atomic_write_hook([&](auto actual, const fs::path& target) {
      if (actual == stage && target == bucket) throw std::runtime_error("injected write failure");
    });
    EXPECT_FALSE(save()); EXPECT_EQ(read(bucket), original);
    wpce::storage::set_atomic_write_hook({});
    EXPECT_EQ(load().status, Status::Loaded);
  }
}

TEST_F(BillingStorageSafety, BackupFailureDoesNotReplaceCurrentBucket)
{
  ASSERT_TRUE(save()); const auto original = read(bucket);
  wpce::storage::set_atomic_write_hook([](auto, const fs::path& target) {
    if (target.extension() == ".bak") throw std::runtime_error("backup failure");
  });
  EXPECT_FALSE(save()); EXPECT_EQ(read(bucket), original);
}

TEST_F(BillingStorageSafety, UnicodePathsAndJsonEscapesRoundTrip)
{
  bucket = root / fs::path(L"\u8ba2\u5355-\U0001f600") / "2026-10-24" / "orders.json"; repo.setFilePath(bucket);
  auto order = makeOrder(); order.user_id = "用户"; orders = {order};
  ASSERT_TRUE(save()); ASSERT_TRUE(load().ok()); ASSERT_EQ(orders.size(), 1u);
  EXPECT_EQ(orders.front().user_id, "用户");
  auto text = read(bucket);
  const auto at = text.find("用户"); ASSERT_NE(at, std::string::npos);
  text.replace(at, std::string("用户").size(), "\\u7528\\u6237"); write(bucket, text);
  ASSERT_TRUE(load().ok()); EXPECT_EQ(orders.front().user_id, "用户");
  wpce::common::CounterStore counters; counters.setStatePath(bucket.parent_path() / L"\u8ba1\u6570.json");
  EXPECT_FALSE(counters.nextBatchId().empty());
  EXPECT_TRUE(wpce::common::AppConfig::defaults().save_to_json(bucket.parent_path() / L"\u914d\u7f6e.json"));
}

TEST_F(BillingStorageSafety, CandidateDateMismatchCannotReplaceBucket)
{
  ASSERT_TRUE(save()); const auto original = read(bucket);
  EXPECT_FALSE(repo.save(tasks, jobs, orders, "2026-10-25")); EXPECT_EQ(read(bucket), original);
}

TEST_F(BillingStorageSafety, CounterAndConfigPropagateFinalWriteFailure)
{
  const auto configPath = root / "config.json";
  const auto config = wpce::common::AppConfig::defaults();
  ASSERT_TRUE(config.save_to_json(configPath));
  wpce::common::CounterStore counter;
  const auto counterPath = root / "counter.json"; counter.setStatePath(counterPath);
  ASSERT_FALSE(counter.nextBatchId().empty());
  const auto configBefore = read(configPath), counterBefore = read(counterPath);
  wpce::storage::set_atomic_write_hook([](auto stage, const fs::path&) {
    if (stage == wpce::storage::AtomicWriteStage::Close) throw std::runtime_error("close failed");
  });
  EXPECT_FALSE(config.save_to_json(configPath)); EXPECT_TRUE(counter.nextOrderId().empty());
  EXPECT_EQ(read(configPath), configBefore); EXPECT_EQ(read(counterPath), counterBefore);
}

TEST_F(BillingStorageSafety, CorruptCounterFailsClosed)
{
  const auto path = root / "counter.json"; write(path, "{broken");
  wpce::common::CounterStore counter; counter.setStatePath(path);
  EXPECT_FALSE(counter.healthy()); EXPECT_TRUE(counter.nextBatchId().empty());
  EXPECT_TRUE(counter.nextOrderId().empty()); EXPECT_EQ(read(path), "{broken");
}

TEST_F(BillingStorageSafety, FailedReadDoesNotReplaceCallerState)
{
  orders = {makeOrder()}; date = "sentinel";
  auto backup = bucket; backup += ".bak";
  write(backup, "{broken");
  EXPECT_FALSE(load().ok()); ASSERT_EQ(orders.size(), 1u); EXPECT_EQ(date, "sentinel");
}

TEST_F(BillingStorageSafety, SchemaLastAndUtf8BomAreSupported)
{
  write(bucket, "\xef\xbb\xbf" R"({"orders":[],"tasks":[],"business_date":"2026-10-24","schema_version":4})");
  EXPECT_TRUE(load().ok());
  write(bucket, R"({"orders":[],"tasks":[{"batch_id":"x","job_ids":[],"state":"active","total_price":0}],"business_date":"2026-10-24","schema_version":4})");
  EXPECT_FALSE(load().ok());
}

TEST_F(BillingStorageSafety, CaptureWriterRotatesAtEnqueueAcrossMidnight)
{
  wpce::storage::JobSnapshotWriterConfig config;
  config.output_directory = root / "2026-10-24"; config.rotate_daily = true;
  std::string now = "2026-10-24"; config.date_provider = [&] { return now; };
  wpce::storage::AsyncJobSnapshotWriter writer(config); writer.start();
  wpce::core::JobSnapshot job; job.job_id = 1; job.printer_name = "Test";
  wpce::core::MetadataSnapshot metadata; metadata.job_id = 1; metadata.printer_name = "Test";
  ASSERT_TRUE(writer.enqueue(job, metadata));
  now = "2026-10-25"; job.job_id = 2; metadata.job_id = 2;
  ASSERT_TRUE(writer.enqueue(job, metadata)); writer.stop();
  EXPECT_TRUE(fs::exists(root / "2026-10-24/jobs" / wpce::storage::printer_storage_token("Test") / "job-1.json"));
  EXPECT_TRUE(fs::exists(root / "2026-10-25/jobs" / wpce::storage::printer_storage_token("Test") / "job-2.json"));
  EXPECT_FALSE(fs::exists(root / "2026-10-24/jobs" / wpce::storage::printer_storage_token("Test") / "job-2.json"));
}

TEST_F(BillingStorageSafety, MigrationGroupsThreeDatesAndRetriesWithoutDuplicates)
{
  for (const auto& compact : {"20261022", "20261023", "20261024"})
  {
    const auto order = makeOrder(std::string("SETTLE-") + compact + "-000001");
    const auto day = std::string(compact).substr(0,4) + "-" + std::string(compact).substr(4,2) + "-" + std::string(compact).substr(6,2);
    write(root / "history" / day / (order.order_id + ".json"), wpce::bridge::SettlementBridge::toJson(order));
    write(root / "_state/settlement_orders" / (order.order_id + ".json"), wpce::bridge::SettlementBridge::toJson(order));
  }
  auto migrated = wpce::domain::migrate_legacy_billing(root, "2026-10-24");
  ASSERT_TRUE(migrated.ok()); EXPECT_EQ(migrated.migrated_dates, 3u);
  EXPECT_TRUE(wpce::domain::migrate_legacy_billing(root, "2026-10-24").ok());
  for (const auto* day : {"2026-10-22", "2026-10-23", "2026-10-24"})
  {
    Repository target; target.setFilePath(root / day / "orders.json");
    ASSERT_TRUE(target.loadDaily(tasks, jobs, orders, date).ok());
    ASSERT_EQ(orders.size(), 1u); EXPECT_EQ(date, day);
  }
  EXPECT_TRUE(fs::exists(root / "_state/settlement_orders/SETTLE-20261022-000001.json"));
}

TEST_F(BillingStorageSafety, MigrationConflictAndAmbiguousDatePreserveSources)
{
  auto order = makeOrder(); const auto source = root / "history/2026-10-24/order.json";
  write(source, wpce::bridge::SettlementBridge::toJson(order));
  order.total_price += 1;
  write(root / "_state/settlement_orders/copy.json", wpce::bridge::SettlementBridge::toJson(order));
  auto result = wpce::domain::migrate_legacy_billing(root, "2026-10-24");
  EXPECT_FALSE(result.ok()); EXPECT_FALSE(fs::exists(bucket)); EXPECT_TRUE(fs::exists(source));
  fs::remove(root / "_state/settlement_orders/copy.json");
  order.order_id = "unknown-date"; write(root / "_state/settlement_orders/unknown.json", wpce::bridge::SettlementBridge::toJson(order));
  EXPECT_FALSE(wpce::domain::migrate_legacy_billing(root, "2026-10-24").ok());
  EXPECT_TRUE(fs::exists(root / "_state/settlement_orders/unknown.json"));
}

TEST_F(BillingStorageSafety, MigrationInterruptedWriteRetriesWithoutRemovingSources)
{
  const auto source = root / "_state/settlement_orders/order.json";
  const auto order = makeOrder();
  write(source, wpce::bridge::SettlementBridge::toJson(order));
  wpce::storage::set_atomic_write_hook([this](auto stage, const fs::path& target) {
    if (target == bucket && stage == wpce::storage::AtomicWriteStage::Replace) throw std::runtime_error("migration interrupted");
  });
  EXPECT_FALSE(wpce::domain::migrate_legacy_billing(root, "2026-10-24").ok());
  EXPECT_FALSE(fs::exists(bucket)); EXPECT_TRUE(fs::exists(source));
  wpce::storage::set_atomic_write_hook({});
  EXPECT_TRUE(wpce::domain::migrate_legacy_billing(root, "2026-10-24").ok());
  ASSERT_TRUE(load().ok()); ASSERT_EQ(orders.size(), 1u);
  EXPECT_EQ(orders.front().order_id, order.order_id); EXPECT_TRUE(fs::exists(source));
}
}
