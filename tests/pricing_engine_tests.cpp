/// @file pricing_engine_tests.cpp
/// Unit tests for the rule-based pricing engine.

#include "common/pricing_engine.h"
#include "core/data_contracts.h"

#include <gtest/gtest.h>

#include <algorithm>
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
    const std::string& file_ext = "",
    const std::string& printer_name = "Test Printer")
{
  core::MetadataSnapshot m;
  m.job_id = job_id;
  m.printer_name = printer_name;
  m.total_pages  = core::FieldValue{total_pages, core::FieldState::Reported};
  m.copies       = core::FieldValue{copies, core::FieldState::Reported};
  m.duplex       = core::FieldValue{duplex, core::FieldState::Reported};
  m.color        = core::FieldValue{color, core::FieldState::Reported};
  m.paper_size   = core::FieldValue{paper_size, core::FieldState::Reported};
  if (!file_ext.empty())
    m.file_extension = core::FieldValue{file_ext, core::FieldState::Reported};
  return m;
}

// PricingRule fields: rule_id, paper_size, duplex_mode, printer_name, color_mode, file_extension, price_per_sheet, priority
core::PricingRule make_rule(const std::string& id, const std::string& paper,
                            const std::string& duplex, const std::string& printer,
                            const std::string& color, const std::string& ext,
                            double price)
{
  core::PricingRule r;
  r.rule_id = id;
  r.paper_size = paper;
  r.duplex_mode = duplex;
  r.printer_name = printer;
  r.color_mode = color;
  r.file_extension = ext;
  r.price_per_sheet = price;
  r.priority = common::rule_specificity(r);
  return r;
}

common::PricingConfig make_config_with_rules(
    const std::vector<core::PricingRule>& rules,
    bool color_price_pending = true)
{
  common::PricingConfig config;
  config.rules = rules;
  config.color_price_pending = color_price_pending;
  common::sort_rules_by_priority(config.rules);
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
  auto rule = make_rule("r1", "", "", "", "", "", 0.10);
  EXPECT_EQ(common::rule_specificity(rule), 0);
}

TEST(PricingEngine, RuleSpecificity_AllFilled)
{
  auto rule = make_rule("r1", "A4", "simplex", "PrinterA", "monochrome", "pdf", 0.10);
  EXPECT_EQ(common::rule_specificity(rule), 5);
}

TEST(PricingEngine, RuleSpecificity_Partial)
{
  auto rule = make_rule("r1", "A4", "simplex", "", "", "", 0.10);
  EXPECT_EQ(common::rule_specificity(rule), 2);
}

// ── sort_rules_by_priority ────────────────────────────────────────────────

