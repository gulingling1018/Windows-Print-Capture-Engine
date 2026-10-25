#include "storage/xml_serializer.h"

#include <ostream>
#include <sstream>

namespace wpce::storage
{
namespace
{

void append_xml_text(std::ostream& output, const std::string_view value)
{
  for (const char character : value)
  {
    switch (character)
    {
      case '&':
        output << "&amp;";
        break;
      case '<':
        output << "&lt;";
        break;
      case '>':
        output << "&gt;";
        break;
      case '"':
        output << "&quot;";
        break;
      case '\'':
        output << "&apos;";
        break;
      default:
        output << character;
        break;
    }
  }
}

void append_xml_element(std::ostream& output, const std::string_view name, const std::string_view value)
{
  output << "  <" << name << ">";
  append_xml_text(output, value);
  output << "</" << name << ">\n";
}

void append_xml_element(std::ostream& output, const std::string_view name, const std::int32_t value)
{
  output << "  <" << name << ">" << value << "</" << name << ">\n";
}

void append_xml_element(std::ostream& output, const std::string_view name, const std::uint32_t value)
{
  output << "  <" << name << ">" << value << "</" << name << ">\n";
}

void append_field_value(std::ostream& output, const std::string_view name, const core::FieldValue& field)
{
  output << "  <" << name << " state=\"" << core::to_string(field.state) << "\">";
  append_xml_text(output, field.value);
  output << "</" << name << ">\n";
}

}  // namespace

std::string xml_escape(const std::string_view value)
{
  std::ostringstream output;
  append_xml_text(output, value);
  return output.str();
}

std::string to_xml(const core::JobSnapshot& snapshot)
{
  std::ostringstream output;
  output << "<job_snapshot>\n";
  append_xml_element(output, "schema_version", snapshot.schema_version);
  append_xml_element(output, "job_id", snapshot.job_id);
  append_xml_element(output, "user", snapshot.user);
  append_xml_element(output, "document_name", snapshot.document_name);
  append_xml_element(output, "printer_name", snapshot.printer_name);
  append_xml_element(output, "submit_time_utc", snapshot.submit_time_utc);
  append_xml_element(output, "status", snapshot.status);
  append_xml_element(output, "raw_status_flags", snapshot.raw_status_flags);
  append_xml_element(output, "source", snapshot.source);
  output << "</job_snapshot>\n";
  return output.str();
}

std::string to_xml(const core::MetadataSnapshot& snapshot)
{
  std::ostringstream output;
  output << "<metadata_snapshot>\n";
  append_xml_element(output, "schema_version", snapshot.schema_version);
  append_xml_element(output, "job_id", snapshot.job_id);
  append_xml_element(output, "printer_name", snapshot.printer_name);
  append_field_value(output, "total_pages", snapshot.total_pages);
  append_field_value(output, "pages_printed", snapshot.pages_printed);
  append_field_value(output, "copies", snapshot.copies);
  append_field_value(output, "duplex", snapshot.duplex);
  append_field_value(output, "color", snapshot.color);
  append_field_value(output, "paper_size", snapshot.paper_size);
  append_field_value(output, "orientation", snapshot.orientation);
  append_field_value(output, "n_up", snapshot.n_up);
  append_field_value(output, "collate", snapshot.collate);
  append_field_value(output, "print_quality", snapshot.print_quality);
  append_field_value(output, "media_type", snapshot.media_type);
  append_field_value(output, "page_range", snapshot.page_range);
  append_field_value(output, "file_extension", snapshot.file_extension);
  append_xml_element(output, "dm_fields_hex", snapshot.dm_fields_hex);
  output << "</metadata_snapshot>\n";
  return output.str();
}

std::string to_xml(const core::PageArtifactIndex& index)
{
  std::ostringstream output;
  output << "<page_artifact_index>\n";
  append_xml_element(output, "schema_version", index.schema_version);
  append_xml_element(output, "job_id", index.job_id);
  append_xml_element(output, "printer_name", index.printer_name);
  append_xml_element(output, "document_sequence", index.document_sequence);
  append_xml_element(output, "page_sequence", index.page_sequence);
  append_xml_element(output, "job_print_ticket", index.job_print_ticket);
  append_xml_element(output, "document_print_ticket", index.document_print_ticket);
  append_xml_element(output, "page_print_ticket", index.page_print_ticket);
  append_xml_element(output, "fixed_page", index.fixed_page);
  append_xml_element(output, "source", index.source);
  append_xml_element(output, "capture_id", index.capture_id);
  append_xml_element(output, "fixed_page_state", index.fixed_page_state);
  output << "</page_artifact_index>\n";
  return output.str();
}

}  // namespace wpce::storage
