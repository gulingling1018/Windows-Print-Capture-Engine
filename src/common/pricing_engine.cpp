/// @file pricing_engine.cpp
/// Pricing engine implementation (matrix-only, no legacy fallback).

#include "common/pricing_engine.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>

namespace wpce::common
{

// ── File type detection ──────────────────────────────────────────────────

std::string extract_file_extension(std::string_view document_name)
{
  if (document_name.empty()) return {};

  auto dot_pos = document_name.rfind('.');
  if (dot_pos == std::string_view::npos || dot_pos + 1 >= document_name.size())
    return {};

  std::string ext;
  ext.reserve(document_name.size() - dot_pos - 1);
  for (std::size_t i = dot_pos + 1; i < document_name.size(); ++i)
  {
    char ch = document_name[i];
    if (ch == '/' || ch == '\\' || ch == ' ' || ch == '\t')
      break;
    ext += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return ext;
}

// ── Rule matching ────────────────────────────────────────────────────────

int rule_specificity(const core::PricingRule& rule)
{
  int score = 0;
  // A dimension counts as 1 if either the single-string form or the
  // set form is non-empty.  We check the set form first (authoritative)
  // and fall back to the string form for older configs.
  if (!rule.paper_size_set.empty() || !rule.paper_size.empty()) ++score;
  if (!rule.duplex_mode.empty()) ++score;
  if (!rule.color_mode.empty()) ++score;
  if (!rule.file_extension_set.empty() || !rule.file_extension.empty()) ++score;
  return score;
}

namespace
{

bool icase_equals(std::string_view a, std::string_view b)
{
  if (a.size() != b.size()) return false;
  return std::equal(a.begin(), a.end(), b.begin(),
                    [](char ca, char cb) {
                      return std::tolower(static_cast<unsigned char>(ca)) ==
                             std::tolower(static_cast<unsigned char>(cb));
                    });
}

/// Match a job value against a rule dimension that may be either a
/// single string or a multi-value set.  Empty rule dimension = wildcard.
bool dimension_matches_set(
    const std::vector<std::string>& rule_set,
    const std::string& rule_string,
    std::string_view job_value)
{
  // Authoritative: use set form when non-empty.
  if (!rule_set.empty())
  {
    if (job_value.empty()) return false;
    for (const auto& rv : rule_set)
      if (icase_equals(rv, job_value)) return true;
    return false;
  }
  // Fallback: single-string form (empty = wildcard).
  if (rule_string.empty()) return true;
  if (job_value.empty()) return false;
  return icase_equals(rule_string, job_value);
}

/// Single-string dimension match (used for duplex_mode / color_mode which
/// remain single-value).  Empty rule value = wildcard.
bool dimension_matches(std::string_view rule_value,
                        std::string_view job_value)
{
  if (rule_value.empty()) return true;
  if (job_value.empty()) return false;
  return icase_equals(rule_value, job_value);
}

}  // namespace

std::string normalize_duplex_for_matching(std::string_view duplex_value)
{
  if (duplex_value.empty() || duplex_value == "simplex")
    return "simplex";
  return "duplex";
}

std::string normalize_color_for_matching(std::string_view color_value)
{
  if (color_value.empty()) return {};
  if (color_value == "color") return "color";
  if (color_value == "monochrome" || color_value == "grayscale")
    return "monochrome";
  return std::string(color_value);
}

const core::PricingRule* find_best_rule(
    const core::MetadataSnapshot& metadata,
    const std::vector<core::PricingRule>& rules,
    std::string_view effective_color)
{
  if (rules.empty()) return nullptr;

  const std::string job_duplex = normalize_duplex_for_matching(metadata.duplex.value);
  const std::string job_color(effective_color.begin(), effective_color.end());
  const std::string& job_paper = metadata.paper_size.value;
  const std::string& job_ext = metadata.file_extension.value;

  const core::PricingRule* best = nullptr;
  int best_specificity = -1;
  std::size_t best_index = static_cast<std::size_t>(-1);

  for (std::size_t i = 0; i < rules.size(); ++i)
  {
    const auto& rule = rules[i];

    if (!dimension_matches_set(rule.paper_size_set, rule.paper_size, job_paper))
      continue;
    if (!dimension_matches(rule.duplex_mode, job_duplex)) continue;
    if (!dimension_matches(rule.color_mode, job_color)) continue;
    if (!dimension_matches_set(rule.file_extension_set, rule.file_extension, job_ext))
      continue;

    const int spec = rule.priority > 0 ? rule.priority : rule_specificity(rule);
    if (spec > best_specificity ||
        (spec == best_specificity && i < best_index))
    {
      best_specificity = spec;
      best = &rule;
      best_index = i;
    }
  }

  return best;
}

void sort_rules_by_priority(std::vector<core::PricingRule>& rules)
{
  for (auto& r : rules)
    r.priority = rule_specificity(r);
  std::sort(rules.begin(), rules.end(),
            [](const core::PricingRule& a, const core::PricingRule& b) {
              return a.priority > b.priority;
            });
}

// ── Sheet breakdown (duplex odd-page tail) ──────────────────────────────

core::SheetBreakdown compute_sheet_breakdown(
    int rendered_pages, bool is_duplex)
{
  core::SheetBreakdown breakdown;
  if (rendered_pages <= 0) return breakdown;

  if (is_duplex)
  {
    breakdown.full_duplex_sheets = rendered_pages / 2;
    breakdown.simplex_tail_pages = rendered_pages % 2;
  }
  else
  {
    breakdown.full_duplex_sheets = 0;
    breakdown.simplex_tail_pages = rendered_pages;
  }

  breakdown.total_sheets = breakdown.full_duplex_sheets +
                           breakdown.simplex_tail_pages;
  return breakdown;
}

// ── Job attribute queries ────────────────────────────────────────────────

bool is_duplex_job(const core::MetadataSnapshot& metadata)
{
  const std::string& duplex = metadata.duplex.value;
  if (duplex.empty() || duplex == "simplex") return false;
  return (duplex == "vertical" ||
          duplex == "horizontal" ||
          duplex == "duplex" ||
          duplex == "long-edge (book)" ||
          duplex == "short-edge (calendar)" ||
          duplex == "TwoSidedLongEdge" ||
          duplex == "TwoSidedShortEdge");
}

bool is_color_job(const core::MetadataSnapshot& metadata)
{
  const std::string& color = metadata.color.value;
  return color == "color";
}

bool is_large_paper(const core::MetadataSnapshot& metadata)
{
  const std::string& paper = metadata.paper_size.value;
  if (paper.empty()) return false;
  return icase_equals(paper, "A3") ||
         icase_equals(paper, "Tabloid") ||
         icase_equals(paper, "Ledger") ||
         icase_equals(paper, "Legal");
}

bool is_high_quality(const core::MetadataSnapshot& metadata)
{
  const std::string& quality = metadata.print_quality.value;
  if (quality.empty()) return false;
  return quality == "high" ||
         quality == "1200 dpi" ||
         quality == "2400 dpi" ||
         quality == "4800 dpi";
}

int parse_copies(const core::MetadataSnapshot& metadata)
{
  const auto& f = metadata.copies;
  if (f.state != core::FieldState::Reported || f.value.empty())
    return 1;
  try
  {
    int n = std::stoi(f.value);
    return n > 0 ? n : 1;
  }
  catch (...) { return 1; }
}

int parse_total_pages(const core::MetadataSnapshot& metadata)
{
  const auto& f = metadata.total_pages;
  if (f.state != core::FieldState::Reported || f.value.empty())
    return 0;
  try { return std::stoi(f.value); }
  catch (...) { return 0; }
}

std::string effective_color_mode(
    const core::MetadataSnapshot& metadata,
    const PrinterProfileConfig& printers,
    bool& downgraded)
{
  downgraded = false;
  const std::string raw = normalize_color_for_matching(metadata.color.value);
  if (raw == "color")
  {
    const bool printer_color = lookup_color_capability(printers, metadata.printer_name);
    if (!printer_color)
    {
      downgraded = true;
      return "monochrome";
    }
  }
  return raw;
}

// ── Full pricing calculation ─────────────────────────────────────────────

namespace
{

/// Resolve the rules to use: config.rules when non-empty, otherwise the
/// factory defaults.  Returns a pointer to the effective rules vector
/// (either the caller's config or a static defaults instance).
const std::vector<core::PricingRule>& resolve_rules(
    const PricingConfig& config)
{
  if (!config.rules.empty()) return config.rules;
  // Use the factory default rules.  We construct them once and cache
  // statically so repeated calls are cheap.
  static const AppConfig defaults_app = AppConfig::defaults();
  return defaults_app.pricing.rules;
}

}  // namespace

core::JobPricingEntry compute_job_pricing(
    const core::MetadataSnapshot& metadata,
    const PricingConfig& config,
    const PrinterProfileConfig& printers)
{
  core::JobPricingEntry entry;
  entry.job_id = metadata.job_id;

  const int logical_pages = parse_total_pages(metadata);
  entry.total_pages = std::max(0, logical_pages);

  const int copies = parse_copies(metadata);
  entry.copies = copies;

  const int rendered_pages = logical_pages * copies;
  if (rendered_pages <= 0)
  {
    entry.price_pending = true;
    entry.pending_reason = "unknown_pages";
    return entry;
  }

  const bool duplex = is_duplex_job(metadata);

  // Effective color (printer-aware downgrade).
  bool color_downgraded = false;
  const std::string eff_color = effective_color_mode(metadata, printers, color_downgraded);
  entry.color_downgraded = color_downgraded;
  if (color_downgraded)
    entry.note = "color_on_bw_printer";

  // Compute physical sheet breakdown.
  const core::SheetBreakdown sheets = compute_sheet_breakdown(rendered_pages, duplex);
  entry.duplex_full_sheets = sheets.full_duplex_sheets;
  entry.simplex_tail_pages = sheets.simplex_tail_pages;
  entry.effective_sheets   = sheets.total_sheets;

  const std::vector<core::PricingRule>& rules = resolve_rules(config);

  // ── Match the main rule for the duplex (or simplex) body ──────────
  // We pass the effective color so that color-downgraded jobs match
  // monochrome rules.
  const core::PricingRule* matched = find_best_rule(metadata, rules, eff_color);

  if (matched == nullptr)
  {
    // No rule matched at all → pending.
    entry.price_pending = true;
    entry.pending_reason = eff_color == "color"
      ? "unmatched_rule_color"
      : "unmatched_rule";
    return entry;
  }

  entry.matched_rule_id = matched->rule_id;

  // For duplex jobs, the body sheets are charged at the matched duplex
  // rule's price.  The simplex tail (if any) is priced separately by a
  // simplex rule below.
  //
  // For simplex jobs, the matched rule already targets simplex and
  // simplex_tail_pages == rendered_pages, so we just charge them all.
  if (duplex)
  {
    entry.base_price = static_cast<double>(sheets.full_duplex_sheets) *
                       matched->price_per_sheet;
  }
  else
  {
    entry.base_price = static_cast<double>(sheets.simplex_tail_pages) *
                       matched->price_per_sheet;
  }

  // ── Duplex odd-page tail: separately priced as simplex ────────────
  if (duplex && sheets.simplex_tail_pages > 0)
  {
    // Find a simplex rule with the same paper/color/filetype dimensions.
    // We do this by temporarily treating the job as simplex for the
    // matching pass.  The simplest way is to call find_best_rule with a
    // synthetic "simplex" duplex — but find_best_rule reads
    // metadata.duplex directly.  So instead we inline a simplex-targeted
    // search using the same matching logic.
    const std::string job_paper = metadata.paper_size.value;
    const std::string job_ext = metadata.file_extension.value;

    const core::PricingRule* tail_rule = nullptr;
    int tail_spec = -1;
    std::size_t tail_index = static_cast<std::size_t>(-1);

    for (std::size_t i = 0; i < rules.size(); ++i)
    {
      const auto& rule = rules[i];
      // Must explicitly be a simplex rule to qualify as the tail rule.
      if (rule.duplex_mode != "simplex") continue;

      if (!dimension_matches_set(rule.paper_size_set, rule.paper_size, job_paper))
        continue;
      if (!dimension_matches(rule.color_mode, eff_color)) continue;
      if (!dimension_matches_set(rule.file_extension_set, rule.file_extension, job_ext))
        continue;

      const int spec = rule.priority > 0 ? rule.priority : rule_specificity(rule);
      if (spec > tail_spec || (spec == tail_spec && i < tail_index))
      {
        tail_spec = spec;
        tail_rule = &rule;
        tail_index = i;
      }
    }

    if (tail_rule != nullptr)
    {
      entry.tail_rule_id = tail_rule->rule_id;
      entry.tail_price = static_cast<double>(sheets.simplex_tail_pages) *
                         tail_rule->price_per_sheet;
    }
    else
    {
      // No simplex rule for the tail → flag as pending so the operator
      // can fill a price for this single tail page.
      entry.tail_price = 0.0;
      entry.price_pending = true;
      entry.pending_reason = "no_simplex_rule_for_tail";
    }
  }

  entry.total_price = entry.base_price + entry.tail_price;

  // ── Color pending ──────────────────────────────────────────────────
  // Only flag color-pending when the job is genuinely color (i.e. not
  // downgraded by a B&W printer) and the matched rule was not itself a
  // color-specific rule.
  if (eff_color == "color" && config.color_price_pending && !color_downgraded)
  {
    if (matched->color_mode != "color")
    {
      entry.price_pending = true;
      if (entry.pending_reason.empty()) entry.pending_reason = "color";
    }
  }

  return entry;
}

}  // namespace wpce::common
