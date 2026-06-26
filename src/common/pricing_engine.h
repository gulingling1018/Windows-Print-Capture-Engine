/// @file pricing_engine.h
/// Rule-based print-job pricing engine.
///
/// Each PricingRule specifies {paper, duplex, printer, color, filetype} → price,
/// and the engine finds the best-matching rule for each job.  Paper
/// size, printer name, and file extension dimensions support multi-value
/// matching (e.g. "A4|B5", "HP|Canon", or "pdf|dwg") via the
/// paper_size_set / printer_name_set / file_extension_set fields
/// on PricingRule.
///
/// Key behaviours:
///   - Duplex odd-page tail is correctly split into simplex pages and
///     charged at a separately-matched simplex rule, not the duplex
///     rule's per-sheet price.
///   - Color jobs on monochrome printers are downgraded (color_downgraded
///     flag set on the result) and priced as monochrome.
///   - Color jobs on color printers with color_pages==0 are flagged as
///     price_pending ("awaiting_color_pages") until the operator fills in
///     the color page count.
///   - Color jobs with color_pages>0 are split-priced: color and mono
///     portions independently go through duplex sheet breakdown, each
///     matched to their own color_mode rule.
///   - Unmatched rule combinations are flagged as price_pending.
///   - When rules is empty, the engine uses AppConfig::defaults().rules
///     (the factory default rules) instead of falling back to legacy.

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
///   0 = catch-all, 5 = fully specific (paper+duplex+printer+color+filetype).
/// Multi-value set fields count as 1 dimension (not N).
int rule_specificity(const core::PricingRule& rule);

/// Find the best-matching pricing rule for a given job.
/// Matches metadata.printer_name against rule.printer_name /
/// rule.printer_name_set (case-insensitive).  Empty printer_name on a
/// rule means "match any printer" (wildcard).
/// The effective_color parameter is matched against rule.color_mode
/// ("color", "monochrome", or "" for wildcard).
///
/// Returns a pointer to the rule with the highest specificity among all
/// matching rules, or nullptr if no rule matches.
/// When multiple rules have the same specificity, the first one in the
/// list wins.
const core::PricingRule* find_best_rule(
    const core::MetadataSnapshot& metadata,
    const std::vector<core::PricingRule>& rules,
    const std::string& effective_color = {});

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
/// into account.  Only an explicit "color" job on a color-capable
/// printer remains "color"; all other values are priced as monochrome.
/// A "color" job on a B&W printer is downgraded and the `downgraded`
/// out-parameter is set to true.
std::string effective_color_mode(
    const core::MetadataSnapshot& metadata,
    const PrinterProfileConfig& printers,
    bool& downgraded);

// ── Full pricing calculation ─────────────────────────────────────────────

/// Compute the complete pricing breakdown for a single print job.
///
/// Uses matrix-based matching with dimensions {paper, duplex, printer, color, filetype}:
///   1. Compute effective color (downgrade color on B&W printers).
///   2. Compute sheet breakdown (with duplex tail correction).
///   3. Find best-matching PricingRule by all 5 dimensions.
///   4. For duplex odd-page tail: find best-matching simplex rule and
///      price the tail separately.  No simplex rule → price_pending
///      with reason "no_simplex_rule_for_tail".
///   5. For color jobs on color-capable printers:
///      a. If color_pages == 0 → total_price = 0, price_pending with
///         reason "awaiting_color_pages" (operator must fill color page count).
///      b. If color_pages > 0 → split pricing:
///         - Color pages priced by the best color_mode="color" rule
///         - Remaining mono pages priced by the best color_mode="monochrome" rule
///   6. Flag unmatched jobs as price_pending.
///
/// @param metadata    The print job's metadata snapshot
/// @param config      Full pricing configuration (rules + color_pending flag)
/// @param printers    Printer capability profiles for color downgrade
/// @param color_pages Operator-filled color page count (0 = not yet filled)
/// @return            JobPricingEntry with complete breakdown and pending flags
core::JobPricingEntry compute_job_pricing(
    const core::MetadataSnapshot& metadata,
    const PricingConfig& config,
    const PrinterProfileConfig& printers,
    int color_pages = 0);

}  // namespace wpce::common
