/// @file print_ticket_parser.cpp
/// PrintTicket XML parser — extracts standard Print Schema keywords.
///
/// Strategy: string-based XML extraction (no external XML library).
/// This is deliberately minimal and fast; it processes PrintTicket XML
/// produced by any XPSDrv v4 driver regardless of brand, namespace
/// prefix, or driver flavour.
///
/// Supported extraction paths:
///   1. <*:Feature name="psk:FeatureName"> … <*:Option name="psk:Value" />
///      — the standard Feature/Option pattern.
///   2. <*:ParameterRef name="psk:FeatureName" />
///      <*:ParameterInit name="psk:FeatureName">
///        <*:Value …>N</*:Value>
///      — the ParameterRef/ParameterInit pattern used for copies.

#include "parser/print_ticket_parser.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

namespace wpce::parser
{
namespace
{

// ── Generic XML helpers — prefix-agnostic ───────────────────────────────

/// Find the first occurrence of `needle` in `haystack`, starting at `pos`.
constexpr std::size_t find_at(std::string_view haystack,
                                std::string_view needle,
                                std::size_t pos = 0) noexcept
{
  return haystack.find(needle, pos);
}

/// Find the end (index of '>') of the XML tag that starts at `tag_start`.
/// Skips over quoted attribute values so '>' inside an attribute does
/// not terminate the scan prematurely.  Handles single- and
/// double-quoted attribute values.  Returns npos if no closing '>' is
/// found.  The returned position points at the '>' character (the tag
/// content ends at the character before it; for self-closing tags that
/// is '/').
constexpr std::size_t find_tag_end(std::string_view xml,
                                    std::size_t tag_start) noexcept
{
  if (tag_start >= xml.size() || xml[tag_start] != '<')
    return std::string_view::npos;

  std::size_t pos = tag_start + 1;
  char in_quote = '\0';
  while (pos < xml.size())
  {
    const char ch = xml[pos];
    if (in_quote != '\0')
    {
      if (ch == in_quote) in_quote = '\0';
      ++pos;
      continue;
    }
    if (ch == '"' || ch == '\'')
    {
      in_quote = ch;
      ++pos;
      continue;
    }
    if (ch == '>')
      return pos;
    ++pos;
  }
  return std::string_view::npos;
}

/// Extract the value of a named attribute from an XML opening tag.
/// e.g. attr_value(tag, "name") from `<psf:Feature name="psk:ISOA4">`
/// returns "psk:ISOA4".
/// Prefix-agnostic — works with psf:, xps:, wprt:, or custom prefixes.
std::string attr_value(std::string_view tag, std::string_view attr_name)
{
  std::string pattern;
  pattern.reserve(attr_name.size() + 3);
  pattern += attr_name;
  pattern += "=\"";
  std::size_t start = find_at(tag, pattern);
  if (start == std::string_view::npos) return {};
  start += pattern.size();
  std::size_t end = find_at(tag, "\"", start);
  if (end == std::string_view::npos) return {};
  return std::string(tag.substr(start, end - start));
}

/// Find a Feature by its Print Schema keyword name (e.g. "psk:PageMediaSize").
/// Returns the selected option name, or empty if not found.
///
/// Prefix-agnostic: matches <*:Feature name="psk:..." > regardless of
/// whether the namespace prefix is psf, xps, wprt, or a driver-specific
/// prefix like ns0000.
///
/// Handles two patterns:
///   A) <*:Option … selected="true" … /> — the selected option
///   B) First <*:Option … /> when no selected="true" is present
///
/// Robustness fixes:
///   - Correctly locates the Feature element's end tag by tracking
///     nesting depth (Feature → Option → ScoredProperty), so nested
///     self-closing tags no longer confuse the boundary detection.
///   - When the same feature name appears multiple times in the XML
///     (e.g. both Job- and Page-level PrintTicket concatenated), the
///     caller may pass a search start position; otherwise the first
///     occurrence is used, which is the convention for a single
///     PrintTicket document.
///   - Self-closing Feature tags (<*:Feature name="..." />) are handled
///     without mistaking a nested "/>" for the Feature's end.
std::string find_feature_option(std::string_view xml,
                                 std::string_view feature_name)
{
  // Build the search pattern: name="psk:FeatureName"
  std::string pattern;
  pattern.reserve(feature_name.size() + 8);
  pattern += "name=\"";
  pattern += feature_name;
  pattern += "\"";
  std::size_t feat_pos = find_at(xml, pattern);
  if (feat_pos == std::string_view::npos) return {};

  // Find the enclosing Feature tag start (prefix-agnostic: <*:Feature).
  // Walk back from feat_pos to find "<" then check for ":Feature ".
  std::size_t tag_start = xml.rfind('<', feat_pos);
  if (tag_start == std::string_view::npos) return {};
  std::size_t colon = find_at(xml, ":Feature ", tag_start);
  if (colon == std::string_view::npos || colon > feat_pos)
    return {};  // not inside a Feature tag

  // Find the end of the Feature opening tag (the first '>' after tag_start).
  // We must skip over quoted attribute values so '>' inside an attribute
  // does not fool us.
  std::size_t open_tag_end = find_tag_end(xml, tag_start);
  if (open_tag_end == std::string_view::npos) return {};

  // Self-closing Feature: <*:Feature name="..." /> — no Options inside.
  if (open_tag_end >= 1 && xml[open_tag_end - 1] == '/')
    return {};

  // Extract the prefix from this tag to build the closing tag pattern.
  // tag_start points at '<'; colon points at the ':' before 'Feature'.
  // The prefix is the characters between them, excluding both: e.g. "psf".
  std::string_view prefix = xml.substr(tag_start + 1, colon - (tag_start + 1));
  std::string close_pattern;
  close_pattern.reserve(prefix.size() + 14);
  close_pattern += "</";
  close_pattern += prefix;
  close_pattern += ":Feature>";

  // Find the matching close tag by tracking nesting depth of the same
  // element name.  We scan forward from the end of the opening tag and
  // increment depth for each nested <prefix:Feature and decrement for
  // each </prefix:Feature>.  Depth starts at 1 (the opening tag itself).
  std::string open_pattern;
  open_pattern.reserve(prefix.size() + 10);
  open_pattern += "<";
  open_pattern += prefix;
  open_pattern += ":Feature";

  std::size_t scan = open_tag_end + 1;
  int depth = 1;
  std::size_t feat_end = std::string_view::npos;
  while (scan < xml.size())
  {
    std::size_t next_open = find_at(xml, open_pattern, scan);
    std::size_t next_close = find_at(xml, close_pattern, scan);

    if (next_close == std::string_view::npos) break;
    if (next_open != std::string_view::npos && next_open < next_close)
    {
      ++depth;
      scan = next_open + open_pattern.size();
      continue;
    }

    --depth;
    if (depth == 0)
    {
      feat_end = next_close;
      break;
    }
    scan = next_close + close_pattern.size();
  }

  if (feat_end == std::string_view::npos) return {};

  std::string_view body = xml.substr(open_tag_end + 1,
                                      feat_end - open_tag_end - 1);

  // ── Pattern A: find selected="true" ─────────────────────────────────
  // Walk every occurrence of selected="true" and find the enclosing
  // <*:Option …> tag (prefix-agnostic).
  std::size_t opt_pos = 0;
  while (opt_pos < body.size())
  {
    auto sel = find_at(body, "selected=\"true\"", opt_pos);
    if (sel == std::string_view::npos) break;

    // Walk backwards to the nearest opening Option tag.
    std::size_t nearest_option = body.rfind(":Option", sel);
    if (nearest_option == std::string_view::npos)
    {
      opt_pos = sel + 14;
      continue;
    }

    // Verify this ":Option" is an opening tag (has '<' before it and
    // the preceding character is not '/').
    std::size_t open_brace = body.rfind('<', nearest_option);
    if (open_brace == std::string_view::npos)
    {
      opt_pos = sel + 14;
      continue;
    }
    if (open_brace > 0 && body[open_brace - 1] == '/')
    {
      // </*:Option — closing tag, not an opening tag.
      opt_pos = sel + 14;
      continue;
    }

    // Confirm the Option tag actually encloses the selected="true"
    // attribute by checking the tag's end is past `sel`.
    std::size_t tag_end = find_tag_end(body, open_brace);
    if (tag_end == std::string_view::npos || tag_end < sel)
    {
      opt_pos = sel + 14;
      continue;
    }

    std::string_view tag(body.data() + open_brace, tag_end - open_brace + 1);
    std::string name = attr_value(tag, "name");
    if (!name.empty()) return name;

    opt_pos = sel + 14;
  }

  // ── Pattern B: first Option (fallback) ──────────────────────────────
  // Prefix-agnostic: search for ":Option name=\""
  opt_pos = find_at(body, ":Option name=\"");
  if (opt_pos == std::string_view::npos) return {};

  // Walk back to the '<' that starts this Option tag, then find the
  // tag end.  This correctly handles attributes that contain ">"
  // inside quoted values and self-closing Options.
  std::size_t open_brace = body.rfind('<', opt_pos);
  if (open_brace == std::string_view::npos) return {};
  std::size_t tag_end = find_tag_end(body, open_brace);
  if (tag_end == std::string_view::npos) return {};

  std::string_view tag(body.data() + open_brace, tag_end - open_brace + 1);
  return attr_value(tag, "name");
}

/// Find a ParameterInit by its Print Schema keyword name and extract the
/// integer value inside <*:Value xsi:type="xsd:integer">N</*:Value>.
///
/// This handles the common case where copies (psk:JobCopiesAllDocuments)
/// are specified via ParameterRef + ParameterInit rather than
/// Feature/Option.  The Windows print dialog produces this pattern for
/// nearly all copies specifications.
///
/// Returns the integer value, or -1 if not found.
int find_parameter_init_int(std::string_view xml,
                             std::string_view param_name)
{
  // Build search pattern: name="psk:ParameterName"
  std::string pattern;
  pattern.reserve(param_name.size() + 8);
  pattern += "name=\"";
  pattern += param_name;
  pattern += "\"";

  // Find the ParameterInit (not ParameterRef).
  std::size_t pos = 0;
  while (pos < xml.size())
  {
    // Find next <*:ParameterInit
    auto init_pos = find_at(xml, ":ParameterInit ", pos);
    if (init_pos == std::string_view::npos) return -1;

    // Does this one have our name?
    auto name_pos = find_at(xml, pattern, init_pos);
    if (name_pos == std::string_view::npos ||
        name_pos > find_at(xml, ">", init_pos))
    {
      // Name not in this ParameterInit — skip past it.
      auto close = find_at(xml, ":ParameterInit>", init_pos);
      pos = (close != std::string_view::npos) ? close + 16 : init_pos + 16;
      continue;
    }

    // Found the right ParameterInit — extract the integer Value.
    auto value_start = find_at(xml, ":Value", name_pos);
    if (value_start == std::string_view::npos) return -1;

    auto value_content = find_at(xml, ">", value_start);
    if (value_content == std::string_view::npos) return -1;
    ++value_content;  // skip past '>'

    auto value_end = find_at(xml, "<", value_content);
    if (value_end == std::string_view::npos) return -1;

    std::string_view num_text = xml.substr(value_content,
                                            value_end - value_content);
    // Trim whitespace.
    while (!num_text.empty() && (num_text.front() == ' ' || num_text.front() == '\t' ||
                                   num_text.front() == '\r' || num_text.front() == '\n'))
      num_text.remove_prefix(1);
    while (!num_text.empty() && (num_text.back() == ' ' || num_text.back() == '\t' ||
                                   num_text.back() == '\r' || num_text.back() == '\n'))
      num_text.remove_suffix(1);

    char* end = nullptr;
    long val = std::strtol(num_text.data(), &end, 10);
    if (end != num_text.data() + num_text.size()) return -1;
    return static_cast<int>(val);
  }

  return -1;
}

/// Find a ParameterInit by its Print Schema keyword name and extract the
/// string value inside <*:Value xsi:type="xsd:string">…</*:Value>.
///
/// Used for psk:PageRange, which carries a human-readable page range
/// string such as "1-3,5,7-9".  Returns empty string if not found or
/// if the ParameterInit has no string Value child.
std::string find_parameter_init_string(std::string_view xml,
                                        std::string_view param_name)
{
  std::string pattern;
  pattern.reserve(param_name.size() + 8);
  pattern += "name=\"";
  pattern += param_name;
  pattern += "\"";

  std::size_t pos = 0;
  while (pos < xml.size())
  {
    auto init_pos = find_at(xml, ":ParameterInit ", pos);
    if (init_pos == std::string_view::npos) return {};

    auto name_pos = find_at(xml, pattern, init_pos);
    auto tag_end_pos = find_tag_end(xml, xml.rfind('<', init_pos));
    if (name_pos == std::string_view::npos ||
        tag_end_pos == std::string_view::npos ||
        name_pos > tag_end_pos)
    {
      // Name not in this opening tag — skip to the next ParameterInit.
      pos = init_pos + 16;
      continue;
    }

    // Found the right ParameterInit — extract the string Value.
    auto value_start = find_at(xml, ":Value", tag_end_pos);
    if (value_start == std::string_view::npos) return {};

    auto value_content = find_at(xml, ">", value_start);
    if (value_content == std::string_view::npos) return {};
    ++value_content;

    auto value_end = find_at(xml, "<", value_content);
    if (value_end == std::string_view::npos) return {};

    std::string text(xml.substr(value_content, value_end - value_content));
    // Trim whitespace.
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                              text.front() == '\r' || text.front() == '\n'))
      text.erase(text.begin());
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                              text.back() == '\r' || text.back() == '\n'))
      text.pop_back();
    return text;
  }

  return {};
}

