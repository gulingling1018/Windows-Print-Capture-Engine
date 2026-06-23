/// @file printer_profile.h
/// Printer capability profile for color-aware pricing.
///
/// In real print-shop workflows, color vs monochrome printing is
/// separated by physical printer — the shop has dedicated B&W
/// printers and dedicated color printers.  A "color" job sent to a
/// B&W printer is actually printed in monochrome, so the pricing
/// engine must downgrade it to the monochrome rate instead of
/// flagging it as price_pending.
///
/// This module provides:
///   - PrinterProfile: per-printer color capability record
///   - PrinterProfileConfig: collection of profiles + default fallback
///   - detect_printer_capabilities(): Win32 probe of local printers
///   - lookup_color_capability(): fast query for the pricing engine
///
/// Configuration persistence is handled by AppConfig (this header
/// only defines the data structures and pure query logic).

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wpce::common
{

struct PrinterProfile
{
  /// Exact printer name as reported by the spooler.
  /// Matching is case-insensitive against metadata.printer_name.
  std::string printer_name;

  /// True if this printer is a color-capable device.
  /// False → color jobs sent here are downgraded to monochrome pricing.
  bool is_color_capable{false};
};

struct PrinterProfileConfig
{
  std::vector<PrinterProfile> profiles;

  /// Conservative fallback when a printer has no explicit profile.
  /// Default false: unknown printers are assumed monochrome so the
  /// shop never overcharges a customer for a job that was actually
  /// printed in B&W.
  bool default_color_capable{false};
};

/// Determine whether a given printer is color-capable according to
/// the supplied profiles.  Falls back to default_color_capable when
/// no profile matches the printer name.
bool lookup_color_capability(
    const PrinterProfileConfig& config,
    std::string_view printer_name) noexcept;

/// Detect color capability for all locally-installed printers via
/// Win32 `DeviceCapabilitiesW(DC_COLORDEVICE)`.  Returns a config
/// prepopulated with one PrinterProfile per detected printer.
/// On non-Windows platforms returns an empty config.
PrinterProfileConfig detect_printer_capabilities();

/// Case-insensitive name match (used by lookup_color_capability).
bool printer_name_matches(std::string_view a, std::string_view b) noexcept;

}  // namespace wpce::common
