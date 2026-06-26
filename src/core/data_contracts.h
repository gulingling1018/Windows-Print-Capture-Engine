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
  std::uint32_t raw_status_flags{0};
  std::string source{"winspool"};
};

struct MetadataSnapshot
{
  std::int32_t schema_version{1};
  std::uint32_t job_id{0};
  std::string printer_name;
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
  FieldValue media_type;      // Media (plain, glossy, transparency, etc.)

  /// Page range selected by the user in the print dialog (e.g. "1-3,5,7-9").
  /// Empty when the whole document is printed or the driver does not expose it.
  /// Captured from the PrintTicket's psk:PageRange ParameterInit.
  FieldValue page_range;

  /// File extension extracted from the document name (e.g. "pdf", "dwg", "docx").
  /// Lowercased, without the leading dot.  Used for per-filetype pricing rules.
  FieldValue file_extension;

  /// Hex dump of dmFields bitmask for diagnostics.
  /// e.g. "0x0007635f" allows checking which fields the driver reports.
  /// Set to the literal "PrintTicket" when this snapshot's authoritative
  /// source is the XPS PrintTicket XML (v4 drivers) rather than DEVMODE.
  std::string dm_fields_hex;
};

struct PageArtifactIndex
{
  std::int32_t schema_version{1};
  std::uint32_t job_id{0};
  std::string printer_name;
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

  /// Milliseconds since the previous lifecycle event (0 for first event).
  std::int64_t delta_ms_since_prev{0};

  /// Cumulative milliseconds spent in the current phase so far.
  std::int64_t cumulative_phase_ms{0};
};

/// Dwell time for a completed lifecycle phase.
struct PhaseDwellEntry
{
  std::string phase_name;    // e.g. "spooling", "printing"
  std::int64_t duration_ms;  // -1 for terminal (not a duration)
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

  /// Accumulated dwell time per phase.
  std::vector<PhaseDwellEntry> phase_dwells;

  /// Total observed lifecycle wall-clock duration in milliseconds.
  std::int64_t total_duration_ms{0};
};

// ── Pricing & settlement data structures ─────────────────────────────────

/// A single pricing rule in the matrix-based pricing system.
/// Empty string fields mean "match all" — the rule applies regardless of
/// that dimension.  Priority (specificity score) is computed automatically.
///
/// Multi-value dimensions: `paper_size` and `file_extension` may contain
/// a pipe-separated string like "A4|B5" or "pdf|dwg" in the config file.
/// At load time these are split into `paper_size_set` / `file_extension_set`
/// for proper multi-value matching.  The single-string fields are kept
/// for backward-compat serialisation; the set fields are authoritative
/// for matching when non-empty.
struct PricingRule
{
  std::string rule_id;           // Unique identifier, e.g. "rule-001"
  std::string paper_size;        // Empty = match all; may be "A4|B5"
  std::string duplex_mode;       // "simplex" | "duplex" | "" = all
  std::string printer_name;      // Printer name, or "" = match all printers; may be pipe-separated
  std::string color_mode;        // "color" | "monochrome" | "" = all
  std::string file_extension;    // Empty = match all; may be "pdf|dwg"
  double price_per_sheet{0.0};         // Price per physical sheet
  int priority{0};                     // Specificity: count of non-empty dimensions

  /// Multi-value expansions of paper_size (lowercased, no surrounding pipes).
  /// Empty means "match all paper sizes".
  std::vector<std::string> paper_size_set;

  /// Multi-value expansions of printer_name (case-insensitive matching).
  /// Empty means "match all printers".
  std::vector<std::string> printer_name_set;

  /// Multi-value expansions of file_extension (lowercased).
  /// Empty means "match all file types".
  std::vector<std::string> file_extension_set;
};

/// Breakdown of physical sheet consumption for a print job,
/// correctly handling the odd-page tail in duplex printing.
struct SheetBreakdown
{
  int full_duplex_sheets{0};   // Complete duplex sheets (2 pages each)
  int simplex_tail_pages{0};   // Leftover simplex pages (0 or 1 for duplex)
  int total_sheets{0};         // total = full_duplex_sheets + simplex_tail_pages
};

/// Per-job pricing result produced by the pricing engine.
/// Carries enough detail for the frontend to display a transparent
/// price breakdown and flag items that require manual operator input.
struct JobPricingEntry
{
  std::uint32_t job_id{0};
  int total_pages{0};
  int copies{1};
  int effective_sheets{0};       // Total physical sheets consumed
  int duplex_full_sheets{0};     // Complete duplex sheets
  int simplex_tail_pages{0};     // Odd-page tail (simplex)
  std::string matched_rule_id;      // ID of the rule that matched, or empty
  std::string tail_rule_id;         // ID of the rule used for simplex tail, or empty
  std::string matched_printer_name; // Printer name from the matched rule, or empty if wildcard
  double base_price{0.0};        // Full duplex sheets price
  double tail_price{0.0};        // Simplex tail pages price
  double total_price{0.0};
  int color_pages{0};           // Operator-filled: count of color pages (0 = not filled)
  int mono_pages{0};            // B&W pages = total_pages - color_pages
  double color_subtotal{0.0};   // Color portion subtotal
  double mono_subtotal{0.0};    // B&W portion subtotal
  bool price_pending{false};     // Requires operator to manually enter a price
  std::string pending_reason;    // "awaiting_color_pages" | "unmatched_rule" | "no_simplex_rule_for_tail" | ...
  bool color_downgraded{false};  // True if color job on B&W printer → charged as monochrome
  std::string note;              // Human-readable note (e.g. "color_on_bw_printer")
};

/// A frozen settlement order that captures all pricing decisions at the
/// moment of settlement.  This is the immutable audit record.
struct SettlementOrder
{
  std::int32_t schema_version{2};
  std::string order_id;             // e.g. "SETTLE-20260622-000001"
  std::string batch_id;             // Associated BillingBatch.batch_id (v2+)
  std::string task_id;              // Legacy field (kept for v1 backward compat)
  std::string user_id;              // Batch display_name (v2+) or legacy user
  std::string settled_by;           // Operator name (filled by frontend)
  std::string settled_at_utc;       // ISO8601 timestamp of settlement
  std::string period_start_utc;     // Time window start
  std::string period_end_utc;       // Time window end
  std::vector<std::uint32_t> job_ids;      // Print jobs in this order
  std::vector<JobPricingEntry> pricing_entries;  // Per-job cost breakdown
  std::string pricing_snapshot_json; // Frozen PricingConfig at settlement time
  double total_price{0.0};
  double color_merged_total{0.0};   // >0 when operator entered a single merged color price
  std::string currency{"CNY"};
};

}  // namespace wpce::core
