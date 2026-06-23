/// @file billing_batch.cpp
/// BillingBatch entity helpers.

#include "domain/billing_batch.h"

namespace wpce::domain
{

std::string_view to_string(BatchState state) noexcept
{
  switch (state)
  {
    case BatchState::Active:    return "active";
    case BatchState::Suspended: return "suspended";
    case BatchState::Settled:   return "settled";
    case BatchState::Voided:    return "voided";
  }
  return "unknown";
}

}  // namespace wpce::domain
