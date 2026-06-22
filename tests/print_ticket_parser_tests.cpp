/// Unit tests for the PrintTicket XML parser.
///
/// Validates the reliability fixes:
///   - find_feature_option correctly handles self-closing Feature tags
///   - find_feature_option correctly handles nested self-closing Options
///   - Multiple features with the same name do not confuse the parser
///   - PageRange (ParameterInit string) is extracted for traceability
///   - N-up word-number option names (psk:TwoUp, psk:FourUp) are decoded
///   - Copies via ParameterInit is parsed
///   - Colour, duplex, paper, orientation from standard psk: features

#include "parser/print_ticket_parser.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace
{

/// A representative v4-driver PrintTicket with the patterns that
/// previously broke the parser:
///   - Self-closing <psf:Option ... /> inside Features
///   - Multiple <psf:Feature name="psk:PageMediaSize"> blocks
///     (job-level + page-level concatenated)
///   - ParameterInit for copies and page range
///   - Word-number N-up option name (psk:TwoUp)
constexpr std::string_view kSamplePrintTicket = R"(
<psf:PrintTicket xmlns:psf="http://schemas.microsoft.com/windows/2003/08/printing/printschemaframework"
                 xmlns:psk="http://schemas.microsoft.com/windows/2003/08/printing/printschemakeywords">
  <psf:Feature name="psk:JobCopiesAllDocuments">
    <psf:ParameterRef name="psk:JobCopiesAllDocuments" />
  </psf:Feature>
  <psf:ParameterInit name="psk:JobCopiesAllDocuments">
    <psf:Value xsi:type="xsd:integer">3</psf:Value>
  </psf:ParameterInit>

  <psf:Feature name="psk:JobDuplexAllDocumentsContiguously">
    <psf:Option name="psk:TwoSidedLongEdge" />
  </psf:Feature>

  <psf:Feature name="psk:JobNUpAllDocumentsContiguously">
    <psf:Option name="psk:TwoUp" />
  </psf:Feature>

  <psf:Feature name="psk:PageMediaSize">
    <psf:Option name="psk:ISOA4">
      <psf:ScoredProperty name="psk:MediaSizeWidth">
        <psf:Value xsi:type="xsd:integer">210000</psf:Value>
      </psf:ScoredProperty>
      <psf:ScoredProperty name="psk:MediaSizeHeight">
        <psf:Value xsi:type="xsd:integer">297000</psf:Value>
      </psf:ScoredProperty>
    </psf:Option>
  </psf:Feature>

  <psf:Feature name="psk:PageOrientation">
    <psf:Option name="psk:Portrait" />
  </psf:Feature>

  <psf:Feature name="psk:PageOutputColor">
    <psf:Option name="psk:Color" />
  </psf:Feature>

  <psf:Feature name="psk:PageRange">
    <psf:Option name="psk:CustomRange" />
  </psf:Feature>
  <psf:ParameterInit name="psk:PageRange">
    <psf:Value xsi:type="xsd:string">1-3,5,7-9</psf:Value>
  </psf:ParameterInit>

  <psf:Feature name="psk:PageMediaType">
    <psf:Option name="psk:Stationery" />
  </psf:Feature>

  <psf:Feature name="psk:PageResolution">
    <psf:Option name="psk:600x600dpi" />
  </psf:Feature>
</psf:PrintTicket>
)";

/// A second PageMediaSize Feature appearing later in the XML — the
/// parser must pick the FIRST occurrence (job-level) by default.
constexpr std::string_view kDuplicateFeatureTicket = R"(
<psf:PrintTicket xmlns:psf="..." xmlns:psk="...">
  <psf:Feature name="psk:PageMediaSize">
    <psf:Option name="psk:ISOA3" />
  </psf:Feature>
  <psf:Feature name="psk:PageMediaSize">
    <psf:Option name="psk:ISOA4" />
  </psf:Feature>
</psf:PrintTicket>
)";

/// A Feature with a self-closing Option that contains nested
/// ScoredProperty elements with their own self-closing tags — the
/// old parser's "/>" search would mistake these for the Feature end.
constexpr std::string_view kNestedSelfClosingTicket = R"(
<psf:PrintTicket xmlns:psf="..." xmlns:psk="...">
  <psf:Feature name="psk:PageMediaSize">
    <psf:Option name="psk:NorthAmericaLetter">
      <psf:ScoredProperty name="psk:MediaSizeWidth">
        <psf:Value xsi:type="xsd:integer">215900</psf:Value>
      </psf:ScoredProperty>
      <psf:ScoredProperty name="psk:MediaSizeHeight">
        <psf:Value xsi:type="xsd:integer">279400</psf:Value>
      </psf:ScoredProperty>
    </psf:Option>
  </psf:Feature>
  <psf:Feature name="psk:PageOutputColor">
    <psf:Option name="psk:Monochrome" />
  </psf:Feature>
</psf:PrintTicket>
)";

}  // namespace

TEST(PrintTicketParser, ExtractsCopiesFromParameterInit)
{
  wpce::core::MetadataSnapshot snapshot;
  wpce::parser::parse_print_ticket_xml(kSamplePrintTicket, snapshot);
  EXPECT_EQ(snapshot.copies.state, wpce::core::FieldState::Reported);
  EXPECT_EQ(snapshot.copies.value, "3");
}

