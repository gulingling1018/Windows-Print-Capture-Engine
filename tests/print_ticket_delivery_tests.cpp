#include "billing/billing_service.h"
#include "ui/watcher_bridge.h"
#include "bridge/print_ticket_inbox.h"
#include "common/physical_job_identity.h"
#include "common/business_clock.h"
#include <QCoreApplication>
#include <gtest/gtest.h>

namespace wpce::ui
{
class WatcherBridgeInboxTestPeer
{
 public:
  static void start(WatcherBridge& bridge, const std::filesystem::path& output, core::JobSnapshot job)
  {
    bridge.running_ = true;
    bridge.seedStoredJobs({std::move(job)});
    bridge.seedMinSyntheticJobId(101);
    bridge.resetCapturedJobMappings();
    bridge.startPrintTicketInbox(output); // no printer discovery or device API
  }
  static void verifyRestartMapping(WatcherBridge& bridge, const core::JobSnapshot& stored)
  {
    bridge.seedStoredJobs({stored});
    bridge.seedMinSyntheticJobId(stored.job_id + 1);
    for (int session = 0; session < 2; ++session)
    {
      bridge.resetCapturedJobMappings();
      EXPECT_EQ(bridge.capturedSnapshots_.at(stored.job_id).external_job_id, stored.external_job_id);
      EXPECT_EQ(bridge.normalizedJobIdForSnapshot(stored.printer_name, stored.external_job_id, stored), stored.job_id);
      auto reused = stored; reused.submit_time_utc = "2026-10-24T02:00:00";
      EXPECT_NE(bridge.normalizedJobIdForSnapshot(stored.printer_name, stored.external_job_id, reused), stored.job_id);
    }
  }
};
}
namespace
{
class PrintTicketDelivery : public testing::Test
{
 protected:
  int argc{1}; char name[16]{"pt-inbox-test"}; char* argv[2]{name,nullptr};
  std::unique_ptr<QCoreApplication> app;
  std::filesystem::path root, output;
  std::unique_ptr<wpce::ui::BillingService> billing;
  std::unique_ptr<wpce::ui::WatcherBridge> bridge;
  wpce::core::JobSnapshot job;
  bool allow{true};
  void SetUp() override
  {
    app=std::make_unique<QCoreApplication>(argc,argv);
    root=std::filesystem::temp_directory_path()/("wpce-pt-delivery-"+wpce::storage::unique_storage_id());
    output=root/wpce::common::current_business_date().yyyy_mm_dd;
    billing=std::make_unique<wpce::ui::BillingService>();
    billing->setStateDirectory(QString::fromStdWString((root/"_state").wstring()));
    billing->setOrderBucketPath(QString::fromStdWString((output/"orders.json").wstring()));
    job.job_id=100; job.external_job_id=42; job.printer_name="Test"; job.user="alice";
    job.document_name="file.pdf"; job.submit_time_utc="2026-10-24T01:00:00";
    billing->onJobSnapshot(job); billing->flushSave();
  }
  void start()
  {
    // Explicitly disable process/machine inbox discovery in this isolated test.
    bridge=std::make_unique<wpce::ui::WatcherBridge>(nullptr,false);
    QObject::connect(bridge.get(),&wpce::ui::WatcherBridge::metadataReceived,billing.get(),&wpce::ui::BillingService::onMetadata);
    bridge->setPrintTicketAcknowledgement([this](std::uint32_t id) {
      if(!allow) return false;
      billing->flushSave();
      return billing->storageReady() && !billing->hasPendingSave() && billing->jobRecord(id);
    });
    wpce::ui::WatcherBridgeInboxTestPeer::start(*bridge,output,job);
  }
  void put()
  {
    wpce::bridge::PrintTicketMessage message;
    message.message_id="known-message"; message.physical_identity=wpce::common::physical_job_identity(job);
    message.metadata.job_id=42; message.metadata.printer_name=job.printer_name;
    message.metadata.copies={"2",wpce::core::FieldState::Reported};
    wpce::storage::write_text_file_atomic(wpce::bridge::serialize_pt_message(message),output/"known.pt-message.json");
  }
  bool wait(bool pump, const std::function<bool()>& predicate)
  {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(6);
    while(!predicate() && std::chrono::steady_clock::now()<deadline)
    { if(pump) QCoreApplication::processEvents(); QThread::msleep(10); }
    return predicate();
  }
  bool acked() const
  { return std::filesystem::exists(output/"_pt-acks"/(wpce::storage::stable_digest("known-message")+".ack")); }
  void TearDown() override
  {
    if(bridge) (void)bridge->stopWatching(); bridge.reset(); billing.reset(); app.reset();
    std::error_code error; std::filesystem::remove_all(root,error);
  }
};
TEST_F(PrintTicketDelivery, BackgroundClaimAwaitsGuiPersistenceAndStopsWithoutDeadlock)
{
  put(); start();
  EXPECT_TRUE(wait(false,[this] {return std::filesystem::exists(output/"known.pt-message.json.claim");}));
  EXPECT_FALSE(acked());
  EXPECT_TRUE(bridge->stopWatching()); // joining cannot wait for GUI delivery
  EXPECT_EQ(billing->jobRecord(100)->metadata.copies.value,"2"); EXPECT_TRUE(acked());
  EXPECT_TRUE(billing->prepareShutdown().ready);
}
TEST_F(PrintTicketDelivery, FailedBusinessAcknowledgementKeepsInputAndRestartRetries)
{
  put(); allow=false; start();
  EXPECT_TRUE(wait(true,[this] {return std::filesystem::exists(output/"known.pt-message.json.claim");}));
  EXPECT_TRUE(bridge->stopWatching()); EXPECT_FALSE(acked()); bridge.reset();
  allow=true; start(); EXPECT_TRUE(wait(true,[this] {return acked();}));
  EXPECT_EQ(billing->jobRecord(100)->metadata.copies.value,"2");
  EXPECT_FALSE(std::filesystem::exists(output/"known.pt-message.json.claim"));
}
TEST_F(PrintTicketDelivery, StoredPhysicalIdentitySurvivesSessionResetAndRawIdReuse)
{
  wpce::ui::WatcherBridge isolated(nullptr, false);
  wpce::ui::WatcherBridgeInboxTestPeer::verifyRestartMapping(isolated, job);
}
}
