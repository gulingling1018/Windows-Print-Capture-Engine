#include "storage/json_serializer.h"

#include <iomanip>
#include <ostream>
#include <sstream>

namespace wpce::storage
{
namespace
{

void append_json_string(std::ostream& output, const std::string_view value)
{
  output << '"';
  for (const unsigned char character : value)
  {
    switch (character)
    {
      case '"':
        output << "\\\"";
        break;
      case '\\':
        output << "\\\\";
        break;
      case '\b':
        output << "\\b";
        break;
      case '\f':
        output << "\\f";
        break;
      case '\n':
        output << "\\n";
        break;
      case '\r':
        output << "\\r";
        break;
      case '\t':
        output << "\\t";
        break;
      default:
        if (character < 0x20)
        {
          output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(character) << std::dec;
        }
        else
        {
          output << character;
        }
        break;
    }
  }
  output << '"';
}

void append_json_property(std::ostream& output, const std::string_view name, const std::string_view value)
{
  append_json_string(output, name);
  output << ": ";
  append_json_string(output, value);
}

void append_json_property(std::ostream& output, const std::string_view name, const std::int32_t value)
{
  append_json_string(output, name);
  output << ": " << value;
}

void append_json_property(std::ostream& output, const std::string_view name, const std::int64_t value)
{
  append_json_string(output, name);
  output << ": " << value;
}

void append_json_property(std::ostream& output, const std::string_view name, const std::uint32_t value)
{
  append_json_string(output, name);
  output << ": " << value;
}

void append_field_value(std::ostream& output, const std::string_view name, const core::FieldValue& field)
{
  append_json_string(output, name);
  output << ": { ";
  append_json_property(output, "value", field.value);
  output << ", ";
  append_json_property(output, "state", core::to_string(field.state));
  output << " }";
}

}  // namespace

std::string json_escape(const std::string_view value)
{
  std::ostringstream output;
  append_json_string(output, value);
  return output.str();
}

std::string to_json(const core::JobSnapshot& snapshot)
{
  std::ostringstream output;
  output << "{\n  ";
  append_json_property(output, "schema_version", snapshot.schema_version);
  output << ",\n  ";
  append_json_property(output, "job_id", snapshot.job_id);
  output << ",\n  ";
  append_json_property(output, "user", snapshot.user);
  output << ",\n  ";
  append_json_property(output, "document_name", snapshot.document_name);
  output << ",\n  ";
  append_json_property(output, "printer_name", snapshot.printer_name);
  output << ",\n  ";
  append_json_property(output, "submit_time_utc", snapshot.submit_time_utc);
  output << ",\n  ";
  append_json_property(output, "status", snapshot.status);
  output << ",\n  ";
  append_json_property(output, "source", snapshot.source);
  output << "\n}\n";
  return output.str();
}

std::string to_json(const core::MetadataSnapshot& snapshot)
{
  std::ostringstream output;
  output << "{\n  ";
  append_json_property(output, "schema_version", snapshot.schema_version);
  output << ",\n  ";
  append_json_property(output, "job_id", snapshot.job_id);
  output << ",\n  ";
  append_json_property(output, "printer_name", snapshot.printer_name);
  output << ",\n  ";
  append_field_value(output, "total_pages", snapshot.total_pages);
  output << ",\n  ";
  append_field_value(output, "pages_printed", snapshot.pages_printed);
  output << ",\n  ";
  append_field_value(output, "copies", snapshot.copies);
  output << ",\n  ";
  append_field_value(output, "duplex", snapshot.duplex);
  output << ",\n  ";
  append_field_value(output, "color", snapshot.color);
  output << ",\n  ";
  append_field_value(output, "paper_size", snapshot.paper_size);
  output << ",\n  ";
  append_field_value(output, "orientation", snapshot.orientation);
  output << ",\n  ";
  append_field_value(output, "n_up", snapshot.n_up);
  output << ",\n  ";
  append_field_value(output, "collate", snapshot.collate);
  output << ",\n  ";
  append_field_value(output, "print_quality", snapshot.print_quality);
  output << ",\n  ";
  append_field_value(output, "media_type", snapshot.media_type);
  output << ",\n  ";
  append_field_value(output, "page_range", snapshot.page_range);
  output << ",\n  ";
  append_json_property(output, "dm_fields_hex", snapshot.dm_fields_hex);
  output << "\n}\n";
  return output.str();
}

std::string to_json(const core::JobLifecycleEvent& event)
{
  std::ostringstream output;
  output << "{\n  ";
  append_json_property(output, "event_id", event.event_id);
  output << ",\n  ";
  append_json_property(output, "job_id", event.job_id);
  output << ",\n  ";
  append_json_property(output, "sequence", event.sequence);
  output << ",\n  ";
  append_json_property(output, "timestamp_utc", event.timestamp_utc);
  output << ",\n  ";
  append_json_property(output, "phase", core::to_string(event.phase));
  output << ",\n  ";
  append_json_property(output, "status_flags", event.status_flags);
  output << ",\n  ";
  append_json_property(output, "pages_printed", event.pages_printed);
  output << ",\n  ";
  append_json_property(output, "total_pages", event.total_pages);
  output << ",\n  ";
  append_json_property(output, "delta_ms_since_prev", event.delta_ms_since_prev);
  output << ",\n  ";
  append_json_property(output, "cumulative_phase_ms", event.cumulative_phase_ms);
  output << ",\n  ";
  append_json_property(output, "detail", event.detail);
  output << "\n}\n";
  return output.str();
}

void append_json_lifecycle_event(std::ostream& output, const core::JobLifecycleEvent& event)
{
  output << "    {\n      ";
  append_json_property(output, "event_id", event.event_id);
  output << ",\n      ";
  append_json_property(output, "job_id", event.job_id);
  output << ",\n      ";
  append_json_property(output, "sequence", event.sequence);
  output << ",\n      ";
  append_json_property(output, "timestamp_utc", event.timestamp_utc);
  output << ",\n      ";
  append_json_property(output, "phase", core::to_string(event.phase));
  output << ",\n      ";
  append_json_property(output, "status_flags", event.status_flags);
  output << ",\n      ";
  append_json_property(output, "pages_printed", event.pages_printed);
  output << ",\n      ";
  append_json_property(output, "total_pages", event.total_pages);
  output << ",\n      ";
  append_json_property(output, "delta_ms_since_prev", event.delta_ms_since_prev);
  output << ",\n      ";
  append_json_property(output, "cumulative_phase_ms", event.cumulative_phase_ms);
  output << ",\n      ";
  append_json_property(output, "detail", event.detail);
  output << "\n    }";
}

std::string to_json(const core::JobLifecycleSummary& summary)
{
  std::ostringstream output;
  output << "{\n  ";
  append_json_property(output, "schema_version", summary.schema_version);
  output << ",\n  ";
  append_json_property(output, "job_id", summary.job_id);
  output << ",\n  ";
  append_json_property(output, "user", summary.user);
  output << ",\n  ";
  append_json_property(output, "document_name", summary.document_name);
  output << ",\n  ";
  append_json_property(output, "printer_name", summary.printer_name);
  output << ",\n  ";
  append_json_property(output, "submit_time_utc", summary.submit_time_utc);
  output << ",\n  ";
  append_json_property(output, "final_phase", core::to_string(summary.final_phase));
  output << ",\n  ";
  append_json_property(output, "progress_percent", summary.progress_percent);
  output << ",\n  ";
  append_json_property(output, "source", summary.source);
  output << ",\n  ";
  append_json_property(output, "total_duration_ms", summary.total_duration_ms);
  output << ",\n  ";
  // Phase dwells array.
  output << "\"phase_dwells\": [\n";
  for (std::size_t i = 0; i < summary.phase_dwells.size(); ++i)
  {
    output << "    {\n      ";
    append_json_property(output, "phase", summary.phase_dwells[i].phase_name);
    output << ",\n      ";
    append_json_property(output, "duration_ms", summary.phase_dwells[i].duration_ms);
    output << "\n    }";
    if (i + 1 < summary.phase_dwells.size())
    {
      output << ',';
    }
    output << '\n';
  }
  output << "  ],\n  ";
  output << "\"events\": [\n";
  for (std::size_t i = 0; i < summary.events.size(); ++i)
  {
    append_json_lifecycle_event(output, summary.events[i]);
    if (i + 1 < summary.events.size())
    {
      output << ',';
    }
    output << '\n';
  }
  output << "  ]\n";
  output << "}\n";
  return output.str();
}

std::string to_json(const core::PageArtifactIndex& index)
{
  std::ostringstream output;
  output << "{\n  ";
  append_json_property(output, "schema_version", index.schema_version);
  output << ",\n  ";
  append_json_property(output, "job_id", index.job_id);
  output << ",\n  ";
  append_json_property(output, "printer_name", index.printer_name);
  output << ",\n  ";
  append_json_property(output, "document_sequence", index.document_sequence);
  output << ",\n  ";
  append_json_property(output, "page_sequence", index.page_sequence);
  output << ",\n  ";
  append_json_property(output, "job_print_ticket", index.job_print_ticket);
  output << ",\n  ";
  append_json_property(output, "document_print_ticket", index.document_print_ticket);
  output << ",\n  ";
  append_json_property(output, "page_print_ticket", index.page_print_ticket);
  output << ",\n  ";
  append_json_property(output, "fixed_page", index.fixed_page);
  output << ",\n  ";
  append_json_property(output, "source", index.source);
  output << "\n}\n";
  return output.str();
}

}  // namespace wpce::storage
