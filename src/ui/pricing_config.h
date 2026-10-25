#pragma once

#include "common/app_config.h"
#include "common/pricing_engine.h"
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

// ── Delegates to wpce::common for job-attribute queries ───────────────
// The authoritative implementations live in common/pricing_engine.h.
// UI code should use these unqualified names (resolved via using-decls).

using wpce::common::is_duplex_job;
using wpce::common::is_color_job;
using wpce::common::parse_copies;
using wpce::common::parse_total_pages;

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