// ── Print Schema keyword mappers ──────────────────────────────────────

std::string map_duplex_option(std::string_view option)
{
  if (option == "psk:OneSided")               return "simplex";
  if (option == "psk:TwoSidedLongEdge")        return "long-edge (book)";
  if (option == "psk:TwoSidedShortEdge")       return "short-edge (calendar)";
  return {};
}

std::string map_paper_option(std::string_view option)
{
  if (option == "psk:ISOA0")    return "A0";
  if (option == "psk:ISOA1")    return "A1";
  if (option == "psk:ISOA2")    return "A2";
  if (option == "psk:ISOA3")    return "A3";
  if (option == "psk:ISOA4")    return "A4";
  if (option == "psk:ISOA5")    return "A5";
  if (option == "psk:ISOA6")    return "A6";
  if (option == "psk:NorthAmericaLetter")         return "Letter";
  if (option == "psk:NorthAmericaLegal")           return "Legal";
  if (option == "psk:NorthAmericaExecutive")       return "Executive";
  if (option == "psk:NorthAmericaTabloid")         return "Tabloid";
  if (option == "psk:NorthAmericaLedger")          return "Ledger";
  if (option == "psk:NorthAmericaStatement")       return "Statement";
  if (option == "psk:NorthAmerica10x14")           return "10x14";
  if (option == "psk:NorthAmerica11x17")           return "11x17";
  if (option == "psk:NorthAmericaQuarto")          return "Quarto";
  if (option == "psk:NorthAmericaFolio")           return "Folio";
  if (option == "psk:NorthAmericaNumber9Envelope") return "Envelope #9";
  if (option == "psk:NorthAmericaNumber10Envelope") return "Envelope #10";
  if (option == "psk:NorthAmericaNumber11Envelope") return "Envelope #11";
  if (option == "psk:NorthAmericaNumber12Envelope") return "Envelope #12";
  if (option == "psk:NorthAmericaNumber14Envelope") return "Envelope #14";
  if (option == "psk:NorthAmericaMonarchEnvelope")  return "Envelope Monarch";
  if (option == "psk:ISOB4")          return "B4 (ISO)";
  if (option == "psk:ISOB5Envelope")  return "B5 Envelope";
  if (option == "psk:ISOC3Envelope")  return "C3 Envelope";
  if (option == "psk:ISOC4Envelope")  return "C4 Envelope";
  if (option == "psk:ISOC5Envelope")  return "C5 Envelope";
  if (option == "psk:ISOC6Envelope")  return "C6 Envelope";
  if (option == "psk:ISODLEnvelope")  return "DL Envelope";
  if (option == "psk:JISB4")          return "B4 (JIS)";
  if (option == "psk:JISB5")          return "B5 (JIS)";
  if (option == "psk:PRC16K")         return "PRC 16K";
  if (option == "psk:PRC32K")         return "PRC 32K";
  if (option == "psk:PRC32KBig")      return "PRC 32K Big";
  if (option == "psk:PRCEnvelope1")   return "PRC Envelope #1";
  if (option == "psk:PRCEnvelope2")   return "PRC Envelope #2";
  if (option == "psk:JapanDoubleHagakiPostcard")      return "Japanese Double Postcard";
  if (option == "psk:JapanDoubleHagakiPostcardRotated") return "Double Postcard Rotated";
  if (option == "psk:JapanHagakiPostcard")             return "Japanese Postcard";
  if (option == "psk:JapanHagakiPostcardRotated")      return "Japanese Postcard Rotated";
  if (option == "psk:Photo4x6")      return "4x6 Photo";
  if (option == "psk:Photo5x7")      return "5x7 Photo";
  if (option == "psk:Photo8x10")     return "8x10 Photo";
  if (option == "psk:Roll04Inch")    return "4\" Roll";
  if (option == "psk:Roll06Inch")    return "6\" Roll";
  if (option == "psk:Roll08Inch")    return "8\" Roll";
  if (option.starts_with("psk:"))
    return std::string(option.substr(4));
  return std::string(option);
}

