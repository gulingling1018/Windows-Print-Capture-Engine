/// @file named_event.h
/// Minimal RAII wrapper around a Windows named event for cross-process
/// signalling between the XPS filter DLL (spooler, session 0) and the
/// WPCE UI process (user session).
///
/// Uses manual-reset events so that signals are not lost when the
/// consumer is not actively waiting - the event stays set until the
/// consumer explicitly resets it after scanning for new metadata.

#pragma once

#ifdef _WIN32
#include <windows.h>
#endif

#include <string>
#include <string_view>

namespace wpce::ipc
{

/// Well-known name for the PrintTicket metadata ready event.
/// MUST use the Global\ namespace because the spooler service runs
/// in session 0 while the UI runs in the user's interactive session.
constexpr const wchar_t* kPTMetadataReadyEventName =
  L"Global\\WPCE_PT_Metadata_Ready";

enum class EventRole
{
  Creator,  // CreateEventW - the signalling side (filter DLL)
  Opener,   // OpenEventW  - the waiting side (WatcherBridge)
};

class NamedEvent
{
 public:
  /// Create or open the named event.
  /// @param name  Must match on both sides (use kPTMetadataReadyEventName).
  /// @param role  Creator calls CreateEventW, Opener calls OpenEventW.
  NamedEvent(std::wstring_view name, EventRole role);
  ~NamedEvent();

  NamedEvent(const NamedEvent&) = delete;
  NamedEvent& operator=(const NamedEvent&) = delete;
  NamedEvent(NamedEvent&& other) noexcept;
  NamedEvent& operator=(NamedEvent&& other) noexcept;

  /// True if the underlying HANDLE was obtained successfully.
  [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

  /// Signal the event.  Idempotent on an already-set manual-reset event.
  void signal() noexcept;

  /// Reset the event to the non-signaled state.  Call after consuming
  /// the signal to prepare for the next notification.
  void reset() noexcept;

  /// Wait up to @p timeout_ms milliseconds for the event to be signaled.
  /// Returns WAIT_OBJECT_0 on success, WAIT_TIMEOUT on timeout, or a
  /// raw Win32 error code on failure.
  [[nodiscard]] DWORD wait(DWORD timeout_ms) const noexcept;

  /// Raw handle for use with WaitForMultipleObjects or other APIs.
  [[nodiscard]] HANDLE native_handle() const noexcept { return handle_; }

 private:
  HANDLE handle_ = nullptr;
};

}  // namespace wpce::ipc
