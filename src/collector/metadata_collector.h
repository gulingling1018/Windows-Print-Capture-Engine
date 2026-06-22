#pragma once

#include "core/data_contracts.h"

#ifdef _WIN32
#include <windows.h>
#include <winspool.h>
#endif

#include <cstdint>
#include <string>
#include <string_view>

namespace wpce::collector
{

// ── DEVMODE field extraction ───────────────────────────────────────────

/// Map dmPaperSize (DMPAPER_*) integer to a human-readable name.
std::string_view paper_size_name(std::int16_t dm_paper_size) noexcept;

/// Map dmDuplex value to a human-readable name.
std::string_view duplex_name(std::int16_t dm_duplex) noexcept;

/// Map dmColor value to a human-readable name.
std::string_view color_name(std::int16_t dm_color) noexcept;

/// Map dmOrientation value to a human-readable name.
std::string_view orientation_name(std::int16_t dm_orientation) noexcept;

/// Map dmNup value to a human-readable description.
std::string_view n_up_name(std::int32_t dm_nup) noexcept;

/// Map dmCollate value to a human-readable name.
std::string_view collate_name(std::int16_t dm_collate) noexcept;

/// Map dmPrintQuality value to a human-readable name.
std::string_view print_quality_name(std::int16_t dm_print_quality) noexcept;

// ── Metadata extraction ────────────────────────────────────────────────

#ifdef _WIN32
/// Build a MetadataSnapshot from JOB_INFO_2W.
///
/// Reads TotalPages and PagesPrinted directly from the job info.
/// Reads pDevMode (DEVMODEW) for copies, duplex, color, paper size,
/// orientation, N-up, collate, and print quality.
/// Fields that are not reported by the driver (dmFields) are
/// marked as FieldState::NotReported.  Unrecognised enum values are mapped
/// to FieldState::Unknown with the raw integer stored as the value.
/// The raw dmFields bitmask is stored as hex in dm_fields_hex.
core::MetadataSnapshot extract_metadata(const JOB_INFO_2W& job);

#endif

}  // namespace wpce::collector