std::string map_orientation_option(std::string_view option)
{
  if (option == "psk:Portrait")            return "portrait";
  if (option == "psk:Landscape")           return "landscape";
  if (option == "psk:ReversePortrait")     return "reverse portrait";
  if (option == "psk:ReverseLandscape")    return "reverse landscape";
  return {};
}

std::string map_color_option(std::string_view option)
{
  if (option == "psk:Color")       return "color";
  if (option == "psk:Monochrome")  return "monochrome";
  if (option == "psk:Grayscale")   return "grayscale";
  return {};
}

std::string map_media_option(std::string_view option)
{
  if (option == "psk:Stationery")        return "plain";
  if (option == "psk:Transparency")      return "transparency";
  if (option == "psk:Photographic")      return "photo";
  if (option == "psk:PhotographicGlossy") return "glossy photo";
  if (option == "psk:PhotographicSemiGloss") return "semi-gloss";
  if (option == "psk:EnvelopePlain")     return "envelope";
  if (option == "psk:Screen")            return "screen";
  if (option == "psk:ScreenPaged")       return "paged screen";
  if (option == "psk:Labels")            return "labels";
  if (option == "psk:CardStock")         return "card stock";
  if (option == "psk:Bond")             return "bond";
  if (option == "psk:StationeryLetterhead") return "letterhead";
  if (option == "psk:StationeryPreprinted") return "preprinted";
  return {};
}

