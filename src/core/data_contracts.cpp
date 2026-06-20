#include "core/data_contracts.h"

namespace wpce::core
{

std::string_view to_string(const FieldState state) noexcept
{
  switch (state)
  {
    case FieldState::Reported:
      return "reported";
    case FieldState::Unsupported:
      return "unsupported";
    case FieldState::NotReported:
      return "not_reported";
    case FieldState::Unknown:
      return "unknown";
    case FieldState::Error:
      return "error";
  }

  return "unknown";
}

std::string_view to_string(const JobLifecyclePhase phase) noexcept
{
  switch (phase)
  {
    case JobLifecyclePhase::Unknown:
      return "unknown";
    case JobLifecyclePhase::Accepted:
      return "accepted";
    case JobLifecyclePhase::Spooling:
      return "spooling";
    case JobLifecyclePhase::Printing:
      return "printing";
    case JobLifecyclePhase::Completed:
      return "completed";
    case JobLifecyclePhase::Failed:
      return "failed";
    case JobLifecyclePhase::Cancelled:
      return "cancelled";
    case JobLifecyclePhase::Stalled:
      return "stalled";
  }

  return "unknown";
}

bool is_terminal_phase(const JobLifecyclePhase phase) noexcept
{
  switch (phase)
  {
    case JobLifecyclePhase::Completed:
    case JobLifecyclePhase::Failed:
    case JobLifecyclePhase::Cancelled:
      return true;
    default:
      return false;
  }
}

}  // namespace wpce::core

