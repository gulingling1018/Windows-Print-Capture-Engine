/// @file paper_size_map.h
/// Shared DMPAPER_* → human-readable name mapping table.
///
/// Extracted from collector/metadata_collector.cpp so that both the
/// collector and printer_profile can use it without a circular dependency.

#pragma once

#include <cstdint>
#include <string_view>

namespace wpce::common
{

struct PaperSizeEntry
{
  std::int16_t id;
  std::string_view name;
};

/// Lookup table mapping DMPAPER_* constants to human-readable names.
/// Sorted by DMPAPER ID for readability; linear search is fine for 140 entries.
extern const PaperSizeEntry kPaperSizes[];
extern const std::size_t kPaperSizesCount;

/// Map a dmPaperSize (DMPAPER_*) integer to a human-readable name.
/// Returns an empty string_view when the ID is not found.
std::string_view paper_size_name(std::int16_t dm_paper_size) noexcept;

}  // namespace wpce::common
