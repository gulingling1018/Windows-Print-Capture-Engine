#pragma once

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

struct VolumeDiscountTier
{
  int min_pages{0};
  double discount{1.0}; // 1.0 = full price, 0.85 = 15% off
};

struct PricingConfig
{
  /// Price per page for single-sided (simplex) printing.  Default: 0.15 CNY/page.
  double single_sided_per_page{0.15};

  /// Price per sheet for double-sided (duplex) printing.  Default: 0.25 CNY/sheet.
  double double_sided_per_sheet{0.25};

  /// Additional surcharge per page when printing in colour.
  double color_surcharge{0.50};

  /// Additional surcharge per page for large paper sizes (A3, Tabloid, Ledger).
  double large_paper_surcharge{0.30};

  /// Additional surcharge per page for high-quality / photo mode.
  double high_quality_surcharge{0.10};

  /// Volume discount tiers.  Sorted by min_pages ascending.
  /// e.g. [{100, 0.95}, {500, 0.90}, {1000, 0.85}]
  std::vector<VolumeDiscountTier> volume_discounts;

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

  /// Build a config populated with sensible factory defaults.
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
  bool load_from_json(const std::filesystem::path& path);
};

}  // namespace wpce::common
