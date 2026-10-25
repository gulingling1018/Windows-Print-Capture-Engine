#include "billing/billing_service.h"
#include "storage/file_utils.h"
#include "bridge/settlement_bridge.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <gtest/gtest.h>
#include <chrono>
#include <fstream>
#include <sstream>

namespace
{
namespace fs = std::filesystem;
std::string bytes(const fs::path& path)
{ std::ifstream in(path, std::ios::binary); std::ostringstream out; out << in.rdbuf(); return out.str(); }
class BillingServiceSafety : public testing::Test
{
 protected:
  int argc{1}; char name[32]{"wpce-safety-tests"}; char* argv[2]{name, nullptr};
  std::unique_ptr<QCoreApplication> app;
  std::unique_ptr<wpce::ui::BillingService> service;
  fs::path root, bucket; std::string today{"2026-10-24"};
  void SetUp() override
  {
    if (!QCoreApplication::instance()) app = std::make_unique<QCoreApplication>(argc, argv);
    root = fs::temp_directory_path() / ("wpce-service-safety-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    bucket = root / today / "orders.json";
    service = std::make_unique<wpce::ui::BillingService>();
    service->setBusinessDateProvider([this] { return today; });
    service->setStateDirectory(QString::fromStdWString((root / "_state").wstring()));
    service->setOrderBucketPath(QString::fromStdWString(bucket.wstring()));
  }
  void TearDown() override
  {
    wpce::storage::set_atomic_write_hook({}); service.reset(); app.reset();
    std::error_code ec; fs::remove_all(root, ec);
  }
  void addJob(std::uint32_t id)
  {
    wpce::core::JobSnapshot snapshot; snapshot.job_id = id; snapshot.external_job_id = id;
    snapshot.idempotency_key = "physical-" + std::to_string(id); snapshot.printer_name = "Test";
    snapshot.user = "alice"; snapshot.document_name = "file.pdf"; snapshot.submit_time_utc = today + "T01:00:00Z";
    service->onJobSnapshot(snapshot);
    wpce::core::MetadataSnapshot metadata; metadata.job_id = id; metadata.printer_name = "Test";
    metadata.total_pages = {"2", wpce::core::FieldState::Reported};
    metadata.copies = {"1", wpce::core::FieldState::Reported};
    metadata.paper_size = {"A4", wpce::core::FieldState::Reported};
    metadata.color = {"monochrome", wpce::core::FieldState::Reported};
    metadata.duplex = {"simplex", wpce::core::FieldState::Reported};
    service->onMetadata(metadata);
  }
  QString active() { return QString::fromStdString(service->currentActiveBatch()->batch_id); }
  void pump()
  {
    QEventLoop loop; QTimer::singleShot(1200, &loop, &QEventLoop::quit); loop.exec();
  }
};

TEST_F(BillingServiceSafety, FrozenFinancialsSurviveLateEventsAndRestart)
{
  addJob(101); const auto task = active();
  const auto order = service->settle(task); ASSERT_FALSE(order.isEmpty());
  const auto previousPricing = service->jobRecord(101)->pricing;
  const auto previousBatch = *service->batchById(task.toStdString());
  wpce::core::MetadataSnapshot late; late.job_id = 101;
  late.total_pages = {"100", wpce::core::FieldState::Reported};
  late.copies = {"20", wpce::core::FieldState::Reported};
  late.pages_printed = {"99", wpce::core::FieldState::Reported};
  service->onMetadata(late);
  wpce::core::JobLifecycleEvent event; event.job_id = 101;
  event.phase = wpce::core::JobLifecyclePhase::Completed; event.total_pages = 200; event.pages_printed = 200;
  service->onLifecycleEvent(event); service->setColorPages(101, 1);
  EXPECT_FALSE(service->setManualPrice(101, 500)); EXPECT_FALSE(service->deleteJob(101));
  service->flushSave();
  EXPECT_DOUBLE_EQ(service->jobRecord(101)->pricing.total_price, previousPricing.total_price);
  EXPECT_EQ(service->jobRecord(101)->metadata.total_pages.value, "2");
  EXPECT_EQ(service->jobRecord(101)->metadata.pages_printed.value, "200");
  EXPECT_EQ(service->batchById(task.toStdString())->total_pages, previousBatch.total_pages);
  service->load();
  EXPECT_DOUBLE_EQ(service->batchById(task.toStdString())->total_price, previousBatch.total_price);
  EXPECT_EQ(service->settle(task), order);
}

TEST_F(BillingServiceSafety, FailedSettlementDoesNotCommitMemoryOrDisk)
{
  addJob(101); service->flushSave(); const auto original = bytes(bucket); const auto task = active();
  wpce::storage::set_atomic_write_hook([this](auto stage, const fs::path& path) {
    if (path == bucket && stage == wpce::storage::AtomicWriteStage::Replace) throw std::runtime_error("disk failure");
  });
  EXPECT_TRUE(service->settle(task).isEmpty());
  EXPECT_TRUE(service->batchById(task.toStdString())->is_active());
  EXPECT_EQ(bytes(bucket), original);
  wpce::storage::set_atomic_write_hook({});
  EXPECT_FALSE(service->settle(task).isEmpty());
}

TEST_F(BillingServiceSafety, DamagedBucketBlocksNewWrites)
{
  fs::create_directories(bucket.parent_path());
  { std::ofstream out(bucket); out << "{broken"; }
  service->load(); EXPECT_FALSE(service->storageReady());
  addJob(101); service->flushSave();
  EXPECT_EQ(service->todayJobCount(), 0); EXPECT_EQ(bytes(bucket), "{broken");
}

TEST_F(BillingServiceSafety, MidnightKeepsOldOrdersAndSwitchesIdsAndBucket)
{
  addJob(101); ASSERT_FALSE(service->settle(active()).isEmpty());
  addJob(102); service->flushSave(); const auto old = bytes(bucket);
  today = "2026-10-25"; ASSERT_TRUE(service->synchronizeBusinessDay());
  EXPECT_EQ(service->todayJobCount(), 0); EXPECT_EQ(bytes(bucket), old);
  addJob(103); const auto task = active();
  EXPECT_TRUE(task.startsWith("B-20261025-"));
  const auto order = service->settle(task);
  EXPECT_TRUE(order.startsWith("SETTLE-20261025-"));
  EXPECT_TRUE(fs::exists(root / today / "orders.json"));
  service->load(); EXPECT_EQ(service->todayJobCount(), 1); EXPECT_EQ(bytes(bucket), old);
  wpce::core::JobLifecycleEvent oldEvent; oldEvent.job_id = 102; oldEvent.total_pages = 500;
  service->onLifecycleEvent(oldEvent); EXPECT_EQ(service->todayJobCount(), 1);
}

TEST_F(BillingServiceSafety, FailedOldDayFlushBlocksSwitchAndReplaysDeferredEvents)
{
  addJob(101); service->flushSave(); ASSERT_TRUE(service->setManualPrice(101, 3));
  const auto original = bytes(bucket);
  wpce::storage::set_atomic_write_hook([this](auto stage, const fs::path& path) {
    if (path == bucket && stage == wpce::storage::AtomicWriteStage::Replace) throw std::runtime_error("old day locked");
  });
  today = "2026-10-25"; EXPECT_FALSE(service->synchronizeBusinessDay());
  EXPECT_EQ(service->boundBusinessDate(), "2026-10-24");
  addJob(103); EXPECT_EQ(bytes(bucket), original); EXPECT_FALSE(fs::exists(root / today / "orders.json"));
  wpce::storage::set_atomic_write_hook({});
  pump();
  EXPECT_EQ(service->boundBusinessDate(), "2026-10-25"); EXPECT_EQ(service->todayJobCount(), 1);
  EXPECT_NE(service->jobRecord(103), nullptr);
  EXPECT_NE(bytes(bucket), original);
}

TEST_F(BillingServiceSafety, CorruptNextDayDoesNotReplacePreviousDayState)
{
  addJob(101); service->flushSave(); const auto original = bytes(bucket);
  const auto next = root / "2026-10-25/orders.json";
  fs::create_directories(next.parent_path()); { std::ofstream out(next); out << "{broken"; }
  today = "2026-10-25";
  EXPECT_FALSE(service->synchronizeBusinessDay()); EXPECT_EQ(service->todayJobCount(), 1);
  EXPECT_EQ(bytes(bucket), original); EXPECT_EQ(bytes(next), "{broken");
}

TEST_F(BillingServiceSafety, ClearPreservesSettledHistory)
{
  addJob(101); const auto task = active(); const auto order = service->settle(task); ASSERT_FALSE(order.isEmpty());
  addJob(102); service->clear(); service->load();
  EXPECT_EQ(service->settledBatchCount(), 1); EXPECT_EQ(service->todayJobCount(), 1);
  EXPECT_EQ(service->settle(task), order);
}

TEST_F(BillingServiceSafety, DuplicateAliasAfterSettlementCannotRepriceFrozenJob)
{
  addJob(101); const auto task = active(); ASSERT_FALSE(service->settle(task).isEmpty());
  const auto frozen = service->jobRecord(101)->pricing;
  auto config = wpce::common::AppConfig::defaults().pricing;
  for (auto& rule : config.rules) rule.price_per_sheet = 1000;
  service->setPricingConfig(&config);
  wpce::core::JobSnapshot duplicate;
  duplicate.job_id = 909; duplicate.idempotency_key = "physical-101";
  duplicate.printer_name = "Test"; duplicate.document_name = "file.pdf";
  duplicate.user = "alice"; duplicate.submit_time_utc = today + "T01:00:00Z";
  service->onJobSnapshot(duplicate);
  wpce::core::MetadataSnapshot changed; changed.job_id = 909;
  changed.total_pages = {"500", wpce::core::FieldState::Reported};
  service->onMetadata(changed);
  EXPECT_EQ(service->todayJobCount(), 1);
  EXPECT_DOUBLE_EQ(service->jobRecord(101)->pricing.total_price, frozen.total_price);
  EXPECT_EQ(service->jobRecord(101)->metadata.total_pages.value, "2");
  service->setPricingConfig(nullptr);
}

TEST_F(BillingServiceSafety, FailedReloadRetainsUnsavedMemory)
{
  addJob(101); service->flushSave(); ASSERT_TRUE(service->setManualPrice(101, 7));
  wpce::storage::set_atomic_write_hook([this](auto stage, const fs::path& path) {
    if (path == bucket && stage == wpce::storage::AtomicWriteStage::Replace) throw std::runtime_error("locked");
  });
  service->load();
  EXPECT_DOUBLE_EQ(service->jobRecord(101)->pricing.total_price, 7);
  EXPECT_TRUE(service->hasPendingSave());
}

TEST_F(BillingServiceSafety, RestoredCountersDoNotReusePersistedTaskOrOrderIds)
{
  addJob(101); const auto task = active(); const auto order = service->settle(task); ASSERT_FALSE(order.isEmpty());
  const auto counterPath = root / "_state/counters.json";
  { std::ofstream out(counterPath); out << R"({"last_date_yyyymmdd":"20261024","batch_counter":0,"order_counter":0})"; }
  service->setStateDirectory(QString::fromStdWString((root / "_state").wstring())); service->load();
  addJob(102); EXPECT_NE(active(), task);
  EXPECT_NE(service->settle(active()), order);
}

TEST_F(BillingServiceSafety, DeferredEventsKeepOriginalBusinessDateAcrossLongOutage)
{
  addJob(101); service->flushSave(); ASSERT_TRUE(service->setManualPrice(101, 3));
  wpce::storage::set_atomic_write_hook([this](auto stage, const fs::path& path) {
    if (path == bucket && stage == wpce::storage::AtomicWriteStage::Replace) throw std::runtime_error("offline disk");
  });
  today = "2026-10-25"; addJob(201);
  today = "2026-10-26"; addJob(301);
  wpce::storage::set_atomic_write_hook({});
  pump(); service->flushSave();
  for (const auto& [date, id] : std::vector<std::pair<std::string, std::uint32_t>>{{"2026-10-25",201},{"2026-10-26",301}})
  {
    wpce::domain::BillingBatchRepository repository; repository.setFilePath(root / date / "orders.json");
    std::vector<wpce::domain::BillingBatch> tasks;
    std::unordered_map<std::uint32_t, wpce::domain::PrintJobRecord> jobs;
    std::vector<wpce::core::SettlementOrder> orders; std::string saved;
    ASSERT_TRUE(repository.loadDaily(tasks, jobs, orders, saved).ok());
    EXPECT_EQ(saved, date); ASSERT_EQ(jobs.size(), 1u); EXPECT_TRUE(jobs.contains(id));
  }
}

TEST_F(BillingServiceSafety, ReusedIdWithNewPhysicalIdentityIsAcceptedAfterRollover)
{
  addJob(101); service->flushSave(); today = "2026-10-25";
  ASSERT_TRUE(service->synchronizeBusinessDay());
  wpce::core::JobSnapshot newJob; newJob.job_id = 101; newJob.external_job_id = 101;
  newJob.idempotency_key = "new-physical-job"; newJob.user = "bob";
  newJob.printer_name = "Test"; newJob.document_name = "new.pdf"; newJob.submit_time_utc = today + "T02:00:00Z";
  service->onJobSnapshot(newJob);
  ASSERT_EQ(service->todayJobCount(), 1);
  EXPECT_EQ(service->jobRecord(101)->snapshot.user, "bob");
  EXPECT_TRUE(active().startsWith("B-20261025-"));
}

TEST_F(BillingServiceSafety, ShutdownPersistsWithoutWaitingForDebounce)
{
  addJob(101); ASSERT_TRUE(service->setManualPrice(101, 7));
  EXPECT_TRUE(service->prepareShutdown().ready);
  service->load(); ASSERT_NE(service->jobRecord(101), nullptr);
  EXPECT_DOUBLE_EQ(service->jobRecord(101)->pricing.total_price, 7);
}
TEST_F(BillingServiceSafety, ShutdownFailureRetainsMemoryAndCanRetry)
{
  addJob(101); service->flushSave(); const auto old = bytes(bucket);
  ASSERT_TRUE(service->setManualPrice(101, 8));
  wpce::storage::set_atomic_write_hook([this](auto stage, const fs::path& path) {
    if (path == bucket && stage == wpce::storage::AtomicWriteStage::Replace) throw std::runtime_error("locked");
  });
  EXPECT_FALSE(service->prepareShutdown().ready); EXPECT_EQ(bytes(bucket), old);
  EXPECT_DOUBLE_EQ(service->jobRecord(101)->pricing.total_price, 8);
  wpce::storage::set_atomic_write_hook({});
  EXPECT_TRUE(service->prepareShutdown().ready); service->load();
  EXPECT_DOUBLE_EQ(service->jobRecord(101)->pricing.total_price, 8);
}
TEST_F(BillingServiceSafety, ShutdownDrainsOriginalDatesBeforeNextTimerTick)
{
  addJob(101); service->flushSave(); ASSERT_TRUE(service->setManualPrice(101, 3));
  wpce::storage::set_atomic_write_hook([this](auto stage, const fs::path& path) {
    if (path == bucket && stage == wpce::storage::AtomicWriteStage::Replace) throw std::runtime_error("offline");
  });
  today="2026-10-25"; addJob(201);
  today="2026-10-26"; addJob(301);
  ASSERT_GT(service->deferredEventCount(),0u);
  EXPECT_FALSE(service->prepareShutdown().ready);
  wpce::storage::set_atomic_write_hook({});
  EXPECT_TRUE(service->prepareShutdown().ready); EXPECT_EQ(service->deferredEventCount(),0u);
  for (const auto& [day,id] : std::vector<std::pair<std::string,std::uint32_t>>{{"2026-10-25",201},{"2026-10-26",301}})
  {
    wpce::domain::BillingBatchRepository repo; repo.setFilePath(root/day/"orders.json");
    std::vector<wpce::domain::BillingBatch> tasks;
    std::unordered_map<std::uint32_t,wpce::domain::PrintJobRecord> jobs;
    std::vector<wpce::core::SettlementOrder> orders; std::string saved;
    ASSERT_TRUE(repo.loadDaily(tasks,jobs,orders,saved).ok());
    EXPECT_EQ(saved,day); EXPECT_TRUE(jobs.contains(id));
    EXPECT_EQ(jobs.at(id).metadata.total_pages.value,"2");
  }
}
TEST_F(BillingServiceSafety, RecoveryExportPreservesIdentityDatesAndManualMoney)
{
  addJob(101); ASSERT_TRUE(service->setManualPrice(101,9));
  wpce::storage::set_atomic_write_hook([this](auto stage, const fs::path& path) {
    if (path == bucket && stage == wpce::storage::AtomicWriteStage::Replace) throw std::runtime_error("offline");
  });
  today="2026-10-25"; addJob(201);
  const auto count=service->deferredEventCount();
  const auto target=root/u8"恢复数据.json"; QString error;
  ASSERT_TRUE(service->exportRecovery(QString::fromStdWString(target.wstring()), &error, {"capture write failed"}));
  const auto json=QJsonDocument::fromJson(QByteArray::fromStdString(bytes(target))).object();
  EXPECT_EQ(json["format"].toString(),"wpce-shutdown-recovery");
  EXPECT_EQ(json["capture_errors"].toArray()[0].toString(),"capture write failed");
  const auto pending=json["deferred_events"].toArray(); ASSERT_EQ(pending.size(),static_cast<qsizetype>(count));
  EXPECT_EQ(pending[0].toObject()["business_date"].toString(),"2026-10-25");
  EXPECT_EQ(pending[0].toObject()["payload"].toObject()["idempotency_key"].toString(),"physical-201");
  EXPECT_DOUBLE_EQ(json["jobs"].toArray()[0].toObject()["pricing"].toObject()["total_price"].toDouble(),9);
  EXPECT_EQ(service->deferredEventCount(),count); EXPECT_FALSE(service->prepareShutdown().ready);
  EXPECT_FALSE(service->exportRecovery(QString::fromStdWString(target.wstring()), &error));
  EXPECT_FALSE(service->exportRecovery(QString::fromStdWString(bucket.wstring()), &error));
}
TEST_F(BillingServiceSafety, CounterFailureDefersJobAndShutdownRetriesIt)
{
  const auto counter=root/"_state/counters.json";
  wpce::storage::set_atomic_write_hook([counter](auto stage, const fs::path& path) {
    if(path==counter && stage==wpce::storage::AtomicWriteStage::Replace) throw std::runtime_error("counter locked");
  });
  addJob(101); ASSERT_GT(service->deferredEventCount(),0u);
  EXPECT_FALSE(service->prepareShutdown().ready);
  wpce::storage::set_atomic_write_hook({});
  EXPECT_TRUE(service->prepareShutdown().ready); ASSERT_NE(service->jobRecord(101),nullptr);
  EXPECT_EQ(service->jobRecord(101)->metadata.total_pages.value,"2");
}
TEST_F(BillingServiceSafety, UnassignedMetadataCannotBeSilentlyDiscardedAtShutdown)
{
  wpce::core::MetadataSnapshot metadata; metadata.job_id=200; service->onMetadata(metadata);
  EXPECT_FALSE(service->prepareShutdown().ready);
  EXPECT_NE(service->jobRecord(200),nullptr);
}
}