TEST(PrintTicketParser, ExtractsDuplexLongEdge)
{
  wpce::core::MetadataSnapshot snapshot;
  wpce::parser::parse_print_ticket_xml(kSamplePrintTicket, snapshot);
  EXPECT_EQ(snapshot.duplex.state, wpce::core::FieldState::Reported);
  EXPECT_EQ(snapshot.duplex.value, "long-edge (book)");
}

TEST(PrintTicketParser, ExtractsColor)
{
  wpce::core::MetadataSnapshot snapshot;
  wpce::parser::parse_print_ticket_xml(kSamplePrintTicket, snapshot);
  EXPECT_EQ(snapshot.color.state, wpce::core::FieldState::Reported);
  EXPECT_EQ(snapshot.color.value, "color");
}

TEST(PrintTicketParser, ExtractsPaperA4)
{
  wpce::core::MetadataSnapshot snapshot;
  wpce::parser::parse_print_ticket_xml(kSamplePrintTicket, snapshot);
  EXPECT_EQ(snapshot.paper_size.state, wpce::core::FieldState::Reported);
  EXPECT_EQ(snapshot.paper_size.value, "A4");
}

TEST(PrintTicketParser, ExtractsOrientationPortrait)
{
  wpce::core::MetadataSnapshot snapshot;
  wpce::parser::parse_print_ticket_xml(kSamplePrintTicket, snapshot);
  EXPECT_EQ(snapshot.orientation.state, wpce::core::FieldState::Reported);
  EXPECT_EQ(snapshot.orientation.value, "portrait");
}

TEST(PrintTicketParser, ExtractsPageRangeCustomRange)
{
  wpce::core::MetadataSnapshot snapshot;
  wpce::parser::parse_print_ticket_xml(kSamplePrintTicket, snapshot);
  EXPECT_EQ(snapshot.page_range.state, wpce::core::FieldState::Reported);
  EXPECT_EQ(snapshot.page_range.value, "1-3,5,7-9");
}

TEST(PrintTicketParser, ExtractsMediaTypeStationery)
{
  wpce::core::MetadataSnapshot snapshot;
  wpce::parser::parse_print_ticket_xml(kSamplePrintTicket, snapshot);
  EXPECT_EQ(snapshot.media_type.state, wpce::core::FieldState::Reported);
  EXPECT_EQ(snapshot.media_type.value, "plain");
}

TEST(PrintTicketParser, ExtractsResolutionWithDpiSuffix)
{
  wpce::core::MetadataSnapshot snapshot;
  wpce::parser::parse_print_ticket_xml(kSamplePrintTicket, snapshot);
  EXPECT_EQ(snapshot.print_quality.state, wpce::core::FieldState::Reported);
  EXPECT_EQ(snapshot.print_quality.value, "600x600 dpi");
}

TEST(PrintTicketParser, DecodesWordNumberNUpOption)
{
  wpce::core::MetadataSnapshot snapshot;
  wpce::parser::parse_print_ticket_xml(kSamplePrintTicket, snapshot);
  EXPECT_EQ(snapshot.n_up.state, wpce::core::FieldState::Reported);
  EXPECT_EQ(snapshot.n_up.value, "2-up");
}

TEST(PrintTicketParser, PicksFirstFeatureWhenDuplicated)
{
  wpce::core::MetadataSnapshot snapshot;
  wpce::parser::parse_print_ticket_xml(kDuplicateFeatureTicket, snapshot);
  EXPECT_EQ(snapshot.paper_size.state, wpce::core::FieldState::Reported);
  EXPECT_EQ(snapshot.paper_size.value, "A3");
}

TEST(PrintTicketParser, HandlesNestedSelfClosingScoredProperties)
{
  wpce::core::MetadataSnapshot snapshot;
  wpce::parser::parse_print_ticket_xml(kNestedSelfClosingTicket, snapshot);
  EXPECT_EQ(snapshot.paper_size.state, wpce::core::FieldState::Reported);
  EXPECT_EQ(snapshot.paper_size.value, "Letter");
  EXPECT_EQ(snapshot.color.state, wpce::core::FieldState::Reported);
  EXPECT_EQ(snapshot.color.value, "monochrome");
}

TEST(PrintTicketParser, AllPagesRangeReturnsAll)
{
  constexpr std::string_view xml = R"(
<psf:PrintTicket xmlns:psf="..." xmlns:psk="...">
  <psf:Feature name="psk:PageRange">
    <psf:Option name="psk:AllPages" />
  </psf:Feature>
</psf:PrintTicket>
)";
  wpce::core::MetadataSnapshot snapshot;
  wpce::parser::parse_print_ticket_xml(xml, snapshot);
  EXPECT_EQ(snapshot.page_range.state, wpce::core::FieldState::Reported);
  EXPECT_EQ(snapshot.page_range.value, "all");
}

TEST(PrintTicketParser, EmptyXmlLeavesFieldsNotReported)
{
  wpce::core::MetadataSnapshot snapshot;
  wpce::parser::parse_print_ticket_xml(std::string_view{}, snapshot);
  EXPECT_NE(snapshot.copies.state, wpce::core::FieldState::Reported);
  EXPECT_NE(snapshot.duplex.state, wpce::core::FieldState::Reported);
  EXPECT_NE(snapshot.color.state, wpce::core::FieldState::Reported);
}