std::string map_collate_option(std::string_view option)
{
  if (option == "psk:Collated")    return "collated";
  if (option == "psk:Uncollated")  return "uncollated";
  return {};
}

int extract_option_count(std::string_view option_name)
{
  std::size_t colon = option_name.rfind(':');
  std::string_view last = (colon != std::string_view::npos)
    ? option_name.substr(colon + 1)
    : option_name;
  if (last.empty()) return -1;

  // Numeric suffix (e.g. "psk:2", "psk:4Up" → "2", "4").
  bool all_digits = true;
  for (char c : last) { if (c < '0' || c > '9') { all_digits = false; break; } }
  if (all_digits)
  {
    char* end = nullptr;
    long val = std::strtol(last.data(), &end, 10);
    if (end != last.data() + last.size()) return -1;
    return static_cast<int>(val);
  }

  // Word-number prefix (e.g. "psk:TwoUp", "psk:FourUp", "psk:SixUp").
  // Match case-insensitively against common PrintSchema N-up option names.
  auto icase_eq = [](std::string_view a, std::string_view b) noexcept {
    if (a.size() < b.size()) return false;
    for (std::size_t i = 0; i < b.size(); ++i)
    {
      char ca = static_cast<char>(a[i]);
      char cb = static_cast<char>(b[i]);
      if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
      if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
      if (ca != cb) return false;
    }
    return true;
  };

  if (icase_eq(last, "one"))        return 1;
  if (icase_eq(last, "two"))        return 2;
  if (icase_eq(last, "four"))       return 4;
  if (icase_eq(last, "six"))        return 6;
  if (icase_eq(last, "nine"))       return 9;
  if (icase_eq(last, "sixteen"))    return 16;
  if (icase_eq(last, "oneup"))      return 1;
  if (icase_eq(last, "twoup"))      return 2;
  if (icase_eq(last, "fourup"))     return 4;
  if (icase_eq(last, "sixup"))      return 6;
  if (icase_eq(last, "nineup"))     return 9;
  if (icase_eq(last, "sixteenup"))  return 16;

  return -1;
}

