/// @file settlement_bridge_tests.cpp
/// Unit tests for the settlement bridge module.

#include "bridge/settlement_bridge.h"
#include "common/app_config.h"
#include "common/pricing_engine.h"
#include "core/data_contracts.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace wpce;

// ── Helpers ───────────────────────────────────────────────────────────────

namespace
{

common::PricingConfig make_test_pricing_config()
{
  common::PricingConfig config;
  config.currency = "CNY";
  config.color_price_pending = true;

  core::PricingRule r1;
  r1.rule_id = "r1"; r1.paper_size = "A4"; r1.duplex_mode = "simplex";
  r1.printer_name = ""; r1.color_mode = "monochrome"; r1.file_extension = "";
  r1.price_per_sheet = 0.15;
  r1.priority = common::rule_specificity(r1);

  core::PricingRule r2;
  r2.rule_id = "r2"; r2.paper_size = ""; r2.duplex_mode = "duplex";
  r2.printer_name = ""; r2.color_mode = "monochrome"; r2.file_extension = "";
  r2.price_per_sheet = 0.25;
  r2.priority = common::rule_specificity(r2);

  config.rules = {r1, r2};
  common::sort_rules_by_priority(config.rules);
  return config;
}

std::filesystem::path test_output_dir()
{
  return std::filesystem::temp_directory_path() / "wpce_test_settlement";
}

}  // namespace

// ── generateOrderId ────────────────────────────────────────────────────────

TEST(SettlementBridge, GenerateOrderId)
{
  // 6-digit zero-padded format (consistent with CounterStore).
  std::string id = bridge::SettlementBridge::generateOrderId("20260622", 1);
  EXPECT_EQ(id, "SETTLE-20260622-000001");

  id = bridge::SettlementBridge::generateOrderId("20260622", 42);
  EXPECT_EQ(id, "SETTLE-20260622-000042");

  id = bridge::SettlementBridge::generateOrderId("20260622", 999);
  EXPECT_EQ(id, "SETTLE-20260622-000999");
}

// ── snapshotPricingConfig ──────────────────────────────────────────────────

TEST(SettlementBridge, SnapshotPricingConfig_ProducesValidJson)
{
  auto config = make_test_pricing_config();
  std::string json = bridge::SettlementBridge::snapshotPricingConfig(config);

  EXPECT_FALSE(json.empty());
  EXPECT_NE(json, "{}");
  EXPECT_NE(json.find("pricing"), std::string::npos);
  EXPECT_NE(json.find("rules"), std::string::npos);
}

// ── createSettlementOrder ──────────────────────────────────────────────────

TEST(SettlementBridge, CreateSettlementOrder)
{
  auto config = make_test_pricing_config();

  std::vector<std::uint32_t> job_ids = {101, 102, 103};
  std::vector<core::JobPricingEntry> entries;

  core::JobPricingEntry e1;
  e1.job_id = 101; e1.total_pages = 3; e1.copies = 1;
  e1.effective_sheets = 3; e1.base_price = 0.45; e1.total_price = 0.45;
  entries.push_back(e1);

  core::JobPricingEntry e2;
  e2.job_id = 102; e2.total_pages = 2; e2.copies = 1;
  e2.effective_sheets = 1; e2.base_price = 0.25; e2.total_price = 0.25;
  e2.price_pending = true; e2.pending_reason = "color";
  entries.push_back(e2);

  auto order = bridge::SettlementBridge::createSettlementOrder(
      "SETTLE-20260622-001", "20260622-003", "alice",
      job_ids, entries, config,
      "2026-06-22T10:00:00Z", "2026-06-22T10:30:00Z",
      "operator1");

  EXPECT_EQ(order.order_id, "SETTLE-20260622-001");
  EXPECT_EQ(order.task_id, "20260622-003");
  EXPECT_EQ(order.user_id, "alice");
  EXPECT_EQ(order.settled_by, "operator1");
  EXPECT_EQ(order.period_start_utc, "2026-06-22T10:00:00Z");
  EXPECT_EQ(order.period_end_utc, "2026-06-22T10:30:00Z");
  EXPECT_EQ(order.currency, "CNY");

  // Total price = 0.45 + 0.25 = 0.70
  EXPECT_DOUBLE_EQ(order.total_price, 0.70);

  // Verify job_ids preserved.
  ASSERT_EQ(order.job_ids.size(), 3u);
  EXPECT_EQ(order.job_ids[0], 101u);
  EXPECT_EQ(order.job_ids[2], 103u);

  // Verify pricing entries preserved.
  ASSERT_EQ(order.pricing_entries.size(), 2u);
  EXPECT_EQ(order.pricing_entries[0].job_id, 101u);
  EXPECT_TRUE(order.pricing_entries[1].price_pending);

  // Verify pricing snapshot is non-empty.
  EXPECT_FALSE(order.pricing_snapshot_json.empty());

  // Verify settled_at_utc is populated.
  EXPECT_FALSE(order.settled_at_utc.empty());
}

TEST(SettlementBridge, CreateSettlementOrder_EmptySettledBy)
{
  auto config = make_test_pricing_config();
  auto order = bridge::SettlementBridge::createSettlementOrder(
      "SETTLE-20260622-002", "20260622-004", "bob",
      {201}, {}, config,
      "2026-06-22T11:00:00Z", "2026-06-22T11:05:00Z");

  EXPECT_EQ(order.settled_by, ""); // operator not yet filled
  EXPECT_DOUBLE_EQ(order.total_price, 0.0);
}

