/// @file pricing_engine_tests.cpp
/// Unit tests for the rule-based pricing engine.

#include "common/pricing_engine.h"
#include "core/data_contracts.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace wpce;

// ── Helpers ───────────────────────────────────────────────────────────────

namespace
{

core::MetadataSnapshot make_metadata(
    std::uint32_t job_id,
    const std::string& total_pages,
    const std::string& copies,
    const std::string& duplex,
    const std::string& color,
    const std::string& paper_size,
    const std::string& file_ext = "")
{
  core::MetadataSnapshot m;
  m.job_id = job_id;
  m.total_pages  = core::FieldValue{total_pages, core::FieldState::Reported};
  m.copies       = core::FieldValue{copies, core::FieldState::Reported};
  m.duplex       = core::FieldValue{duplex, core::FieldState::Reported};
  m.color        = core::FieldValue{color, core::FieldState::Reported};
  m.paper_size   = core::FieldValue{paper_size, core::FieldState::Reported};
  if (!file_ext.empty())
    m.file_extension = core::FieldValue{file_ext, core::FieldState::Reported};
  return m;
}

common::PricingConfig make_config_with_rules(
    const std::vector<core::PricingRule>& rules,
    bool color_price_pending = true)
{
  common::PricingConfig config;
  config.rules = rules;
  config.color_price_pending = color_price_pending;
  // Sort by priority descending for correct matching.
  common::sort_rules_by_priority(config.rules);
  return config;
}

common::PricingConfig make_legacy_config(
    double simplex = 0.15, double duplex = 0.25,
    double color = 0.50, double large = 0.30, double hq = 0.10)
{
  common::PricingConfig config;
  config.single_sided_per_page = simplex;
  config.double_sided_per_sheet = duplex;
  config.color_surcharge = color;
  config.large_paper_surcharge = large;
  config.high_quality_surcharge = hq;
  return config;
}

}  // namespace

// ── extract_file_extension ────────────────────────────────────────────────

TEST(PricingEngine, ExtractFileExtension_Basic)
{
  EXPECT_EQ(common::extract_file_extension("report.pdf"), "pdf");
  EXPECT_EQ(common::extract_file_extension("drawing.DWG"), "dwg");
  EXPECT_EQ(common::extract_file_extension("MyDoc.DocX"), "docx");
}

TEST(PricingEngine, ExtractFileExtension_NoExtension)
{
  EXPECT_EQ(common::extract_file_extension("noext"), "");
  EXPECT_EQ(common::extract_file_extension(""), "");
  EXPECT_EQ(common::extract_file_extension("file."), "");
}

TEST(PricingEngine, ExtractFileExtension_MultipleDots)
{
  EXPECT_EQ(common::extract_file_extension("archive.tar.gz"), "gz");
  EXPECT_EQ(common::extract_file_extension("a.b.c.d"), "d");
}

TEST(PricingEngine, ExtractFileExtension_WithPath)
{
  EXPECT_EQ(common::extract_file_extension("C:\\Users\\me\\doc.pdf"), "pdf");
  EXPECT_EQ(common::extract_file_extension("/home/user/file.txt"), "txt");
}

// ── rule_specificity ──────────────────────────────────────────────────────

TEST(PricingEngine, RuleSpecificity_AllEmpty)
{
  core::PricingRule rule{"r1", "", "", "", "", 0.10, 0};
  EXPECT_EQ(common::rule_specificity(rule), 0);
}

TEST(PricingEngine, RuleSpecificity_AllFilled)
{
  core::PricingRule rule{"r1", "A4", "simplex", "monochrome", "pdf", 0.10, 0};
  EXPECT_EQ(common::rule_specificity(rule), 4);
}

TEST(PricingEngine, RuleSpecificity_Partial)
{
  core::PricingRule rule{"r1", "A4", "simplex", "", "", 0.10, 0};
  EXPECT_EQ(common::rule_specificity(rule), 2);
}

// ── sort_rules_by_priority ────────────────────────────────────────────────

TEST(PricingEngine, SortRulesByPriority)
{
  std::vector<core::PricingRule> rules = {
    {"r1", "", "", "", "", 0.10, 0},         // specificity 0
    {"r2", "A4", "simplex", "monochrome", "pdf", 0.50, 0}, // specificity 4
    {"r3", "A4", "simplex", "", "", 0.15, 0},  // specificity 2
  };
  common::sort_rules_by_priority(rules);
  EXPECT_EQ(rules[0].rule_id, "r2"); // most specific first
  EXPECT_EQ(rules[1].rule_id, "r3");
  EXPECT_EQ(rules[2].rule_id, "r1"); // least specific last
}

