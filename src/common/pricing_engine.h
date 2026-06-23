/// @file pricing_engine.h
/// Rule-based print-job pricing engine.
///
/// Each PricingRule specifies {paper, duplex, color, filetype} → price,
/// and the engine finds the best-matching rule for each job.  Paper
/// size and file extension dimensions support multi-value matching
/// (e.g. "A4|B5" or "pdf|dwg") via the paper_size_set / file_extension_set
/// fields on PricingRule.
///
/// Key behaviours:
///   - Duplex odd-page tail is correctly split into simplex pages and
///     charged at a separately-matched simplex rule, not the duplex
///     rule's per-sheet price.
///   - Color jobs on monochrome printers are downgraded to the
///     monochrome rate (color_downgraded flag set on the result) and
///     never flagged as price_pending, regardless of color_price_pending.
///   - Color jobs on color printers with no matching color rule are
///     flagged as price_pending (operator must fill the price).
///   - Unmatched rule combinations are flagged as price_pending.
///   - When rules is empty, the engine uses AppConfig::defaults().rules
///     (the 6 factory default rules) instead of falling back to legacy.

#pragma once

#include "common/app_config.h"
#include "common/printer_profile.h"
#include "core/data_contracts.h"

#include <string>
#include <string_view>
#include <vector>

namespace wpce::common
{

// ── File type detection ──────────────────────────────────────────────────

/// Extract the file extension from a document name.
/// Returns the lowercased extension without the leading dot, e.g.
///   "report.PDF"    → "pdf"
///   "drawing.DWG"   → "dwg"
///   "noext"         → ""
///   ""              → ""
std::string extract_file_extension(std::string_view document_name);

// ── Rule matching ────────────────────────────────────────────────────────

/// Compute the specificity (priority) of a pricing rule.
/// Returns the count of non-empty dimension fields:
///   0 = catch-all, 4 = fully specific (paper+duplex+color+filetype).
/// Multi-value set fields count as 1 dimension (not N).
int rule_specificity(const core::PricingRule& rule);

/// Find the best-matching pricing rule for a given job, using the
/// effective color mode (after printer-capability downgrade).
/// Returns a pointer to the rule with the highest specificity among all
/// matching rules, or nullptr if no rule matches.
/// When multiple rules have the same specificity, the first one in the
/// list wins.
const core::PricingRule* find_best_rule(
    const core::MetadataSnapshot& metadata,
    const std::vector<core::PricingRule>& rules,
    std::string_view effective_color);

/// Sort rules by priority descending (most specific first).
/// Call this after loading rules from config to ensure correct ordering.
void sort_rules_by_priority(std::vector<core::PricingRule>& rules);

// ── Sheet breakdown (duplex odd-page tail) ──────────────────────────────

/// Compute the physical sheet breakdown for a print job.
///
/// Correctly handles the duplex odd-page tail scenario:
///   rendered_pages=3, duplex → 1 full duplex sheet + 1 simplex tail page
///   rendered_pages=4, duplex → 2 full duplex sheets
///   rendered_pages=5, simplex → 5 simplex tail pages
core::SheetBreakdown compute_sheet_breakdown(
    int rendered_pages, bool is_duplex);

// ── Job attribute queries ────────────────────────────────────────────────

/// Determine whether a metadata snapshot indicates double-sided printing.
bool is_duplex_job(const core::MetadataSnapshot& metadata);

/// Determine whether printing is in colour (explicitly "color", not
/// "monochrome" or "grayscale").
bool is_color_job(const core::MetadataSnapshot& metadata);

/// Determine whether the paper size is "large" (A3, Tabloid, Ledger, Legal).
bool is_large_paper(const core::MetadataSnapshot& metadata);

/// Determine whether the print quality setting indicates high quality.
bool is_high_quality(const core::MetadataSnapshot& metadata);

/// Parse the copies count from metadata, defaulting to 1.
int parse_copies(const core::MetadataSnapshot& metadata);

/// Parse the total logical page count from metadata, returning 0 if not reported.
int parse_total_pages(const core::MetadataSnapshot& metadata);

/// Normalize a color value for rule matching.
///   "color" → "color"
///   "monochrome" / "grayscale" → "monochrome"
///   other / empty → as-is (empty means unknown)
std::string normalize_color_for_matching(std::string_view color_value);

/// Normalize a duplex value for rule matching.
///   "simplex" / empty → "simplex"
///   any duplex variant → "duplex"
std::string normalize_duplex_for_matching(std::string_view duplex_value);

/// Compute the effective color mode for a job, taking printer capability
/// into account.  A "color" job on a B&W printer is downgraded to
/// "monochrome" and the `downgraded` out-parameter is set to true.
std::string effective_color_mode(
    const core::MetadataSnapshot& metadata,
    const PrinterProfileConfig& printers,
    bool& downgraded);

// ── Full pricing calculation ─────────────────────────────────────────────

/// Compute the complete pricing breakdown for a single print job.
///
/// Uses matrix-based matching:
///   1. Compute effective color (downgrade color on B&W printers).
///   2. Compute sheet breakdown (with duplex tail correction).
///   3. Find best-matching PricingRule for the duplex (or simplex) sheets.
///   4. For duplex odd-page tail: find best-matching simplex rule and
///      price the tail separately.  No simplex rule → price_pending
///      with reason "no_simplex_rule_for_tail".
///   5. Flag color-on-color-printer jobs as price_pending when no color
///      rule matched and config.color_price_pending is true.
///   6. Flag unmatched jobs as price_pending.
///
/// When `config.rules` is empty, the engine substitutes
/// `AppConfig::defaults().pricing.rules` so the factory default 6-rule
/// matrix always applies.
///
/// @param metadata  The print job's metadata snapshot
/// @param config    Full pricing configuration (rules + color_pending flag)
/// @param printers  Printer capability profiles for color downgrade
/// @return          JobPricingEntry with complete breakdown and pending flags
core::JobPricingEntry compute_job_pricing(
    const core::MetadataSnapshot& metadata,
    const PricingConfig& config,
    const PrinterProfileConfig& printers);

}  // namespace wpce::common

