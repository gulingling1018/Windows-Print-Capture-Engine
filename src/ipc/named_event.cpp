/// @file named_event.cpp
/// Named event RAII wrapper implementation.

#include "ipc/named_event.h"

#include <utility>

namespace wpce::ipc
{

NamedEvent::NamedEvent(std::wstring_view name, EventRole role)
{
  if (role == EventRole::Creator)
  {
    // ── Creator: CreateEventW with NULL DACL ──────────────────────────
    // The spooler service runs in session 0; the UI runs in the user's
    // interactive session.  A default DACL would block cross-session
    // OpenEventW.  A NULL DACL grants full access to everyone — safe
    // for a local-machine signalling primitive.
    SECURITY_DESCRIPTOR sd{};
    ::InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    ::SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);  // NULL DACL

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;

    // Manual-reset (bManualReset=TRUE), initially non-signaled.
    handle_ = ::CreateEventW(&sa, TRUE, FALSE, name.data());

    // CreateEventW returns the existing handle with ERROR_ALREADY_EXISTS
    // if a previous filter invocation already created it — that's fine.
  }
  else
  {
    // ── Opener: OpenEventW ───────────────────────────────────────────
    // SYNCHRONIZE for WaitForSingleObject; EVENT_MODIFY_STATE for ResetEvent.
    handle_ = ::OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name.data());
  }
}

NamedEvent::~NamedEvent()
{
  if (handle_ != nullptr)
  {
    ::CloseHandle(handle_);
  }
}

NamedEvent::NamedEvent(NamedEvent&& other) noexcept
  : handle_(std::exchange(other.handle_, nullptr))
{
}

NamedEvent& NamedEvent::operator=(NamedEvent&& other) noexcept
{
  if (this != &other)
  {
    if (handle_ != nullptr)
    {
      ::CloseHandle(handle_);
    }
    handle_ = std::exchange(other.handle_, nullptr);
  }
  return *this;
}

void NamedEvent::signal() noexcept
{
  if (handle_ != nullptr)
  {
    ::SetEvent(handle_);
  }
}

void NamedEvent::reset() noexcept
{
  if (handle_ != nullptr)
  {
    ::ResetEvent(handle_);
  }
}

DWORD NamedEvent::wait(DWORD timeout_ms) const noexcept
{
  if (handle_ == nullptr)
  {
    return WAIT_FAILED;
  }
  return ::WaitForSingleObject(handle_, timeout_ms);
}

}  // namespace wpce::ipc
