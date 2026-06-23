/// @file billing_batch.h
/// BillingBatch entity + state machine for the time-windowed user
/// billing model.
///
/// A BillingBatch represents "all print jobs received during a time
/// window that belong to one customer".  The boundary is determined by
/// the operator clicking "结算" (settle): all jobs in the current
/// Active batch are frozen into a SettlementOrder, the batch is
/// sealed, and the next print job automatically opens a new Active
/// batch for the next customer.
///
/// State machine:
///
///     [new job, no Active]
///            │
///            ▼
///     ┌── Active ◄── resume() ──┐
///     │     │                    │
///     │  suspend()  settle()     │
///     │     │     │              │
///     │     ▼     ▼              │
///     │  Suspended  Settled      │
///     │     │                    │
///     │  (operator resume)       │
///     │     │                    │
///     └─────┘                    │
///        (continues receiving)
///
/// Hang-up (suspend) is for temporarily switching to print a new
/// user's jobs without settling the current one.  The current batch
/// becomes Suspended; new jobs open a new Active batch (the new user).
/// The operator can later resume the Suspended batch to continue
/// adding to it.
///
/// Anchors: first_doc_anchor and last_doc_anchor store the document
/// name + job_id + submit_time + page count of the first and last
/// jobs in the batch.  These are visual cues for the operator to
/// quickly identify which customer a historical batch belongs to.

#pragma once

#include "core/data_contracts.h"

#include <cstdint>
#include <string>
#include <vector>

namespace wpce::domain
{

enum class BatchState
{
  Active,     // receiving new jobs
  Suspended,  // temporarily parked by operator; not receiving jobs
  Settled,    // frozen + settlement order generated
  Voided,     // cancelled (kept for audit)
};

std::string_view to_string(BatchState state) noexcept;

struct BatchDocAnchor
{
  std::uint32_t job_id{0};
  std::string document_name;
  std::string submit_time_utc;
  int page_count{0};
};

struct BillingBatch
{
  std::string batch_id;            // "B-YYYYMMDD-NNNNNN"
  std::string display_name;        // operator-editable, default "用户#NN"
  std::string source_user;         // Windows pUserName (metadata only)
  BatchState state{BatchState::Active};
  std::string period_start_utc;    // first job submit time
  std::string period_end_utc;      // settle/suspend time (empty while active)
  std::string suspend_time_utc;    // last suspend time (for ordering)
  std::vector<std::uint32_t> job_ids;
  BatchDocAnchor first_doc_anchor;
  BatchDocAnchor last_doc_anchor;
  std::string settle_order_id;     // filled after settle()

  // Cached totals (recomputed by BillingService on job updates).
  int total_pages{0};
  int simplex_pages{0};
  int duplex_pages{0};
  int document_count{0};           // distinct document_name+ext count
  double total_price{0.0};
  int pending_count{0};

  [[nodiscard]] bool is_active() const noexcept { return state == BatchState::Active; }
  [[nodiscard]] bool is_suspended() const noexcept { return state == BatchState::Suspended; }
  [[nodiscard]] bool is_settled() const noexcept { return state == BatchState::Settled; }
};

/// Aggregated info about a single print job tracked by the BillingService.
/// Carries the latest snapshot, metadata, pricing, and lifecycle phase.
struct PrintJobRecord
{
  std::uint32_t job_id{0};
  std::uint32_t batch_job_seq{0};      // 1-based sequence within the batch
  core::JobSnapshot snapshot;
  core::MetadataSnapshot metadata;
  core::JobLifecyclePhase phase{core::JobLifecyclePhase::Unknown};
  core::JobPricingEntry pricing;
  std::string last_updated_utc;
};

}  // namespace wpce::domain
