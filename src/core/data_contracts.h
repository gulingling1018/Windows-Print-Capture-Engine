#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wpce::core
{

enum class FieldState
{
  Reported,
  Unsupported,
  NotReported,
  Unknown,
  Error
};

std::string_view to_string(FieldState state) noexcept;

struct FieldValue
{
  std::string value;
  FieldState state{FieldState::Unknown};
};

struct JobSnapshot
{
  std::int32_t schema_version{1};
  std::uint32_t job_id{0};
  std::string user;
  std::string document_name;
  std::string printer_name;
  std::string submit_time_utc;
  std::string status;
  std::string source{"winspool"};
};

struct MetadataSnapshot
{
  std::int32_t schema_version{1};
  std::uint32_t job_id{0};
  FieldValue total_pages;
  FieldValue pages_printed;
  FieldValue copies;
  FieldValue duplex;
  FieldValue color;
  FieldValue paper_size;
  FieldValue orientation;
  FieldValue n_up;            // Pages-per-sheet (1=normal, 2/4/6/9/16=N-up)
  FieldValue collate;         // Whether copies are collated
  FieldValue print_quality;   // DPI resolution category

  /// Hex dump of dmFields bitmask for diagnostics.
  /// e.g. "0x0007635f" allows checking which fields the driver reports.
  std::string dm_fields_hex;
};

struct PageArtifactIndex
{
  std::int32_t schema_version{1};
  std::uint32_t job_id{0};
  std::int32_t document_sequence{0};
  std::int32_t page_sequence{0};
  std::string job_print_ticket;
  std::string document_print_ticket;
  std::string page_print_ticket;
  std::string fixed_page;
  std::string source{"xps-filter"};
};

// ── Job lifecycle events ───────────────────────────────────────────────

/// High-level job lifecycle phase derived from raw JOB_STATUS flags.
enum class JobLifecyclePhase
{
  Unknown,
  Accepted,    // Job submitted to spooler
  Spooling,    // Data being sent to spooler
  Printing,    // Data being sent to printer
  Completed,   // printed | complete | retained
  Failed,      // error | paperout | offline | user_intervention
  Cancelled,   // deleting | deleted (user or system cancelled)
  Stalled,     // No progress within timeout window
};

std::string_view to_string(JobLifecyclePhase phase) noexcept;

/// Whether this phase represents a terminal (end-of-life) state.
bool is_terminal_phase(JobLifecyclePhase phase) noexcept;

/// A single timestamped event in a print job's lifecycle.
struct JobLifecycleEvent
{
  std::string event_id;               // "{job_id}-{sequence}"
  std::uint32_t job_id{0};
  std::int32_t sequence{0};           // Monotonic per-job event index
  std::string timestamp_utc;
  JobLifecyclePhase phase{JobLifecyclePhase::Unknown};
  std::string status_flags;           // Raw JOB_STATUS at event time
  std::string detail;                 // Human-readable description
  std::int32_t pages_printed{0};
  std::int32_t total_pages{0};
};

/// Full lifecycle summary for one print job.
struct JobLifecycleSummary
{
  std::int32_t schema_version{1};
  std::uint32_t job_id{0};
  std::string user;
  std::string document_name;
  std::string printer_name;
  std::string submit_time_utc;
  std::vector<JobLifecycleEvent> events;
  JobLifecyclePhase final_phase{JobLifecyclePhase::Unknown};
  std::int32_t progress_percent{0};   // pages_printed * 100 / total_pages, or -1
  std::string last_status;
  std::string source{"winspool"};
};

}  // namespace wpce::core

