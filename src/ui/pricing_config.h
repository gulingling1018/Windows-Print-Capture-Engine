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
  // Treat unknown/empty/not-reported as simplex.
  if (duplex.empty() || duplex == "simplex")
  {
    return false;
  }
  // Recognised duplex values.
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

  // Case-insensitive match against known large sizes.
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
  // psk:Grayscale is a colour-mode variant that some drivers report
  // instead of "monochrome"; it is still monochrome for pricing.
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
/// accounting for duplex and copies.  Used by both the per-job cost
/// calculation and the per-user group aggregation so they stay
/// consistent.
///
///   copies       = metadata.copies (>= 1)
///   logical_pages= total_pages × copies       (the pages the driver renders)
///   sheets       = duplex ? ceil(logical_pages / 2) : logical_pages
///
/// Note: some drivers report total_pages already multiplied by copies
/// (physical pages) while others report the logical page count.  We
/// treat metadata.total_pages as the logical count and multiply by
/// copies here, which matches the PrintTicket semantics where
/// JobCopiesAllDocuments is a separate parameter.
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

/// Calculate the volume discount multiplier for a given total page count.
/// Returns 1.0 if no discount applies.
inline double volume_discount_multiplier(int total_pages,
                                          const std::vector<common::VolumeDiscountTier>& tiers)
{
  if (tiers.empty()) return 1.0;

  double best_discount = 1.0;
  for (const auto& tier : tiers)
  {
    if (total_pages >= tier.min_pages && tier.discount < best_discount)
    {
      best_discount = tier.discount;
    }
  }
  return best_discount;
}

/// Calculate the cost for a single print job based on metadata and pricing config.
///
/// Pricing formula:
///   rendered_pages = total_pages × copies      (logical pages the driver renders)
///   base = duplex ? ceil(rendered_pages/2) × double_sided_per_sheet
///                : rendered_pages × single_sided_per_page
///   surcharges:
///     + color_surcharge  × rendered_pages  (if color job)
///     + large_surcharge  × rendered_pages  (if A3/Tabloid/Ledger/Legal)
///     + hq_surcharge     × rendered_pages  (if high quality)
///   subtotal = base + sum(surcharges)
///   final    = subtotal × volume_discount_multiplier(rendered_pages)
///
/// The rendered page count (total_pages × copies) is used for every
/// per-page charge so the price scales correctly with copy count.
/// This matches the aggregation in UserGroupModel::recomputeGroupSummary.
inline double calculate_job_cost(const core::MetadataSnapshot& metadata,
                                  const PricingConfig& config)
{
  const int logical_pages = parse_total_pages(metadata);
  if (logical_pages <= 0)
  {
    return 0.0;
  }

  const int copies = parse_copies(metadata);
  const int rendered_pages = logical_pages * copies;
  if (rendered_pages <= 0)
  {
    return 0.0;
  }

  // ── Base cost ──────────────────────────────────────────────────────────
  double base_cost = 0.0;
  if (is_duplex_job(metadata))
  {
    // Double-sided: each sheet holds 2 pages.
    const int sheets = static_cast<int>(std::ceil(rendered_pages / 2.0));
    base_cost = sheets * config.double_sided_per_sheet;
  }
  else
  {
    // Single-sided or unknown: charge per page.
    base_cost = rendered_pages * config.single_sided_per_page;
  }

  // ── Surcharges (per rendered page) ─────────────────────────────────────
  double surcharges = 0.0;
  if (is_color_job(metadata))
  {
    surcharges += rendered_pages * config.color_surcharge;
  }
  if (is_large_paper(metadata))
  {
    surcharges += rendered_pages * config.large_paper_surcharge;
  }
  if (is_high_quality(metadata))
  {
    surcharges += rendered_pages * config.high_quality_surcharge;
  }

  const double subtotal = base_cost + surcharges;

  // ── Volume discount (based on rendered page count) ─────────────────────
  const double multiplier = volume_discount_multiplier(rendered_pages, config.volume_discounts);
  return subtotal * multiplier;
}

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
