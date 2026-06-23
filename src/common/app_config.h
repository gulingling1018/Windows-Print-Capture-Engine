#pragma once

#include "common/printer_profile.h"
#include "core/data_contracts.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace wpce::common
{

// ── Sub-config structures ──────────────────────────────────────────────────

struct StorageConfig
{
  /// Root directory for captured output.
  /// Relative paths resolve relative to the executable directory.
  /// Default: "./data" (portable, next to exe).
  std::string output_root{"./data"};

  /// When true, each capture session creates a timestamped subdirectory
  /// (e.g. "capture-20260620-143052") under output_root.
  bool use_timestamp_subdir{true};

  /// Maximum number of pending snapshot items before backpressure.
  std::size_t queue_capacity{256};

  /// Number of days to keep captured data before automatic cleanup.
  /// 0 = never delete.
  int retention_days{90};
};

struct PricingConfig
{
  // ── Matrix pricing rules ───────────────────────────────────────────
  //
  // The legacy additive fields (single_sided_per_page, color_surcharge,
  // volume_discounts, etc.) have been removed.  All pricing is now
  // matrix-based.  Old config files containing legacy fields are still
  // loadable — the legacy fields are silently ignored.

  /// Per-dimension pricing rules.  When non-empty, the pricing engine
  /// uses matrix-based matching.  When empty, the engine falls back to
  /// the built-in factory defaults (see AppConfig::defaults()).
  std::vector<core::PricingRule> rules;

  /// When true, colour print jobs are NOT auto-priced.  Instead they
  /// are flagged as price_pending and the operator must enter a price
  /// manually in the settlement dialog.  Default: true.
  ///
  /// Note: this only applies to jobs on color-capable printers.  A
  /// "color" job on a B&W printer is downgraded to monochrome pricing
  /// and never flagged as pending, regardless of this setting.
  bool color_price_pending{true};

  /// ISO 4217 currency code or symbol.  Default: "CNY".
  std::string currency{"CNY"};
};

struct MerchantConfig
{
  /// Display name of the print shop / merchant.
  std::string name;

  /// Contact phone number.
  std::string phone;

  /// Physical address or location note.
  std::string address;
};

struct MonitoringConfig
{
  /// Seconds without progress before a job is flagged as stalled.
  /// 0 disables stall detection.
  int stall_timeout_seconds{30};

  /// When true, a performance CSV log is written alongside captured data.
  bool perf_log_enabled{false};
};

struct WindowLayoutConfig
{
  int width{1280};
  int height{760};
};

// ── Master configuration ───────────────────────────────────────────────────

struct AppConfig
{
  StorageConfig storage;
  PricingConfig pricing;
  MerchantConfig merchant;
  MonitoringConfig monitoring;
  WindowLayoutConfig window;

  /// Printer capability profiles for color-aware pricing.
  /// Populated automatically on first launch via
  /// detect_printer_capabilities(); user can override in settings.
  PrinterProfileConfig printers;

  /// Build a config populated with sensible factory defaults,
  /// including the 6 default pricing rules.
  static AppConfig defaults();

  /// Resolve the best configuration file path:
  ///   1. <exe_dir>/wpce_config.json   (portable)
  ///   2. <AppData>/WPCE/wpce_config.json  (per-user)
  /// Returns the portable path if neither exists.
  static std::filesystem::path default_config_path();

  /// Create the default configuration file at `path` if it does not
  /// already exist.  Returns true when the file is present after the call
  /// (pre-existing or newly created).
  static bool ensure_config_file(const std::filesystem::path& path);

  /// Write this configuration to `path` as JSON.  Returns true on success.
  bool save_to_json(const std::filesystem::path& path) const;

  /// Populate this configuration from a JSON file.  Returns true on success.
  /// Missing keys retain their current (typically default) values.
  /// Legacy additive pricing fields (if present in an old file) are
  /// silently ignored.
  bool load_from_json(const std::filesystem::path& path);
};

}  // namespace wpce::common

