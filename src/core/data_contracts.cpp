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
      return "Unknown";
    case JobLifecyclePhase::Accepted:
      return "Accepted";
    case JobLifecyclePhase::Spooling:
      return "Spooling";
    case JobLifecyclePhase::Printing:
      return "Printing";
    case JobLifecyclePhase::Completed:
      return "Completed";
    case JobLifecyclePhase::Failed:
      return "Failed";
    case JobLifecyclePhase::Cancelled:
      return "Cancelled";
    case JobLifecyclePhase::Stalled:
      return "Stalled";
  }

  return "Unknown";
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