// ── JSON round-trip ────────────────────────────────────────────────────────

TEST(SettlementBridge, JsonRoundTrip)
{
  auto config = make_test_pricing_config();

  core::JobPricingEntry entry;
  entry.job_id = 301; entry.total_pages = 5; entry.effective_sheets = 5;
  entry.base_price = 0.75; entry.total_price = 0.75;
  entry.price_pending = true; entry.pending_reason = "unmatched_rule";

  auto original = bridge::SettlementBridge::createSettlementOrder(
      "SETTLE-20260622-005", "20260622-005", "charlie",
      {301}, {entry}, config,
      "2026-06-22T08:00:00Z", "2026-06-22T08:15:00Z",
      "op");
  original.first_document_name = "plan-a.pdf";
  original.last_document_name = "plan-z.pdf";

  std::string json = bridge::SettlementBridge::toJson(original);
  EXPECT_FALSE(json.empty());

  auto restored = bridge::SettlementBridge::fromJson(json);
  ASSERT_TRUE(restored.has_value());

  EXPECT_EQ(restored->order_id, original.order_id);
  EXPECT_EQ(restored->task_id, original.task_id);
  EXPECT_EQ(restored->user_id, original.user_id);
  EXPECT_EQ(restored->settled_by, original.settled_by);
  EXPECT_EQ(restored->settled_at_utc, original.settled_at_utc);
  EXPECT_EQ(restored->period_start_utc, original.period_start_utc);
  EXPECT_EQ(restored->period_end_utc, original.period_end_utc);
  EXPECT_EQ(restored->first_document_name, original.first_document_name);
  EXPECT_EQ(restored->last_document_name, original.last_document_name);
  EXPECT_EQ(restored->currency, original.currency);
  EXPECT_DOUBLE_EQ(restored->total_price, original.total_price);
  ASSERT_EQ(restored->job_ids.size(), original.job_ids.size());
  EXPECT_EQ(restored->job_ids[0], original.job_ids[0]);
  ASSERT_EQ(restored->pricing_entries.size(), original.pricing_entries.size());
  EXPECT_EQ(restored->pricing_entries[0].job_id, entry.job_id);
  EXPECT_EQ(restored->pricing_entries[0].price_pending, entry.price_pending);
  EXPECT_EQ(restored->pricing_entries[0].pending_reason, entry.pending_reason);
  EXPECT_FALSE(restored->pricing_snapshot_json.empty());
}

TEST(SettlementBridge, FromJson_InvalidJson)
{
  auto result = bridge::SettlementBridge::fromJson("not valid json");
  EXPECT_FALSE(result.has_value());

  result = bridge::SettlementBridge::fromJson("");
  EXPECT_FALSE(result.has_value());
}

// ── File save / load ───────────────────────────────────────────────────────

TEST(SettlementBridge, SaveAndLoadFile)
{
  auto config = make_test_pricing_config();
  auto order = bridge::SettlementBridge::createSettlementOrder(
      "SETTLE-20260622-010", "20260622-010", "testuser",
      {401}, {}, config,
      "2026-06-22T09:00:00Z", "2026-06-22T09:10:00Z");

  auto out_dir = test_output_dir();

  // Clean up before test.
  std::filesystem::remove_all(out_dir);

  bool saved = bridge::SettlementBridge::saveToFile(order, out_dir);
  EXPECT_TRUE(saved);

  // Verify file exists.
  auto file_path = out_dir / "settlement_orders" / "SETTLE-20260622-010.json";
  EXPECT_TRUE(std::filesystem::exists(file_path));

  // Load and verify.
  auto loaded = bridge::SettlementBridge::loadFromFile(file_path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->order_id, order.order_id);
  EXPECT_EQ(loaded->user_id, order.user_id);

  // Clean up.
  std::filesystem::remove_all(out_dir);
}

TEST(SettlementBridge, RejectsEmptyOrderIdOnSave)
{
  auto config = make_test_pricing_config();
  auto order = bridge::SettlementBridge::createSettlementOrder(
      "", "20260622-011", "testuser",
      {402}, {}, config,
      "2026-06-22T09:00:00Z", "2026-06-22T09:10:00Z");

  auto out_dir = test_output_dir();
  std::filesystem::remove_all(out_dir);

  EXPECT_FALSE(bridge::SettlementBridge::saveToFile(order, out_dir));
  EXPECT_FALSE(bridge::SettlementBridge::saveToDateDir(
      order, out_dir / "history", "2026-06-22", "a.pdf", "z.pdf"));
  EXPECT_FALSE(std::filesystem::exists(out_dir / "settlement_orders" / ".json"));
  EXPECT_FALSE(std::filesystem::exists(out_dir / "history" / "2026-06-22" / ".json"));

  std::filesystem::remove_all(out_dir);
}

TEST(SettlementBridge, LoadFromFile_NonExistent)
{
  auto result = bridge::SettlementBridge::loadFromFile(
      test_output_dir() / "nonexistent.json");
  EXPECT_FALSE(result.has_value());
}