// ── compute_sheet_breakdown ───────────────────────────────────────────────

TEST(PricingEngine, SheetBreakdown_DuplexEven)
{
  auto sb = common::compute_sheet_breakdown(6, true);
  EXPECT_EQ(sb.full_duplex_sheets, 3);
  EXPECT_EQ(sb.simplex_tail_pages, 0);
  EXPECT_EQ(sb.total_sheets, 3);
}

TEST(PricingEngine, SheetBreakdown_DuplexOdd)
{
  // 3 pages duplex → 1 full duplex sheet + 1 simplex tail page
  auto sb = common::compute_sheet_breakdown(3, true);
  EXPECT_EQ(sb.full_duplex_sheets, 1);
  EXPECT_EQ(sb.simplex_tail_pages, 1);
  EXPECT_EQ(sb.total_sheets, 2);
}

TEST(PricingEngine, SheetBreakdown_DuplexSinglePage)
{
  auto sb = common::compute_sheet_breakdown(1, true);
  EXPECT_EQ(sb.full_duplex_sheets, 0);
  EXPECT_EQ(sb.simplex_tail_pages, 1);
  EXPECT_EQ(sb.total_sheets, 1);
}

TEST(PricingEngine, SheetBreakdown_Simplex)
{
  auto sb = common::compute_sheet_breakdown(5, false);
  EXPECT_EQ(sb.full_duplex_sheets, 0);
  EXPECT_EQ(sb.simplex_tail_pages, 5);
  EXPECT_EQ(sb.total_sheets, 5);
}

TEST(PricingEngine, SheetBreakdown_ZeroPages)
{
  auto sb = common::compute_sheet_breakdown(0, true);
  EXPECT_EQ(sb.full_duplex_sheets, 0);
  EXPECT_EQ(sb.simplex_tail_pages, 0);
  EXPECT_EQ(sb.total_sheets, 0);
}

// ── is_duplex_job ─────────────────────────────────────────────────────────

TEST(PricingEngine, IsDuplex_Values)
{
  auto md = make_metadata(1, "10", "1", "simplex", "monochrome", "A4");
  EXPECT_FALSE(common::is_duplex_job(md));

  md.duplex.value = "long-edge (book)";
  EXPECT_TRUE(common::is_duplex_job(md));

  md.duplex.value = "short-edge (calendar)";
  EXPECT_TRUE(common::is_duplex_job(md));

  md.duplex.value = "TwoSidedLongEdge";
  EXPECT_TRUE(common::is_duplex_job(md));

  md.duplex.value = "";
  EXPECT_FALSE(common::is_duplex_job(md));
}

// ── is_color_job ──────────────────────────────────────────────────────────

TEST(PricingEngine, IsColor_Values)
{
  auto md = make_metadata(1, "10", "1", "simplex", "monochrome", "A4");
  EXPECT_FALSE(common::is_color_job(md));

  md.color.value = "color";
  EXPECT_TRUE(common::is_color_job(md));

  md.color.value = "grayscale";
  EXPECT_FALSE(common::is_color_job(md));
}

// ── find_best_rule ────────────────────────────────────────────────────────

TEST(PricingEngine, FindBestRule_ExactMatch)
{
  std::vector<core::PricingRule> rules = {
    {"r1", "A4", "simplex", "monochrome", "", 0.15, 0},
    {"r2", "A3", "simplex", "monochrome", "", 0.30, 0},
  };
  auto md = make_metadata(1, "5", "1", "simplex", "monochrome", "A4");
  const auto* matched = common::find_best_rule(md, rules);
  ASSERT_NE(matched, nullptr);
  EXPECT_EQ(matched->rule_id, "r1");
}

TEST(PricingEngine, FindBestRule_WildcardWinsWhenSpecificMissing)
{
  std::vector<core::PricingRule> rules = {
    {"r1", "A4", "simplex", "monochrome", "dwg", 0.50, 0}, // specific
    {"r2", "A4", "simplex", "", "", 0.15, 0},               // generic color
  };
  // A4 simplex monochrome PDF — no specific rule for monochrome+pdf, but
  // r2 matches with specificity=2 (paper+duplex)
  auto md = make_metadata(1, "5", "1", "simplex", "monochrome", "A4", "pdf");
  const auto* matched = common::find_best_rule(md, rules);
  ASSERT_NE(matched, nullptr);
  EXPECT_EQ(matched->rule_id, "r2");
}

