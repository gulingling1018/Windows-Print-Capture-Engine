/// @file counter_store_tests.cpp
/// Regression tests for persistent billing counters.

#include "common/counter_store.h"
#include "common/business_clock.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>

using namespace wpce;

namespace
{

std::filesystem::path make_test_counter_path()
{
  const auto stamp = std::chrono::steady_clock::now()
                       .time_since_epoch()
                       .count();
  return std::filesystem::temp_directory_path() /
         ("wpce_counter_store_test_" + std::to_string(stamp)) /
         "counters.json";
}

}  // namespace

TEST(BusinessClock, BeijingFallbackUsesUtcPlusEight)
{
  // 1999-12-31 17:00:00 UTC -> 2000-01-01 01:00:00 in China.
  const auto date = common::business_date_from_time(946659600, true);
  EXPECT_EQ(date.yyyy_mm_dd, "2000-01-01");
  EXPECT_EQ(date.yyyymmdd, "20000101");
  EXPECT_TRUE(date.used_beijing_fallback);
}

TEST(CounterStore, NextIdsPersistAndReturn)
{
  const auto path = make_test_counter_path();
  const std::string today = common::CounterStore::currentDateYyyymmdd();

  common::CounterStore store;
  store.setStatePath(path);

  EXPECT_EQ(store.nextBatchId(), "B-" + today + "-000001");
  EXPECT_EQ(store.nextOrderId(), "SETTLE-" + today + "-000001");
  EXPECT_TRUE(std::filesystem::exists(path));

  common::CounterStore reloaded;
  reloaded.setStatePath(path);

  EXPECT_EQ(reloaded.nextBatchId(), "B-" + today + "-000002");
  EXPECT_EQ(reloaded.nextOrderId(), "SETTLE-" + today + "-000002");

  std::error_code ignored;
  std::filesystem::remove_all(path.parent_path(), ignored);
}

TEST(CounterStore, NextIdsFailClosedWhenStatePathCannotBeReplaced)
{
  const auto path = make_test_counter_path();
  std::filesystem::create_directories(path);

  common::CounterStore store;
  store.setStatePath(path);

  EXPECT_EQ(store.nextBatchId(), "");
  EXPECT_EQ(store.nextOrderId(), "");

  std::error_code ignored;
  std::filesystem::remove_all(path.parent_path(), ignored);
}
