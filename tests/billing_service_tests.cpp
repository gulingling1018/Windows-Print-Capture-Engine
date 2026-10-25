#include "billing/billing_service.h"
#include "common/business_clock.h"
#include "domain/billing_batch_repository.h"

#include <QCoreApplication>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

namespace
{

std::filesystem::path unique_test_root()
{
  const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("wpce-billing-idempotency-" + std::to_string(token));
}

wpce::core::JobSnapshot make_snapshot(std::uint32_t internalId)
{
  wpce::core::JobSnapshot snapshot;
  snapshot.job_id = internalId;
  snapshot.external_job_id = 42;
  snapshot.idempotency_key = std::string("Test Printer") + '\x1f' + "42" +
                             '\x1f' + "2026-08-02T01:02:03Z";
  snapshot.user = "DOMAIN\\alice";
  snapshot.document_name = "same-physical-job.pdf";
  snapshot.printer_name = "Test Printer";
  snapshot.submit_time_utc = "2026-08-02T01:02:03Z";
  snapshot.status = "printing";
  return snapshot;
}

wpce::core::MetadataSnapshot make_metadata(std::uint32_t internalId)
{
  wpce::core::MetadataSnapshot metadata;
  metadata.job_id = internalId;
  metadata.printer_name = "Test Printer";
  metadata.total_pages = {"2", wpce::core::FieldState::Reported};
  metadata.copies = {"1", wpce::core::FieldState::Reported};
  metadata.duplex = {"simplex", wpce::core::FieldState::Reported};
  metadata.color = {"monochrome", wpce::core::FieldState::Reported};
  metadata.paper_size = {"A4", wpce::core::FieldState::Reported};
  metadata.file_extension = {"pdf", wpce::core::FieldState::Reported};
  return metadata;
}

}  // namespace

TEST(BillingService, RepeatedListenerEventsAndSettlementAreIdempotent)
{
  int argc = 1;
  char appName[] = "wpce_billing_service_tests";
  char* argv[] = {appName, nullptr};
  std::unique_ptr<QCoreApplication> ownedApp;
  if (QCoreApplication::instance() == nullptr)
    ownedApp = std::make_unique<QCoreApplication>(argc, argv);

  const auto root = unique_test_root();
  const auto date = wpce::common::current_business_date().yyyy_mm_dd;
  const auto bucket = root / date / "orders.json";
  std::filesystem::create_directories(root / "_state");

  {
    wpce::ui::BillingService service;
    service.setStateDirectory(QString::fromStdWString((root / "_state").wstring()));
    service.setOrderBucketPath(QString::fromStdWString(bucket.wstring()));

    service.onJobSnapshot(make_snapshot(100));
    service.onMetadata(make_metadata(100));

    // The same physical spool job is observed again under another internal
    // listener ID after a status transition/reconnect.
    service.onJobSnapshot(make_snapshot(200));
    service.onMetadata(make_metadata(200));

    ASSERT_EQ(service.todayJobCount(), 1);
    ASSERT_NE(service.currentActiveBatch(), nullptr);
    ASSERT_EQ(service.currentActiveBatch()->job_ids.size(), 1u);

    const QString taskId = QString::fromStdString(service.currentActiveBatch()->batch_id);
    const QString firstOrderId = service.settle(taskId);
    ASSERT_FALSE(firstOrderId.isEmpty());
    EXPECT_EQ(service.settle(taskId), firstOrderId);
    service.flushSave();
  }

  wpce::domain::BillingBatchRepository repository;
  repository.setFilePath(bucket);
  std::vector<wpce::domain::BillingBatch> tasks;
  std::unordered_map<std::uint32_t, wpce::domain::PrintJobRecord> jobs;
  std::vector<wpce::core::SettlementOrder> orders;
  std::string savedDate;
  ASSERT_TRUE(repository.load(tasks, jobs, orders, savedDate));
  EXPECT_EQ(savedDate, date);
  ASSERT_EQ(tasks.size(), 1u);
  ASSERT_EQ(jobs.size(), 1u);
  ASSERT_EQ(orders.size(), 1u);
  EXPECT_EQ(orders.front().task_id, tasks.front().batch_id);

  std::filesystem::remove_all(root);
}

TEST(BillingService, RepeatedMergeSettlementReturnsOriginalOrder)
{
  int argc = 1;
  char appName[] = "wpce_billing_merge_tests";
  char* argv[] = {appName, nullptr};
  std::unique_ptr<QCoreApplication> ownedApp;
  if (QCoreApplication::instance() == nullptr)
    ownedApp = std::make_unique<QCoreApplication>(argc, argv);

  const auto root = unique_test_root();
  const auto date = wpce::common::current_business_date().yyyy_mm_dd;
  const auto bucket = root / date / "orders.json";
  std::filesystem::create_directories(root / "_state");

  {
    wpce::ui::BillingService service;
    service.setStateDirectory(QString::fromStdWString((root / "_state").wstring()));
    service.setOrderBucketPath(QString::fromStdWString(bucket.wstring()));

    service.onJobSnapshot(make_snapshot(301));
    service.onMetadata(make_metadata(301));
    ASSERT_NE(service.currentActiveBatch(), nullptr);
    const QString firstTask = QString::fromStdString(service.currentActiveBatch()->batch_id);
    ASSERT_TRUE(service.suspend(firstTask));

    auto secondSnapshot = make_snapshot(302);
    secondSnapshot.external_job_id = 43;
    secondSnapshot.idempotency_key += "-different-physical-job";
    secondSnapshot.document_name = "second-job.pdf";
    service.onJobSnapshot(std::move(secondSnapshot));
    service.onMetadata(make_metadata(302));
    ASSERT_NE(service.currentActiveBatch(), nullptr);
    const QString secondTask = QString::fromStdString(service.currentActiveBatch()->batch_id);
    ASSERT_TRUE(service.suspend(secondTask));

    const QStringList taskIds{firstTask, secondTask};
    const QString firstOrder = service.mergeSettle(taskIds);
    ASSERT_FALSE(firstOrder.isEmpty());
    EXPECT_EQ(service.mergeSettle(taskIds), firstOrder);
    service.flushSave();
  }

  wpce::domain::BillingBatchRepository repository;
  repository.setFilePath(bucket);
  std::vector<wpce::domain::BillingBatch> tasks;
  std::unordered_map<std::uint32_t, wpce::domain::PrintJobRecord> jobs;
  std::vector<wpce::core::SettlementOrder> orders;
  std::string savedDate;
  ASSERT_TRUE(repository.load(tasks, jobs, orders, savedDate));
  EXPECT_EQ(tasks.size(), 2u);
  EXPECT_EQ(jobs.size(), 2u);
  EXPECT_EQ(orders.size(), 1u);

  std::filesystem::remove_all(root);
}