std::string map_page_order(std::string_view option)
{
  if (option == "psk:Standard")       return "standard";
  if (option == "psk:Reverse")       return "reverse";
  if (option == "psk:FrontToBack")   return "front to back";
  if (option == "psk:BackToFront")   return "back to front";
  return {};
}

/// Parse a resolution option string like "psk:600" or "psk:600x600dpi"
/// and return a human-readable string (e.g. "600 dpi" or "600x600 dpi").
std::string map_resolution_option(std::string_view option)
{
  // Strip psk: prefix if present.
  if (option.starts_with("psk:"))
    option.remove_prefix(4);

  if (option.empty()) return {};

  // Pattern: "600x600dpi" → "600x600 dpi"
  auto dpi_pos = option.find("dpi");
  if (dpi_pos != std::string_view::npos)
  {
    std::string result(option.substr(0, dpi_pos));
    result += " dpi";
    return result;
  }

  // Pattern: pure digits → "600 dpi"
  bool all_digits = true;
  for (char c : option) { if (c < '0' || c > '9') { all_digits = false; break; } }
  if (all_digits)
    return std::string(option) + " dpi";

  return std::string(option);
}

void set_if_present(core::FieldValue& field, std::string value)
{
  if (!value.empty())
    field = core::FieldValue{std::move(value), core::FieldState::Reported};
}

}  // namespace

