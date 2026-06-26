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
  if (!rule.printer_name_set.empty() || !rule.printer_name.empty()) ++score;
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

/// Single-string dimension match (used for duplex_mode which
/// remains single-value).  Empty rule value = wildcard.
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
  const auto first = color_value.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) return {};
  const auto last = color_value.find_last_not_of(" \t\r\n");
  const std::string_view value = color_value.substr(first, last - first + 1);

  if (value.empty() || value == "-") return {};
  if (icase_equals(value, "color") ||
      icase_equals(value, "colour") ||
      icase_equals(value, "clolor"))
    return "color";
  if (icase_equals(value, "monochrome") ||
      icase_equals(value, "monchorme") ||
      icase_equals(value, "grayscale") ||
      icase_equals(value, "greyscale") ||
      icase_equals(value, "bw") ||
      icase_equals(value, "b&w"))
    return "monochrome";
  return std::string(value);
}

const core::PricingRule* find_best_rule(
    const core::MetadataSnapshot& metadata,
    const std::vector<core::PricingRule>& rules,
    const std::string& effective_color)
{
  if (rules.empty()) return nullptr;

  const std::string job_duplex = normalize_duplex_for_matching(metadata.duplex.value);
  const std::string& job_paper = metadata.paper_size.value;
  const std::string& job_ext = metadata.file_extension.value;
  const std::string& job_printer = metadata.printer_name;

  const core::PricingRule* best = nullptr;
  int best_specificity = -1;
  std::size_t best_index = static_cast<std::size_t>(-1);

  for (std::size_t i = 0; i < rules.size(); ++i)
  {
    const auto& rule = rules[i];

    if (!dimension_matches_set(rule.paper_size_set, rule.paper_size, job_paper))
      continue;
    if (!dimension_matches(rule.duplex_mode, job_duplex)) continue;
    if (!dimension_matches_set(rule.printer_name_set, rule.printer_name, job_printer))
      continue;
    if (!dimension_matches(rule.color_mode, effective_color)) continue;
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
  return normalize_color_for_matching(metadata.color.value) == "color";
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
    if (printer_color)
      return "color";
    else
    {
      downgraded = true;
      return "monochrome";
    }
  }

  // Driver values such as "-" or not-reported are not evidence of a
  // colour job.  For billing they match monochrome rules; only an
  // explicit colour parameter on a colour-capable printer enters the
  // operator-filled split-pricing path.
  return "monochrome";
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
    const PrinterProfileConfig& printers,
    int color_pages)
{
  core::JobPricingEntry entry;
  entry.job_id = metadata.job_id;

  const int logical_pages = parse_total_pages(metadata);
  entry.total_pages = std::max(0, logical_pages);

  const int copies = parse_copies(metadata);
  entry.copies = copies;

  // Store the operator-filled color page count for UI round-trip.
  entry.color_pages = std::max(0, std::min(color_pages, logical_pages));
  entry.mono_pages = std::max(0, logical_pages - entry.color_pages);

  if (logical_pages <= 0)
  {
    entry.price_pending = true;
    entry.pending_reason = "unknown_pages";
    return entry;
  }

  // Single-page documents cannot physically use both sides of a sheet,
  // so they are always priced as simplex regardless of duplex setting.
  const bool duplex = is_duplex_job(metadata) && logical_pages > 1;
  core::MetadataSnapshot pricing_metadata = metadata;
  pricing_metadata.duplex.value = duplex ? "duplex" : "simplex";
  pricing_metadata.duplex.state = core::FieldState::Reported;

  // Effective color (printer-aware downgrade).
  bool color_downgraded = false;
  const std::string eff_color = effective_color_mode(metadata, printers, color_downgraded);
  entry.color_downgraded = color_downgraded;
  if (color_downgraded)
    entry.note = "color_on_bw_printer";

  // Compute physical sheet breakdown per copy, then scale by copies.
  const core::SheetBreakdown perCopy = compute_sheet_breakdown(logical_pages, duplex);
  core::SheetBreakdown sheets;
  sheets.full_duplex_sheets = perCopy.full_duplex_sheets * copies;
  sheets.simplex_tail_pages = perCopy.simplex_tail_pages * copies;
  sheets.total_sheets       = sheets.full_duplex_sheets + sheets.simplex_tail_pages;
  entry.duplex_full_sheets = sheets.full_duplex_sheets;
  entry.simplex_tail_pages = sheets.simplex_tail_pages;
  entry.effective_sheets   = sheets.total_sheets;

  const std::vector<core::PricingRule>& rules = resolve_rules(config);

  // Determine matching color for rule lookup.  The effective colour
  // already accounts for printer capability and unknown driver values.
  const std::string match_color = eff_color;

  // ── Match the main rule ───────────────────────────────────────────
  const core::PricingRule* matched = find_best_rule(pricing_metadata, rules, match_color);

  if (matched == nullptr)
  {
    entry.price_pending = true;
    entry.pending_reason = "unmatched_rule";
    return entry;
  }

  entry.matched_rule_id = matched->rule_id;

  if (!matched->printer_name_set.empty())
  {
    for (std::size_t pi = 0; pi < matched->printer_name_set.size(); ++pi)
    {
      if (pi > 0) entry.matched_printer_name += "|";
      entry.matched_printer_name += matched->printer_name_set[pi];
    }
  }
  else
  {
    entry.matched_printer_name = matched->printer_name;
  }

  // ── Color job split pricing ───────────────────────────────────────
  const bool is_color = is_color_job(metadata);
  if (is_color && !color_downgraded)
  {
    if (entry.color_pages <= 0)
    {
      // Operator hasn't filled color pages → price=0, pending.
      entry.total_price = 0.0;
      entry.base_price = 0.0;
      entry.tail_price = 0.0;
      entry.price_pending = true;
      entry.pending_reason = "awaiting_color_pages";
      return entry;
    }

    // Split: color and mono portions calculated independently through
    // duplex sheet breakdown, each finding their own matching rules.
    const core::PricingRule* color_rule = find_best_rule(pricing_metadata, rules, "color");
    const core::PricingRule* mono_rule  = find_best_rule(pricing_metadata, rules, "monochrome");

    if (!color_rule) color_rule = find_best_rule(pricing_metadata, rules, "");
    if (!mono_rule)  mono_rule  = find_best_rule(pricing_metadata, rules, "");

    if (!color_rule)
    {
      entry.price_pending = true;
      entry.pending_reason = "no_color_rule";
      return entry;
    }
    if (!mono_rule)
    {
      entry.price_pending = true;
      entry.pending_reason = "no_mono_rule";
      return entry;
    }

    // Helper: find a simplex rule for a given color mode (for tail pricing).
    auto find_simplex_rule = [&](const std::string& color_for_tail) -> const core::PricingRule*
    {
      const std::string& job_paper = metadata.paper_size.value;
      const std::string& job_ext = metadata.file_extension.value;
      const std::string& job_printer = metadata.printer_name;

      const core::PricingRule* best = nullptr;
      int best_spec = -1;
      std::size_t best_idx = static_cast<std::size_t>(-1);

      for (std::size_t i = 0; i < rules.size(); ++i)
      {
        const auto& rule = rules[i];
        if (rule.duplex_mode != "simplex") continue;
        if (!dimension_matches(rule.color_mode, color_for_tail)) continue;
        if (!dimension_matches_set(rule.paper_size_set, rule.paper_size, job_paper)) continue;
        if (!dimension_matches_set(rule.printer_name_set, rule.printer_name, job_printer)) continue;
        if (!dimension_matches_set(rule.file_extension_set, rule.file_extension, job_ext)) continue;

        const int spec = rule.priority > 0 ? rule.priority : rule_specificity(rule);
        if (spec > best_spec || (spec == best_spec && i < best_idx))
        {
          best_spec = spec;
          best = &rule;
          best_idx = i;
        }
      }
      return best;
    };

    // ── Color portion: independent duplex sheet breakdown ──────────
    const bool color_duplex = duplex && entry.color_pages > 1;
    const core::SheetBreakdown color_bd = compute_sheet_breakdown(entry.color_pages, color_duplex);
    double color_price = 0.0;

    if (color_duplex)
    {
      color_price = static_cast<double>(color_bd.full_duplex_sheets) *
                    color_rule->price_per_sheet;
      if (color_bd.simplex_tail_pages > 0)
      {
        const core::PricingRule* color_tail = find_simplex_rule("color");
        if (!color_tail) color_tail = find_simplex_rule("");
        if (color_tail)
          color_price += static_cast<double>(color_bd.simplex_tail_pages) *
                         color_tail->price_per_sheet;
        else
          color_price += static_cast<double>(color_bd.simplex_tail_pages) *
                         color_rule->price_per_sheet;
      }
    }
    else
    {
      // ≤1 color page in a duplex job → physically simplex.
      // Use the simplex color rule, not the duplex-matched color_rule.
      const core::PricingRule* eff_color_rule = color_rule;
      if (duplex)
      {
        const core::PricingRule* csr = find_simplex_rule("color");
        if (!csr) csr = find_simplex_rule("");
        if (csr) eff_color_rule = csr;
      }
      color_price = static_cast<double>(color_bd.simplex_tail_pages) *
                    eff_color_rule->price_per_sheet;
    }

    // ── Mono portion: independent duplex sheet breakdown ──────────
    const bool mono_duplex = duplex && entry.mono_pages > 1;
    const core::SheetBreakdown mono_bd = compute_sheet_breakdown(entry.mono_pages, mono_duplex);
    double mono_price = 0.0;

    if (mono_duplex)
    {
      mono_price = static_cast<double>(mono_bd.full_duplex_sheets) *
                   mono_rule->price_per_sheet;
      if (mono_bd.simplex_tail_pages > 0)
      {
        const core::PricingRule* mono_tail = find_simplex_rule("monochrome");
        if (!mono_tail) mono_tail = find_simplex_rule("");
        if (mono_tail)
          mono_price += static_cast<double>(mono_bd.simplex_tail_pages) *
                        mono_tail->price_per_sheet;
        else
          mono_price += static_cast<double>(mono_bd.simplex_tail_pages) *
                        mono_rule->price_per_sheet;
      }
    }
    else
    {
      // ≤1 mono page in a duplex job → physically simplex.
      const core::PricingRule* eff_mono_rule = mono_rule;
      if (duplex)
      {
        const core::PricingRule* msr = find_simplex_rule("monochrome");
        if (!msr) msr = find_simplex_rule("");
        if (msr) eff_mono_rule = msr;
      }
      mono_price = static_cast<double>(mono_bd.simplex_tail_pages) *
                   eff_mono_rule->price_per_sheet;
    }

    entry.color_subtotal = color_price * copies;
    entry.mono_subtotal  = mono_price * copies;
    entry.base_price     = entry.mono_subtotal;
    entry.tail_price     = 0.0;
    entry.total_price    = entry.color_subtotal + entry.mono_subtotal;
    entry.matched_rule_id = mono_rule->rule_id;
    return entry;
  }

  // ── Standard (monochrome or downgraded) pricing ───────────────────
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
    const std::string job_paper = metadata.paper_size.value;
    const std::string job_ext = metadata.file_extension.value;
    const std::string& job_printer = metadata.printer_name;

    const core::PricingRule* tail_rule = nullptr;
    int tail_spec = -1;
    std::size_t tail_index = static_cast<std::size_t>(-1);

    for (std::size_t i = 0; i < rules.size(); ++i)
    {
      const auto& rule = rules[i];
      if (rule.duplex_mode != "simplex") continue;
      if (!dimension_matches(rule.color_mode, eff_color)) continue;

      if (!dimension_matches_set(rule.paper_size_set, rule.paper_size, job_paper))
        continue;
      if (!dimension_matches_set(rule.printer_name_set, rule.printer_name, job_printer))
        continue;
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
      entry.tail_price = 0.0;
      entry.price_pending = true;
      entry.pending_reason = "no_simplex_rule_for_tail";
    }
  }

  entry.total_price = entry.base_price + entry.tail_price;
  return entry;
}

}  // namespace wpce::common
