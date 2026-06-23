/// @file settlement_bridge.h
/// Settlement order creation and persistence bridge.
///
/// This module sits between the backend pricing engine and the frontend UI.
/// It provides pure-data functions (no Qt dependency) so it can be used
/// from both wpce_core and wpce_ui.
///
/// Responsibilities:
///   - Freeze a PricingConfig snapshot at settlement time (JSON string)
///   - Create a SettlementOrder from task + job data + pricing entries
///   - Persist settlement orders as JSON files for audit trail
///
/// Communication contract with the frontend:
///   - The frontend calls createSettlementOrder() when the operator
///     confirms settlement
///   - The resulting SettlementOrder is written to
///     <output_dir>/settlement_orders/<order_id>.json
///   - The frontend can read these files to display settlement history

#pragma once

#include "common/app_config.h"
#include "core/data_contracts.h"

#include <filesystem>
#include <string>
#include <vector>

namespace wpce::bridge
{

/// Settlement bridge — pure static methods, no state needed.
class SettlementBridge
{
public:
  /// Generate the next settlement order ID.
  /// Format: "SETTLE-YYYYMMDD-NNN"
  /// The frontend should maintain the daily counter; this is a stateless
  /// helper that accepts the counter value.
  static std::string generateOrderId(const std::string& date_yyyymmdd,
                                     int daily_counter);

  /// Serialise the current PricingConfig to a JSON snapshot string.
  /// This snapshot is embedded in every SettlementOrder for audit.
  static std::string snapshotPricingConfig(const common::PricingConfig& config);

  /// Create a fully populated SettlementOrder.
  ///
  /// @param order_id         Unique order identifier
  /// @param task_id          Associated UserTask.taskId
  /// @param user_id          The user being settled
  /// @param job_ids          All print job IDs in this order
  /// @param pricing_entries  Per-job pricing breakdown (from pricing engine)
  /// @param pricing_snapshot The frozen pricing config at settlement time
  /// @param period_start_utc Time window start (ISO8601)
  /// @param period_end_utc   Time window end (ISO8601)
  /// @param settled_by       Operator identifier (empty = frontend fills later)
  /// @return                 Immutable settlement order
  static core::SettlementOrder createSettlementOrder(
      const std::string& order_id,
      const std::string& task_id,
      const std::string& user_id,
      const std::vector<std::uint32_t>& job_ids,
      const std::vector<core::JobPricingEntry>& pricing_entries,
      const common::PricingConfig& pricing_snapshot,
      const std::string& period_start_utc,
      const std::string& period_end_utc,
      const std::string& settled_by = {});

  /// Serialise a SettlementOrder to a JSON string.
  static std::string toJson(const core::SettlementOrder& order);

  /// Deserialise a SettlementOrder from a JSON string.
  /// Returns std::nullopt on parse failure.
  static std::optional<core::SettlementOrder> fromJson(const std::string& json);

  /// Write a settlement order to the standard output directory.
  /// Path: <output_dir>/settlement_orders/<order_id>.json
  /// Creates the directory if it doesn't exist.
  /// Returns true on success.
  static bool saveToFile(const core::SettlementOrder& order,
                         const std::filesystem::path& output_dir);

  /// Load a settlement order from its JSON file.
  /// Returns std::nullopt if the file doesn't exist or parse fails.
  static std::optional<core::SettlementOrder> loadFromFile(
      const std::filesystem::path& file_path);
};

}  // namespace wpce::bridge