TEST(PricingEngine, SortRulesByPriority)
{
  std::vector<core::PricingRule> rules = {
    make_rule("r1", "", "", "", "", "", 0.10),                          // specificity 0
    make_rule("r2", "A4", "simplex", "PrinterA", "monochrome", "pdf", 0.50), // specificity 5
    make_rule("r3", "A4", "simplex", "", "", "", 0.15),                 // specificity 2
  };
  common::sort_rules_by_priority(rules);
  EXPECT_EQ(rules[0].rule_id, "r2");
  EXPECT_EQ(rules[1].rule_id, "r3");
  EXPECT_EQ(rules[2].rule_id, "r1");
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

TEST(PricingEngine, EffectiveColor_DashIsPricedAsMonochrome)
{
  auto md = make_metadata(2, "10", "1", "simplex", "-", "A4", "", "HP_Color");

  common::PrinterProfileConfig printers;
  printers.profiles.push_back({"HP_Color", /*is_color_capable=*/true});

  bool downgraded = false;
  EXPECT_EQ(common::effective_color_mode(md, printers, downgraded), "monochrome");
  EXPECT_FALSE(downgraded);
}

// ── find_best_rule ────────────────────────────────────────────────────────

TEST(PricingEngine, FindBestRule_ExactMatch)
{
  std::vector<core::PricingRule> rules = {
    make_rule("r1", "A4", "simplex", "", "", "", 0.15),
    make_rule("r2", "A3", "simplex", "", "", "", 0.30),
  };
  auto md = make_metadata(1, "5", "1", "simplex", "monochrome", "A4");
  const auto* matched = common::find_best_rule(md, rules);
  ASSERT_NE(matched, nullptr);
  EXPECT_EQ(matched->rule_id, "r1");
}

TEST(PricingEngine, FindBestRule_WildcardWinsWhenSpecificMissing)
{
  std::vector<core::PricingRule> rules = {
    make_rule("r1", "A4", "simplex", "", "", "dwg", 0.50),  // paper+duplex+ext=3
    make_rule("r2", "A4", "simplex", "", "", "", 0.15),       // paper+duplex=2
  };
  auto md = make_metadata(1, "5", "1", "simplex", "monochrome", "A4", "pdf");
  const auto* matched = common::find_best_rule(md, rules);
  ASSERT_NE(matched, nullptr);
  EXPECT_EQ(matched->rule_id, "r2");
}

TEST(PricingEngine, FindBestRule_SpecificityWins)
{
  std::vector<core::PricingRule> rules = {
    make_rule("r_generic", "A4", "simplex", "", "", "", 0.15),             // spec 2
    make_rule("r_specific", "A4", "simplex", "Test Printer", "", "dwg", 0.50), // spec 4
  };
  auto md = make_metadata(1, "5", "1", "simplex", "monochrome", "A4", "dwg");
  const auto* matched = common::find_best_rule(md, rules);
  ASSERT_NE(matched, nullptr);
  EXPECT_EQ(matched->rule_id, "r_specific");
}

TEST(PricingEngine, FindBestRule_NoMatch)
{
  std::vector<core::PricingRule> rules = {
    make_rule("r1", "A3", "simplex", "", "", "", 0.30),
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

TEST(PricingEngine, FindBestRule_ColorModeMatching)
{
  std::vector<core::PricingRule> rules = {
    make_rule("r_mono", "A4", "simplex", "", "monochrome", "", 0.15),
    make_rule("r_color", "A4", "simplex", "", "color", "", 1.00),
  };
  auto md = make_metadata(1, "5", "1", "simplex", "color", "A4");
  const auto* matched = common::find_best_rule(md, rules, "color");
  ASSERT_NE(matched, nullptr);
  EXPECT_EQ(matched->rule_id, "r_color");

  matched = common::find_best_rule(md, rules, "monochrome");
  ASSERT_NE(matched, nullptr);
  EXPECT_EQ(matched->rule_id, "r_mono");
}

// ── compute_job_pricing (matrix rules) ────────────────────────────────────

TEST(PricingEngine, ComputeJobPricing_BasicRuleMatch)
{
  auto config = make_config_with_rules({
    make_rule("r1", "A4", "simplex", "", "", "", 0.15),
  });
  auto md = make_metadata(1, "5", "1", "simplex", "monochrome", "A4");

  common::PrinterProfileConfig printers;
  auto entry = common::compute_job_pricing(md, config, printers);
  EXPECT_EQ(entry.job_id, 1u);
  EXPECT_EQ(entry.total_pages, 5);
  EXPECT_EQ(entry.copies, 1);
  EXPECT_EQ(entry.effective_sheets, 5);
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
    make_rule("r1", "A4", "duplex", "", "", "", 0.25),
  });
  auto md = make_metadata(2, "3", "1", "long-edge (book)", "monochrome", "A4");

  common::PrinterProfileConfig printers;
  auto entry = common::compute_job_pricing(md, config, printers);
  EXPECT_EQ(entry.effective_sheets, 2);
  EXPECT_EQ(entry.duplex_full_sheets, 1);
  EXPECT_EQ(entry.simplex_tail_pages, 1);
  EXPECT_DOUBLE_EQ(entry.base_price, 1.0 * 0.25);
  EXPECT_DOUBLE_EQ(entry.tail_price, 0.0);
}

TEST(PricingEngine, ComputeJobPricing_SinglePageDuplexUsesSimplexRule)
{
  auto config = make_config_with_rules({
    make_rule("r_simplex", "A4", "simplex", "", "monochrome", "", 0.15),
    make_rule("r_duplex", "A4", "duplex", "", "monochrome", "", 0.25),
  });
  auto md = make_metadata(21, "1", "1", "long-edge (book)", "monochrome", "A4");

  common::PrinterProfileConfig printers;
  auto entry = common::compute_job_pricing(md, config, printers);

  EXPECT_FALSE(entry.price_pending);
  EXPECT_EQ(entry.effective_sheets, 1);
  EXPECT_EQ(entry.duplex_full_sheets, 0);
  EXPECT_EQ(entry.simplex_tail_pages, 1);
  EXPECT_EQ(entry.matched_rule_id, "r_simplex");
  EXPECT_DOUBLE_EQ(entry.total_price, 0.15);
}

TEST(PricingEngine, ComputeJobPricing_DashColorMatchesMonochromeRule)
{
  auto config = make_config_with_rules({
    make_rule("r_mono", "A4", "simplex", "", "monochrome", "", 0.15),
    make_rule("r_color", "A4", "simplex", "", "color", "", 1.00),
  });
  auto md = make_metadata(22, "2", "1", "simplex", "-", "A4", "", "HP_Color");

  common::PrinterProfileConfig printers;
  printers.profiles.push_back({"HP_Color", /*is_color_capable=*/true});

  auto entry = common::compute_job_pricing(md, config, printers);

  EXPECT_FALSE(entry.price_pending);
  EXPECT_FALSE(entry.color_downgraded);
  EXPECT_EQ(entry.matched_rule_id, "r_mono");
  EXPECT_DOUBLE_EQ(entry.total_price, 2.0 * 0.15);
}

TEST(PricingEngine, ComputeJobPricing_ColorPagesNotFilled_Pending)
{
  auto config = make_config_with_rules({
    make_rule("r1", "A4", "simplex", "", "", "", 0.15),
  });
  auto md = make_metadata(3, "2", "1", "simplex", "color", "A4");

  common::PrinterProfileConfig printers;
  printers.profiles.push_back({"Test Printer", /*is_color_capable=*/true});
  auto entry = common::compute_job_pricing(md, config, printers);
  EXPECT_TRUE(entry.price_pending);
  EXPECT_EQ(entry.pending_reason, "awaiting_color_pages");
  EXPECT_DOUBLE_EQ(entry.total_price, 0.0);
}

TEST(PricingEngine, ComputeJobPricing_ColorPagesFilled_SplitPricing)
{
  // Color printer, color job, operator filled m=3.
  // Simplex: each portion goes through sheet breakdown independently.
  auto config = make_config_with_rules({
    make_rule("r_mono", "A4", "simplex", "", "monochrome", "", 0.15),
    make_rule("r_color", "A4", "simplex", "", "color", "", 1.00),
  });
  auto md = make_metadata(30, "10", "1", "simplex", "color", "A4", "", "HP_Color");

  common::PrinterProfileConfig printers;
  printers.profiles.push_back({"HP_Color", /*is_color_capable=*/true});

  // m = 3 color pages out of 10 total pages, simplex.
  auto entry = common::compute_job_pricing(md, config, printers, /*color_pages=*/3);

  EXPECT_FALSE(entry.price_pending);
  EXPECT_EQ(entry.color_pages, 3);
  EXPECT_EQ(entry.mono_pages, 7);

  // color: 3 simplex pages = 3 sheets × 1.00 = 3.00
  // mono: 7 simplex pages = 7 sheets × 0.15 = 1.05
  // total = 4.05
  EXPECT_DOUBLE_EQ(entry.color_subtotal, 3.0 * 1.00);
  EXPECT_DOUBLE_EQ(entry.mono_subtotal, 7.0 * 0.15);
  EXPECT_DOUBLE_EQ(entry.total_price, 3.0 + 1.05);
}

TEST(PricingEngine, ComputeJobPricing_ColorPagesZero_PriceIsZero)
{
  auto config = make_config_with_rules({
    make_rule("r_mono", "A4", "simplex", "", "monochrome", "", 0.15),
    make_rule("r_color", "A4", "simplex", "", "color", "", 1.00),
  });
  auto md = make_metadata(31, "5", "1", "simplex", "color", "A4", "", "HP_Color");

  common::PrinterProfileConfig printers;
  printers.profiles.push_back({"HP_Color", /*is_color_capable=*/true});

  auto entry = common::compute_job_pricing(md, config, printers, /*color_pages=*/0);
  EXPECT_TRUE(entry.price_pending);
  EXPECT_EQ(entry.pending_reason, "awaiting_color_pages");
  EXPECT_DOUBLE_EQ(entry.total_price, 0.0);
  EXPECT_EQ(entry.color_pages, 0);
  EXPECT_EQ(entry.mono_pages, 5);
}

TEST(PricingEngine, ComputeJobPricing_ColorNotPendingWhenDisabled)
{
  // color_price_pending=false is irrelevant now; m=0 always means pending.
  auto config = make_config_with_rules({
    make_rule("r1", "A4", "simplex", "", "", "", 0.15),
  }, /*color_price_pending=*/false);
  auto md = make_metadata(5, "1", "1", "simplex", "color", "A4");

  common::PrinterProfileConfig printers;
  printers.profiles.push_back({"Test Printer", /*is_color_capable=*/true});
  auto entry = common::compute_job_pricing(md, config, printers);
  EXPECT_TRUE(entry.price_pending);
  EXPECT_EQ(entry.pending_reason, "awaiting_color_pages");
}

TEST(PricingEngine, ComputeJobPricing_UnmatchedRule)
{
  auto config = make_config_with_rules({
    make_rule("r1", "A3", "simplex", "", "", "", 0.30),
  });
  auto md = make_metadata(6, "5", "1", "simplex", "monochrome", "A4");

  common::PrinterProfileConfig printers;
  auto entry = common::compute_job_pricing(md, config, printers);
  EXPECT_TRUE(entry.price_pending);
  EXPECT_EQ(entry.pending_reason, "unmatched_rule");
}

TEST(PricingEngine, ComputeJobPricing_WithCopies)
{
  auto config = make_config_with_rules({
    make_rule("r1", "A4", "simplex", "", "", "", 0.15),
  });
  auto md = make_metadata(7, "3", "2", "simplex", "monochrome", "A4");

  common::PrinterProfileConfig printers;
  auto entry = common::compute_job_pricing(md, config, printers);
  EXPECT_EQ(entry.total_pages, 3);
  EXPECT_EQ(entry.copies, 2);
  EXPECT_EQ(entry.effective_sheets, 6);
  EXPECT_DOUBLE_EQ(entry.base_price, 6.0 * 0.15);
}

// ── Printer-name matching tests ────────────────────────────────────────

TEST(PricingEngine, FindBestRule_PrinterSpecificMatch)
{
  std::vector<core::PricingRule> rules = {
    make_rule("r1", "A4", "simplex", "HP_LaserJet", "", "", 0.15),
    make_rule("r2", "A4", "simplex", "Canon_iR", "", "", 0.30),
  };
  auto md = make_metadata(1, "5", "1", "simplex", "monochrome", "A4", "", "HP_LaserJet");
  const auto* matched = common::find_best_rule(md, rules);
  ASSERT_NE(matched, nullptr);
  EXPECT_EQ(matched->rule_id, "r1");
}

TEST(PricingEngine, FindBestRule_WildcardPrinterMatchesAny)
{
  std::vector<core::PricingRule> rules = {
    make_rule("r1", "A4", "simplex", "", "", "", 0.15),
  };
  auto md = make_metadata(2, "5", "1", "simplex", "monochrome", "A4", "", "AnyPrinter");
  const auto* matched = common::find_best_rule(md, rules);
  ASSERT_NE(matched, nullptr);
  EXPECT_EQ(matched->rule_id, "r1");
}

TEST(PricingEngine, FindBestRule_PrinterMismatch)
{
  std::vector<core::PricingRule> rules = {
    make_rule("r1", "A4", "simplex", "HP_LaserJet", "", "", 0.15),
  };
  auto md = make_metadata(3, "5", "1", "simplex", "monochrome", "A4", "", "Canon_iR");
  const auto* matched = common::find_best_rule(md, rules);
  EXPECT_EQ(matched, nullptr);
}

TEST(PricingEngine, ComputeJobPricing_BWPrinterColorDowngrade)
{
  auto config = make_config_with_rules({
    make_rule("r1", "A4", "simplex", "", "", "", 0.15),
  });
  auto md = make_metadata(10, "3", "1", "simplex", "color", "A4", "", "BW_Printer");

  common::PrinterProfileConfig printers;
  printers.profiles.push_back({"BW_Printer", /*is_color_capable=*/false});

  auto entry = common::compute_job_pricing(md, config, printers);
  EXPECT_TRUE(entry.color_downgraded);
  EXPECT_EQ(entry.note, "color_on_bw_printer");
  EXPECT_FALSE(entry.price_pending);
}

TEST(PricingEngine, ComputeJobPricing_ColorPrinterSpecificRule_ColorPagesNotFilled)
{
  auto config = make_config_with_rules({
    make_rule("r1", "A4", "simplex", "HP_Color", "", "", 0.50),
  });
  auto md = make_metadata(11, "2", "1", "simplex", "color", "A4", "", "HP_Color");

  common::PrinterProfileConfig printers;
  printers.profiles.push_back({"HP_Color", /*is_color_capable=*/true});

  auto entry = common::compute_job_pricing(md, config, printers);
  EXPECT_TRUE(entry.price_pending);
  EXPECT_EQ(entry.pending_reason, "awaiting_color_pages");
  EXPECT_DOUBLE_EQ(entry.total_price, 0.0);
}

TEST(PricingEngine, ComputeJobPricing_ColorPrinterWildcardRule_ColorPagesNotFilled)
{
  auto config = make_config_with_rules({
    make_rule("r1", "A4", "simplex", "", "", "", 0.15),
  });
  auto md = make_metadata(12, "2", "1", "simplex", "color", "A4", "", "HP_Color");

  common::PrinterProfileConfig printers;
  printers.profiles.push_back({"HP_Color", /*is_color_capable=*/true});

  auto entry = common::compute_job_pricing(md, config, printers);
  EXPECT_TRUE(entry.price_pending);
  EXPECT_EQ(entry.pending_reason, "awaiting_color_pages");
  EXPECT_DOUBLE_EQ(entry.total_price, 0.0);
}

TEST(PricingEngine, ComputeJobPricing_MatchedPrinterName)
{
  auto config = make_config_with_rules({
    make_rule("r1", "A4", "simplex", "HP_LaserJet", "", "", 0.15),
  });
  auto md = make_metadata(13, "1", "1", "simplex", "monochrome", "A4", "", "HP_LaserJet");

  common::PrinterProfileConfig printers;
  auto entry = common::compute_job_pricing(md, config, printers);
  EXPECT_EQ(entry.matched_printer_name, "HP_LaserJet");
}

// ── Split pricing (color pages on color printer) ─────────────────────────

TEST(PricingEngine, ComputeJobPricing_BWPrinter_NoSplit)
{
  auto config = make_config_with_rules({
    make_rule("r1", "A4", "simplex", "", "", "", 0.15),
  });
  auto md = make_metadata(32, "5", "1", "simplex", "color", "A4", "", "BW_Printer");

  common::PrinterProfileConfig printers;
  printers.profiles.push_back({"BW_Printer", /*is_color_capable=*/false});

  auto entry = common::compute_job_pricing(md, config, printers);
  EXPECT_TRUE(entry.color_downgraded);
  EXPECT_FALSE(entry.price_pending);
  EXPECT_DOUBLE_EQ(entry.total_price, 5.0 * 0.15);
}

TEST(PricingEngine, ComputeJobPricing_ColorPagesFilled_DuplexSplit)
{
  // User's example: 17 duplex pages, 4 color.
  // color: 4 pages duplex → 2 sheets × 1.00 = 2.00
  // mono: 13 pages duplex → 6 full sheets × 0.25 + 1 tail × 0.15 = 1.50 + 0.15 = 1.65
  // total = 3.65
  auto config = make_config_with_rules({
    make_rule("r_mono_d", "A4", "duplex", "", "monochrome", "", 0.25),
    make_rule("r_mono_s", "A4", "simplex", "", "monochrome", "", 0.15),
    make_rule("r_color_d", "A4", "duplex", "", "color", "", 1.00),
    make_rule("r_color_s", "A4", "simplex", "", "color", "", 1.00),
  });

  auto md = make_metadata(33, "17", "1", "long-edge (book)", "color", "A4", "", "HP_Color");

  common::PrinterProfileConfig printers;
  printers.profiles.push_back({"HP_Color", /*is_color_capable=*/true});

  auto entry = common::compute_job_pricing(md, config, printers, /*color_pages=*/4);

  EXPECT_FALSE(entry.price_pending);
  EXPECT_EQ(entry.color_pages, 4);
  EXPECT_EQ(entry.mono_pages, 13);

  // color: 4 pages duplex → 2 sheets × 1.00 = 2.00
  EXPECT_DOUBLE_EQ(entry.color_subtotal, 2.0 * 1.00);
  // mono: 13 pages duplex → 6 sheets × 0.25 + 1 tail × 0.15 = 1.65
  EXPECT_DOUBLE_EQ(entry.mono_subtotal, 6.0 * 0.25 + 1.0 * 0.15);
  EXPECT_DOUBLE_EQ(entry.total_price, 2.00 + 1.65);
}

// ── Single-page portion uses simplex rule in duplex jobs ───────────────────

TEST(PricingEngine, ComputeJobPricing_ColorDuplex_SingleColorPage_UsesSimplex)
{
  // Color duplex job, 4 pages, m=1.
  // 1 color page cannot physically be duplex → use simplex color price.
  // color: 1 page simplex → 1 × 1.00 = 1.00
  // mono: 3 pages duplex → 1 sheet × 0.25 + 1 tail × 0.15 = 0.40
  // total = 1.40
  auto config = make_config_with_rules({
    make_rule("r_mono_d", "", "duplex", "", "monochrome", "", 0.25),
    make_rule("r_mono_s", "", "simplex", "", "monochrome", "", 0.15),
    make_rule("r_color_d", "", "duplex", "", "color", "", 2.00),
    make_rule("r_color_s", "", "simplex", "", "color", "", 1.00),
  });

  auto md = make_metadata(40, "4", "1", "long-edge (book)", "color", "A4", "", "HP_Color");

  common::PrinterProfileConfig printers;
  printers.profiles.push_back({"HP_Color", /*is_color_capable=*/true});

  auto entry = common::compute_job_pricing(md, config, printers, /*color_pages=*/1);

  EXPECT_FALSE(entry.price_pending);
  EXPECT_EQ(entry.color_pages, 1);
  EXPECT_EQ(entry.mono_pages, 3);

  // color: 1 page simplex → 1 × 1.00 = 1.00
  EXPECT_DOUBLE_EQ(entry.color_subtotal, 1.0 * 1.00);
  // mono: 3 pages duplex → 1 sheet × 0.25 + 1 tail × 0.15 = 0.40
  EXPECT_DOUBLE_EQ(entry.mono_subtotal, 1.0 * 0.25 + 1.0 * 0.15);
  // total = 1.40
  EXPECT_DOUBLE_EQ(entry.total_price, 1.00 + 0.40);
}

TEST(PricingEngine, ComputeJobPricing_ColorDuplex_SingleMonoPage_UsesSimplex)
{
  // Color duplex job, 4 pages, m=3.
  // color: 3 pages duplex → 1 sheet × 2.00 + 1 tail × 1.00 = 3.00
  // mono: 1 page → cannot be duplex → 1 × 0.15 = 0.15
  // total = 3.15
  auto config = make_config_with_rules({
    make_rule("r_mono_d", "", "duplex", "", "monochrome", "", 0.25),
    make_rule("r_mono_s", "", "simplex", "", "monochrome", "", 0.15),
    make_rule("r_color_d", "", "duplex", "", "color", "", 2.00),
    make_rule("r_color_s", "", "simplex", "", "color", "", 1.00),
  });

  auto md = make_metadata(41, "4", "1", "long-edge (book)", "color", "A4", "", "HP_Color");

  common::PrinterProfileConfig printers;
  printers.profiles.push_back({"HP_Color", /*is_color_capable=*/true});

  auto entry = common::compute_job_pricing(md, config, printers, /*color_pages=*/3);

  EXPECT_FALSE(entry.price_pending);
  EXPECT_EQ(entry.color_pages, 3);
  EXPECT_EQ(entry.mono_pages, 1);

  // color: 3 pages duplex → 1 sheet × 2.00 + 1 tail × 1.00 = 3.00
  EXPECT_DOUBLE_EQ(entry.color_subtotal, 1.0 * 2.00 + 1.0 * 1.00);
  // mono: 1 page → not duplex → 1 × 0.15 = 0.15
  EXPECT_DOUBLE_EQ(entry.mono_subtotal, 1.0 * 0.15);
  // total = 3.15
  EXPECT_DOUBLE_EQ(entry.total_price, 3.00 + 0.15);
}

// ── Default rules fallback ─────────────────────────────────────────────────

TEST(PricingEngine, ComputeJobPricing_TestMatrix50Cases)
{
  // Test matrix:
  //   monochrome simplex = 0.15
  //   monochrome duplex  = 0.25
  //   color simplex      = 1.00
  //   color duplex       = 2.00
  auto config = make_config_with_rules({
    make_rule("matrix_mono_simplex", "A4", "simplex", "", "monochrome", "", 0.15),
    make_rule("matrix_mono_duplex",  "A4", "duplex",  "", "monochrome", "", 0.25),
    make_rule("matrix_color_simplex", "A4", "simplex", "", "color", "", 1.00),
    make_rule("matrix_color_duplex",  "A4", "duplex",  "", "color", "", 2.00),
  });

  common::PrinterProfileConfig printers;
  printers.profiles.push_back({"Color_Printer", /*is_color_capable=*/true});
  printers.profiles.push_back({"BW_Printer", /*is_color_capable=*/false});

  struct Case
  {
    const char* name;
    int pages;
    int copies;
    const char* duplex;
    const char* color;
    const char* printer;
    int m;
    double expected_price;
    bool expected_pending;
    const char* expected_reason;
    bool expected_downgraded;
  };

  const std::vector<Case> cases = {
    {"mono_simplex_1p_c1", 1, 1, "simplex", "monochrome", "BW_Printer", 0, 0.15, false, "", false},
    {"mono_simplex_2p_c1", 2, 1, "simplex", "monochrome", "BW_Printer", 0, 0.30, false, "", false},
    {"mono_simplex_5p_c1", 5, 1, "simplex", "monochrome", "BW_Printer", 0, 0.75, false, "", false},
    {"mono_simplex_10p_c2", 10, 2, "simplex", "monochrome", "BW_Printer", 0, 3.00, false, "", false},
    {"mono_simplex_1p_c3", 1, 3, "simplex", "monochrome", "BW_Printer", 0, 0.45, false, "", false},
    {"mono_simplex_7p_c4", 7, 4, "simplex", "monochrome", "BW_Printer", 0, 4.20, false, "", false},

    {"mono_duplex_1p_c1", 1, 1, "long-edge (book)", "monochrome", "BW_Printer", 0, 0.15, false, "", false},
    {"mono_duplex_2p_c1", 2, 1, "long-edge (book)", "monochrome", "BW_Printer", 0, 0.25, false, "", false},
    {"mono_duplex_3p_c1", 3, 1, "long-edge (book)", "monochrome", "BW_Printer", 0, 0.40, false, "", false},
    {"mono_duplex_4p_c1", 4, 1, "long-edge (book)", "monochrome", "BW_Printer", 0, 0.50, false, "", false},
    {"mono_duplex_5p_c1", 5, 1, "long-edge (book)", "monochrome", "BW_Printer", 0, 0.65, false, "", false},
    {"mono_duplex_10p_c1", 10, 1, "long-edge (book)", "monochrome", "BW_Printer", 0, 1.25, false, "", false},
    {"mono_duplex_3p_c2", 3, 2, "long-edge (book)", "monochrome", "BW_Printer", 0, 0.80, false, "", false},
    {"mono_duplex_5p_c2", 5, 2, "long-edge (book)", "monochrome", "BW_Printer", 0, 1.30, false, "", false},
    {"mono_duplex_7p_c3", 7, 3, "long-edge (book)", "monochrome", "BW_Printer", 0, 2.70, false, "", false},

    {"dash_simplex_4p_color_printer", 4, 1, "simplex", "-", "Color_Printer", 0, 0.60, false, "", false},
    {"dash_duplex_5p_color_printer", 5, 1, "long-edge (book)", "-", "Color_Printer", 0, 0.65, false, "", false},
    {"empty_color_simplex_6p", 6, 1, "simplex", "", "Color_Printer", 0, 0.90, false, "", false},
    {"grayscale_duplex_6p", 6, 1, "long-edge (book)", "grayscale", "Color_Printer", 0, 0.75, false, "", false},
    {"misspelled_monchorme_simplex_2p", 2, 1, "simplex", "monchorme", "Color_Printer", 0, 0.30, false, "", false},

    {"color_bw_simplex_4p_m2", 4, 1, "simplex", "color", "BW_Printer", 2, 0.60, false, "", true},
    {"color_bw_duplex_5p_m3", 5, 1, "long-edge (book)", "color", "BW_Printer", 3, 0.65, false, "", true},
    {"color_bw_duplex_1p_m1", 1, 1, "long-edge (book)", "color", "BW_Printer", 1, 0.15, false, "", true},
    {"color_bw_duplex_7p_c2_m4", 7, 2, "long-edge (book)", "color", "BW_Printer", 4, 1.80, false, "", true},

    {"color_pending_simplex_1p_m0", 1, 1, "simplex", "color", "Color_Printer", 0, 0.00, true, "awaiting_color_pages", false},
    {"color_pending_simplex_5p_m0", 5, 1, "simplex", "color", "Color_Printer", 0, 0.00, true, "awaiting_color_pages", false},
    {"color_pending_duplex_2p_m0", 2, 1, "long-edge (book)", "color", "Color_Printer", 0, 0.00, true, "awaiting_color_pages", false},
    {"color_pending_duplex_5p_m0", 5, 1, "long-edge (book)", "color", "Color_Printer", 0, 0.00, true, "awaiting_color_pages", false},
    {"color_pending_duplex_10p_c2_m0", 10, 2, "long-edge (book)", "color", "Color_Printer", 0, 0.00, true, "awaiting_color_pages", false},

    {"color_simplex_1p_m1", 1, 1, "simplex", "color", "Color_Printer", 1, 1.00, false, "", false},
    {"color_simplex_2p_m1", 2, 1, "simplex", "color", "Color_Printer", 1, 1.15, false, "", false},
    {"color_simplex_5p_m1", 5, 1, "simplex", "color", "Color_Printer", 1, 1.60, false, "", false},
    {"color_simplex_5p_m2", 5, 1, "simplex", "color", "Color_Printer", 2, 2.45, false, "", false},
    {"color_simplex_5p_m5", 5, 1, "simplex", "color", "Color_Printer", 5, 5.00, false, "", false},
    {"color_simplex_10p_m3", 10, 1, "simplex", "color", "Color_Printer", 3, 4.05, false, "", false},
    {"color_simplex_10p_c2_m3", 10, 2, "simplex", "color", "Color_Printer", 3, 8.10, false, "", false},
    {"color_simplex_m_clamped_4p_m8", 4, 1, "simplex", "color", "Color_Printer", 8, 4.00, false, "", false},

    {"color_duplex_1p_m1", 1, 1, "long-edge (book)", "color", "Color_Printer", 1, 1.00, false, "", false},
    {"color_duplex_2p_m1", 2, 1, "long-edge (book)", "color", "Color_Printer", 1, 1.15, false, "", false},
    {"color_duplex_2p_m2", 2, 1, "long-edge (book)", "color", "Color_Printer", 2, 2.00, false, "", false},
    {"color_duplex_3p_m1", 3, 1, "long-edge (book)", "color", "Color_Printer", 1, 1.25, false, "", false},
    {"color_duplex_3p_m2", 3, 1, "long-edge (book)", "color", "Color_Printer", 2, 2.15, false, "", false},
    {"color_duplex_3p_m3", 3, 1, "long-edge (book)", "color", "Color_Printer", 3, 3.00, false, "", false},
    {"color_duplex_4p_m1", 4, 1, "long-edge (book)", "color", "Color_Printer", 1, 1.40, false, "", false},
    {"color_duplex_4p_m3", 4, 1, "long-edge (book)", "color", "Color_Printer", 3, 3.15, false, "", false},
    {"color_duplex_5p_m2", 5, 1, "long-edge (book)", "color", "Color_Printer", 2, 2.40, false, "", false},
    {"color_duplex_5p_m4", 5, 1, "long-edge (book)", "color", "Color_Printer", 4, 4.15, false, "", false},
    {"color_duplex_5p_m5", 5, 1, "long-edge (book)", "color", "Color_Printer", 5, 5.00, false, "", false},
    {"color_duplex_10p_m3", 10, 1, "long-edge (book)", "color", "Color_Printer", 3, 3.90, false, "", false},
    {"color_duplex_17p_c2_m4", 17, 2, "long-edge (book)", "color", "Color_Printer", 4, 11.30, false, "", false},
  };

  ASSERT_EQ(cases.size(), 50u);

  for (const auto& tc : cases)
  {
    SCOPED_TRACE(tc.name);
    auto md = make_metadata(900, std::to_string(tc.pages), std::to_string(tc.copies),
                            tc.duplex, tc.color, "A4", "", tc.printer);
    auto entry = common::compute_job_pricing(md, config, printers, tc.m);

    EXPECT_EQ(entry.total_pages, tc.pages);
    EXPECT_EQ(entry.copies, tc.copies);
    EXPECT_EQ(entry.price_pending, tc.expected_pending);
    EXPECT_EQ(entry.pending_reason, std::string(tc.expected_reason));
    EXPECT_EQ(entry.color_downgraded, tc.expected_downgraded);
    EXPECT_NEAR(entry.total_price, tc.expected_price, 0.000001);

    if (std::string(tc.color) == "color" && std::string(tc.printer) == "Color_Printer")
    {
      const int expected_m = std::max(0, std::min(tc.m, tc.pages));
      EXPECT_EQ(entry.color_pages, expected_m);
      EXPECT_EQ(entry.mono_pages, tc.pages - expected_m);
    }
  }
}

TEST(PricingEngine, ComputeJobPricing_FallsBackToDefaultsWhenRulesEmpty)
{
  common::PricingConfig config;
  config.rules.clear();
  config.color_price_pending = false;
  auto md = make_metadata(20, "3", "1", "long-edge (book)", "monochrome", "A4");

  common::PrinterProfileConfig printers;
  auto entry = common::compute_job_pricing(md, config, printers);
  EXPECT_FALSE(entry.price_pending);
  EXPECT_EQ(entry.effective_sheets, 2);
  EXPECT_EQ(entry.duplex_full_sheets, 1);
  EXPECT_EQ(entry.simplex_tail_pages, 1);
}