// ── Public entry point ────────────────────────────────────────────────

void parse_print_ticket_xml(std::string_view pt_xml,
                            core::MetadataSnapshot& snapshot)
{
  if (pt_xml.empty()) return;

  // ── Job-level features ────────────────────────────────────────────

  // Copies: try Feature/Option first, then ParameterInit (the common case).
  {
    std::string opt = find_feature_option(
      pt_xml, "psk:JobCopiesAllDocuments");
    if (!opt.empty())
    {
      int n = extract_option_count(opt);
      if (n > 0)
        set_if_present(snapshot.copies, std::to_string(n));
    }
    else
    {
      // ParameterRef / ParameterInit — the pattern used by the
      // Windows print dialog for copies in nearly all cases.
      int n = find_parameter_init_int(pt_xml, "psk:JobCopiesAllDocuments");
      if (n > 0)
        set_if_present(snapshot.copies, std::to_string(n));
    }
  }

  // Duplex
  {
    std::string opt = find_feature_option(
      pt_xml, "psk:JobDuplexAllDocumentsContiguously");
    set_if_present(snapshot.duplex, map_duplex_option(opt));
  }

  // N-up
  {
    std::string opt = find_feature_option(
      pt_xml, "psk:JobNUpAllDocumentsContiguously");
    int n = extract_option_count(opt);
    if (n > 1)
      set_if_present(snapshot.n_up, std::to_string(n) + "-up");
    else if (n == 1)
      set_if_present(snapshot.n_up, "1-up (normal)");
  }

  // Collate (DocumentCollate takes priority).
  // JobPageOrder is a SEPARATE concept (reverse page order); it is
  // not stored in the collate field — we note it but don't overwrite.
  {
    std::string opt = find_feature_option(
      pt_xml, "psk:DocumentCollate");
    set_if_present(snapshot.collate, map_collate_option(opt));
  }

  // JobPageOrder — informational, not stored in the collate column.
  // (Previously it incorrectly overwrote the collate value.)
  {
    std::string opt = find_feature_option(
      pt_xml, "psk:JobPageOrder");
    (void)map_page_order(opt);  // reserved for future schema expansion
    (void)opt;
  }

  // JobInputBin — informational, no dedicated column.
  {
    std::string opt = find_feature_option(
      pt_xml, "psk:JobInputBin");
    (void)opt;  // reserved for future schema expansion
  }

  // ── Page-level features ────────────────────────────────────────────

  // Paper size
  {
    std::string opt = find_feature_option(
      pt_xml, "psk:PageMediaSize");
    set_if_present(snapshot.paper_size, map_paper_option(opt));
  }

  // Orientation
  {
    std::string opt = find_feature_option(
      pt_xml, "psk:PageOrientation");
    set_if_present(snapshot.orientation, map_orientation_option(opt));
  }

  // Color
  {
    std::string opt = find_feature_option(
      pt_xml, "psk:PageOutputColor");
    set_if_present(snapshot.color, map_color_option(opt));
  }

  // Media type
  {
    std::string opt = find_feature_option(
      pt_xml, "psk:PageMediaType");
    set_if_present(snapshot.media_type, map_media_option(opt));
  }

  // Print resolution — now handles "600x600dpi" format.
  {
    std::string opt = find_feature_option(
      pt_xml, "psk:PageResolution");
    if (!opt.empty())
    {
      std::string mapped = map_resolution_option(opt);
      if (!mapped.empty())
        set_if_present(snapshot.print_quality, std::move(mapped));
    }
  }

  // ── Page range (user's page selection in the print dialog) ────────
  // Captured for traceability — lets the operator verify which pages
  // were actually printed when reconciling cost against the document.
  // Two shapes:
  //   1. <*:Feature name="psk:PageRange">
  //        <*:Option name="psk:AllPages" />           → "all"
  //        <*:Option name="psk:CustomRange" />
  //      </*:Feature>
  //      <*:ParameterInit name="psk:PageRange">
  //        <*:Value xsi:type="xsd:string">1-3,5,7-9</*:Value>
  //      </*:ParameterInit>                            → "1-3,5,7-9"
  //   2. Some drivers omit the Feature and only emit ParameterInit.
  {
    std::string opt = find_feature_option(pt_xml, "psk:PageRange");
    if (!opt.empty())
    {
      if (opt == "psk:AllPages" || opt == "AllPages")
        set_if_present(snapshot.page_range, std::string("all"));
      else if (opt == "psk:CustomRange" || opt == "CustomRange")
      {
        std::string ranges = find_parameter_init_string(pt_xml, "psk:PageRange");
        if (!ranges.empty())
          set_if_present(snapshot.page_range, std::move(ranges));
        else
          set_if_present(snapshot.page_range, std::string("custom"));
      }
      else
        set_if_present(snapshot.page_range, std::move(opt));
    }
    else
    {
      // No Feature — try ParameterInit directly.
      std::string ranges = find_parameter_init_string(pt_xml, "psk:PageRange");
      if (!ranges.empty())
        set_if_present(snapshot.page_range, std::move(ranges));
    }
  }
}

}  // namespace wpce::parser