TEST(PricingEngine, FindBestRule_SpecificityWins)
{
  std::vector<core::PricingRule> rules = {
    {"r_generic", "A4", "simplex", "", "", 0.15, 0},           // spec 2
    {"r_specific", "A4", "simplex", "monochrome", "dwg", 0.50, 0}, // spec 4
  };
  auto md = make_metadata(1, "5", "1", "simplex", "monochrome", "A4", "dwg");
  const auto* matched = common::find_best_rule(md, rules);
  ASSERT_NE(matched, nullptr);
  // r_specific should win because it has higher specificity
  EXPECT_EQ(matched->rule_id, "r_specific");
}

TEST(PricingEngine, FindBestRule_NoMatch)
{
  std::vector<core::PricingRule> rules = {
    {"r1", "A3", "simplex", "", "", 0.30, 0},
  };
  auto md = make_metadata(1, "5", "1", "simplex", "monochrome", "A4");
  const auto* matched = common::find_best_rule(md, rules);
  EXPECT_EQ(matched, nullptr);
}

TEST(PricingEngine, FindBestRule_EmptyRules)
{
  std::vector<core::PricingRule> rules;
  auto md = make_metadata(1, "5", "1", "simplex", "monochrome", "A4");
  const auto* matched = common::find_best_rule(md, rules);
  EXPECT_EQ(matched, nullptr);
}

// ── compute_job_pricing (matrix rules) ────────────────────────────────────

TEST(PricingEngine, ComputeJobPricing_BasicRuleMatch)
{
  auto config = make_config_with_rules({
    {"r1", "A4", "simplex", "monochrome", "", 0.15, 0},
  });
  auto md = make_metadata(1, "5", "1", "simplex", "monochrome", "A4");

  auto entry = common::compute_job_pricing(md, config);
  EXPECT_EQ(entry.job_id, 1u);
  EXPECT_EQ(entry.total_pages, 5);
  EXPECT_EQ(entry.copies, 1);
  EXPECT_EQ(entry.effective_sheets, 5); // simplex: 5 pages = 5 sheets
  EXPECT_EQ(entry.duplex_full_sheets, 0);
  EXPECT_EQ(entry.simplex_tail_pages, 5);
  EXPECT_EQ(entry.matched_rule_id, "r1");
  EXPECT_DOUBLE_EQ(entry.base_price, 5.0 * 0.15);
  EXPECT_DOUBLE_EQ(entry.total_price, 5.0 * 0.15);
  EXPECT_FALSE(entry.price_pending);
}

TEST(PricingEngine, ComputeJobPricing_DuplexWithOddTail)
{
  auto config = make_config_with_rules({
    {"r1", "A4", "duplex", "monochrome", "", 0.25, 0},
  });
  // 3 pages duplex → 1 full duplex sheet + 1 simplex tail
  auto md = make_metadata(2, "3", "1", "long-edge (book)", "monochrome", "A4");

  auto entry = common::compute_job_pricing(md, config);
  EXPECT_EQ(entry.effective_sheets, 2);
  EXPECT_EQ(entry.duplex_full_sheets, 1);
  EXPECT_EQ(entry.simplex_tail_pages, 1);

  // Both full duplex sheets and simplex tail charged at rule's per-sheet price.
  EXPECT_DOUBLE_EQ(entry.base_price, 2.0 * 0.25);
}

TEST(PricingEngine, ComputeJobPricing_ColorPending)
{
  auto config = make_config_with_rules({
    {"r1", "A4", "simplex", "", "", 0.15, 0},
  }, /*color_price_pending=*/true);
  auto md = make_metadata(3, "2", "1", "simplex", "color", "A4");

  auto entry = common::compute_job_pricing(md, config);
  EXPECT_TRUE(entry.price_pending);
  EXPECT_EQ(entry.pending_reason, "color");
}

TEST(PricingEngine, ComputeJobPricing_ColorNotPendingWhenRuleSpecifiesColor)
{
  auto config = make_config_with_rules({
    {"r1", "A4", "simplex", "color", "", 0.50, 0},
  }, /*color_price_pending=*/true);
  auto md = make_metadata(4, "1", "1", "simplex", "color", "A4");

  auto entry = common::compute_job_pricing(md, config);
  EXPECT_FALSE(entry.price_pending);
  EXPECT_EQ(entry.matched_rule_id, "r1");
}

