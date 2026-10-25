#include "billing/billing_service.h"
#include "ui/watcher_bridge.h"
#include "ui/worker_shutdown.h"
#include <QCoreApplication>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>

TEST(ShutdownBridge, TimeoutRetainsWorkerAndRetryJoinsWithoutTermination)
{
  std::atomic_bool release{false}, finished{false};
  auto* worker=QThread::create([&] { while (!release) QThread::msleep(1); finished=true; });
  std::vector<QThread*> workers{worker}; worker->start();
  EXPECT_FALSE(wpce::ui::join_worker_threads(workers,1));
  ASSERT_EQ(workers.size(),1u); EXPECT_FALSE(finished);
  release=true;
  EXPECT_TRUE(wpce::ui::join_worker_threads(workers,1000));
  EXPECT_TRUE(finished); EXPECT_TRUE(workers.empty());
}
TEST(ShutdownBridge, StopDeliversAlreadyQueuedSignalsBeforeBillingFlush)
{
  int argc=1; char name[]="shutdown-test"; char* argv[]={name,nullptr};
  QCoreApplication app(argc,argv);
  const auto root=std::filesystem::temp_directory_path()/ ("wpce-bridge-shutdown-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  {
    wpce::ui::BillingService billing;
    wpce::ui::WatcherBridge bridge;
    billing.setBusinessDateProvider([] { return "2026-10-24"; });
    billing.setStateDirectory(QString::fromStdWString((root/"_state").wstring()));
    billing.setOrderBucketPath(QString::fromStdWString((root/"2026-10-24/orders.json").wstring()));
    QObject::connect(&bridge,&wpce::ui::WatcherBridge::jobSnapshotReceived,&billing,&wpce::ui::BillingService::onJobSnapshot);
    wpce::core::JobSnapshot job; job.job_id=123; job.idempotency_key="physical-123"; job.document_name="test.pdf";
    QMetaObject::invokeMethod(&bridge,[&] { emit bridge.jobSnapshotReceived(job); },Qt::QueuedConnection);
    EXPECT_EQ(billing.todayJobCount(),0);
    EXPECT_TRUE(bridge.stopWatching());
    EXPECT_EQ(billing.todayJobCount(),1);
    EXPECT_TRUE(billing.prepareShutdown().ready);
    EXPECT_TRUE(bridge.stopWatching());
  }
  std::error_code error; std::filesystem::remove_all(root,error);
}
