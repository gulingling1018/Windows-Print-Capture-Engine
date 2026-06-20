#pragma once

#include "core/data_contracts.h"

#include <cmath>
#include <string>

namespace wpce::ui
{

/// Pricing configuration for print cost calculation.
struct PricingConfig
{
  /// Price per page for single-sided (simplex) printing.  Default: 0.15 元/页.
  double single_sided_per_page{0.15};

  /// Price per sheet for double-sided (duplex) printing.  Default: 0.25 元/张.
  double double_sided_per_sheet{0.25};
};

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
  if (duplex == "vertical" || duplex == "horizontal" || duplex == "duplex")
  {
    return true;
  }
  return false;
}

/// Calculate the cost for a single print job based on metadata and pricing config.
inline double calculate_job_cost(const core::MetadataSnapshot& metadata,
                                 const PricingConfig& config)
{
  int total_pages = 0;
  try
  {
    total_pages = std::stoi(metadata.total_pages.value);
  }
  catch (...)
  {
    return 0.0;
  }

  if (total_pages <= 0)
  {
    return 0.0;
  }

  if (is_duplex_job(metadata))
  {
    // Double-sided: each sheet holds 2 pages.
    const int sheets = static_cast<int>(std::ceil(total_pages / 2.0));
    return sheets * config.double_sided_per_sheet;
  }

  // Single-sided or unknown: charge per page.
  return total_pages * config.single_sided_per_page;
}

}  // namespace wpce::ui