TEST(PricingEngine, ComputeJobPricing_ColorNotPendingWhenDisabled)
{
  auto config = make_config_with_rules({
    {"r1", "A4", "simplex", "", "", 0.15, 0},
  }, /*color_price_pending=*/false);
  auto md = make_metadata(5, "1", "1", "simplex", "color", "A4");

  auto entry = common::compute_job_pricing(md, config);
  EXPECT_FALSE(entry.price_pending);
}

TEST(PricingEngine, ComputeJobPricing_UnmatchedRule)
{
  auto config = make_config_with_rules({
    {"r1", "A3", "simplex", "", "", 0.30, 0}, // only A3
  });
  auto md = make_metadata(6, "5", "1", "simplex", "monochrome", "A4");

  auto entry = common::compute_job_pricing(md, config);
  EXPECT_TRUE(entry.price_pending);
  EXPECT_EQ(entry.pending_reason, "unmatched_rule");
}

TEST(PricingEngine, ComputeJobPricing_WithCopies)
{
  auto config = make_config_with_rules({
    {"r1", "A4", "simplex", "monochrome", "", 0.15, 0},
  });
  // 3 pages × 2 copies = 6 rendered pages
  auto md = make_metadata(7, "3", "2", "simplex", "monochrome", "A4");

  auto entry = common::compute_job_pricing(md, config);
  EXPECT_EQ(entry.total_pages, 3);
  EXPECT_EQ(entry.copies, 2);
  EXPECT_EQ(entry.effective_sheets, 6);
  EXPECT_DOUBLE_EQ(entry.base_price, 6.0 * 0.15);
}

// ── Legacy fallback pricing ───────────────────────────────────────────────

TEST(PricingEngine, LegacyFallback_Simplex)
{
  auto config = make_legacy_config();
  auto md = make_metadata(10, "10", "1", "simplex", "monochrome", "A4");

  double cost = common::calculate_job_cost_legacy(md, config);
  EXPECT_DOUBLE_EQ(cost, 10.0 * 0.15); // 10 pages simplex
}

TEST(PricingEngine, LegacyFallback_DuplexOddTail)
{
  auto config = make_legacy_config(0.15, 0.25);
  auto md = make_metadata(11, "3", "1", "long-edge (book)", "monochrome", "A4");

  double cost = common::calculate_job_cost_legacy(md, config);
  // Correct: 1 full duplex sheet @ 0.25 + 1 simplex tail @ 0.15 = 0.40
  EXPECT_DOUBLE_EQ(cost, 0.40);
}

TEST(PricingEngine, LegacyFallback_DuplexEven)
{
  auto config = make_legacy_config(0.15, 0.25);
  auto md = make_metadata(12, "4", "1", "long-edge (book)", "monochrome", "A4");

  double cost = common::calculate_job_cost_legacy(md, config);
  // 4 pages duplex = 2 full duplex sheets @ 0.25 = 0.50
  EXPECT_DOUBLE_EQ(cost, 0.50);
}

TEST(PricingEngine, LegacyFallback_ColorSurcharge)
{
  auto config = make_legacy_config(0.15, 0.25, 0.50);
  auto md = make_metadata(13, "1", "1", "simplex", "color", "A4");

  double cost = common::calculate_job_cost_legacy(md, config);
  // 1 page simplex @ 0.15 + 1 page color @ 0.50 = 0.65
  EXPECT_DOUBLE_EQ(cost, 0.65);
}

TEST(PricingEngine, LegacyFallback_ZeroPages)
{
  auto config = make_legacy_config();
  auto md = make_metadata(14, "0", "1", "simplex", "monochrome", "A4");

  double cost = common::calculate_job_cost_legacy(md, config);
  EXPECT_DOUBLE_EQ(cost, 0.0);
}

// ── compute_job_pricing (legacy fallback path) ────────────────────────────

TEST(PricingEngine, ComputeJobPricing_FallsBackToLegacyWhenRulesEmpty)
{
  auto config = make_legacy_config(0.15, 0.25, 0.50);
  config.color_price_pending = false;
  auto md = make_metadata(20, "3", "1", "long-edge (book)", "monochrome", "A4");

  auto entry = common::compute_job_pricing(md, config);
  EXPECT_FALSE(entry.price_pending);
  EXPECT_DOUBLE_EQ(entry.total_price, 0.40); // same as legacy
  EXPECT_EQ(entry.effective_sheets, 2);
  EXPECT_EQ(entry.duplex_full_sheets, 1);
  EXPECT_EQ(entry.simplex_tail_pages, 1);
}
