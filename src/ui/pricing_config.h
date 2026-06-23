#pragma once

#include "common/app_config.h"
#include "core/data_contracts.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace wpce::ui
{

/// Pricing configuration for print cost calculation.
/// This is a thin UI-layer wrapper around common::PricingConfig.
using PricingConfig = common::PricingConfig;

/// Determine whether a metadata snapshot indicates double-sided printing.
inline bool is_duplex_job(const core::MetadataSnapshot& metadata)
{
  const std::string& duplex = metadata.duplex.value;
  if (duplex.empty() || duplex == "simplex")
  {
    return false;
  }
  if (duplex == "vertical" ||
      duplex == "horizontal" ||
      duplex == "duplex" ||
      duplex == "long-edge (book)" ||
      duplex == "short-edge (calendar)" ||
      duplex == "TwoSidedLongEdge" ||
      duplex == "TwoSidedShortEdge")
  {
    return true;
  }
  return false;
}

/// Determine whether the paper size is "large" (A3, Tabloid, Ledger).
inline bool is_large_paper(const core::MetadataSnapshot& metadata)
{
  const std::string& paper = metadata.paper_size.value;
  if (paper.empty()) return false;

  const auto icase_equals = [](const std::string& a, const std::string& b)
  {
    if (a.size() != b.size()) return false;
    return std::equal(a.begin(), a.end(), b.begin(),
                      [](char ca, char cb) { return std::tolower(ca) == std::tolower(cb); });
  };

  return icase_equals(paper, "A3") ||
         icase_equals(paper, "a3") ||
         icase_equals(paper, "Tabloid") ||
         icase_equals(paper, "Ledger") ||
         icase_equals(paper, "Legal");
}

/// Determine whether the print quality setting indicates high quality.
inline bool is_high_quality(const core::MetadataSnapshot& metadata)
{
  const std::string& quality = metadata.print_quality.value;
  if (quality.empty()) return false;
  return quality == "high" ||
         quality == "1200 dpi" ||
         quality == "2400 dpi" ||
         quality == "4800 dpi";
}

/// Determine whether printing is in colour.
inline bool is_color_job(const core::MetadataSnapshot& metadata)
{
  const std::string& color = metadata.color.value;
  return color == "color";
}

/// Parse the copies count from the metadata, defaulting to 1 when not
/// reported.  Never returns less than 1.
inline int parse_copies(const core::MetadataSnapshot& metadata)
{
  const auto& f = metadata.copies;
  if (f.state != core::FieldState::Reported || f.value.empty())
    return 1;
  try
  {
    int n = std::stoi(f.value);
    return n > 0 ? n : 1;
  }
  catch (...)
  {
    return 1;
  }
}

/// Parse the total logical page count (pages in the document, before
/// applying copies).  Returns 0 when not reported.
inline int parse_total_pages(const core::MetadataSnapshot& metadata)
{
  const auto& f = metadata.total_pages;
  if (f.state != core::FieldState::Reported || f.value.empty())
    return 0;
  try { return std::stoi(f.value); }
  catch (...) { return 0; }
}

/// Estimate the number of physical sheets the job will consume,
/// accounting for duplex and copies.
inline int estimate_physical_sheets(const core::MetadataSnapshot& metadata)
{
  const int logical = parse_total_pages(metadata);
  if (logical <= 0) return 0;
  const int copies = parse_copies(metadata);
  const int rendered = logical * copies;
  if (is_duplex_job(metadata))
    return static_cast<int>(std::ceil(rendered / 2.0));
  return rendered;
}

// Legacy additive pricing functions (volume_discount_multiplier,
// calculate_job_cost) have been removed.  All pricing is now handled
// by common::compute_job_pricing() which uses the matrix-based rule
// system with printer-aware color downgrade.

/// @returns a human-readable description of the page selection used for
/// this job, suitable for display in the detail panel and for cost
/// reconciliation.  Returns "全部" when the whole document was printed
/// or when the page range was not reported.
inline std::string describe_page_range(const core::MetadataSnapshot& metadata)
{
  const auto& f = metadata.page_range;
  if (f.state != core::FieldState::Reported || f.value.empty())
    return std::string{"全部"};
  if (f.value == "all" || f.value == "AllPages")
    return std::string{"全部"};
  return f.value;
}

}  // namespace wpce::ui
