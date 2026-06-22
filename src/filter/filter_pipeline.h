#pragma once

#ifdef _WIN32
#include <windows.h>
#include <filterpipeline.h>
#include <xpsprint.h>
#endif

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "common/com_ptr.h"
#include "core/data_contracts.h"
#include "storage/page_artifact_writer.h"

namespace wpce::filter
{

// ── CLSID for the XPS capture filter ────────────────────────────────────
// {B1C8E3F0-2D4A-5B6C-7D8E-9F0A1B2C3D4E}
extern const CLSID CLSID_XpsCaptureFilter;

/// Minimal no-UI XPSDrv filter that captures PrintTicket and
/// FixedPage data without modifying the print output.
class XpsCaptureFilter final : public IPrintPipelineFilter
{
 public:
  XpsCaptureFilter() noexcept;
  virtual ~XpsCaptureFilter() noexcept;

  // ── IUnknown ──────────────────────────────────────────────────────────

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  // ── IPrintPipelineFilter ──────────────────────────────────────────────

  HRESULT STDMETHODCALLTYPE InitializeFilter(
    IInterFilterCommunicator* pINegotiation,
    IPrintPipelinePropertyBag* pIPropertyBag,
    IPrintPipelineManagerControl* pIPipelineControl) override;

  HRESULT STDMETHODCALLTYPE StartOperation() override;

  HRESULT STDMETHODCALLTYPE ShutdownOperation() override;

  // Windows 10 SDK IPrintPipelineFilter only has the three methods above.
  // PauseOperation/ResumeOperation are not part of the standard vtable —
  // they were added in IPrintPipelineFilterV2 (WDK-only extended interface)
  // which the spooler does not call for a basic pass-through filter.

  // ── Accessors (for test harness) ──────────────────────────────────────

  /// Collected PrintTicket XML strings, indexed by level.
  /// [0] = Job-level, [1+] = Document-level, then Page-level.
  [[nodiscard]] const std::vector<std::string>& captured_print_tickets() const noexcept
  {
    return captured_print_tickets_;
  }

  /// Parts forwarded to consumer (count).
  [[nodiscard]] std::uint32_t forwarded_part_count() const noexcept
  {
    return forwarded_part_count_;
  }

  /// Collected page artifact indices (for test harness access).
  [[nodiscard]] const std::vector<core::PageArtifactIndex>& page_artifacts() const noexcept
  {
    return page_artifact_indices_;
  }

  /// Last error description, if any.
  [[nodiscard]] const std::string& last_error() const noexcept
  {
    return last_error_;
  }

  /// Set the output directory for PrintTicket / FixedPage file persistence.
  /// May be called before InitializeFilter or before StartOperation.
  void set_output_directory(const std::filesystem::path& dir);

 private:
  void log_pt_capture(const char* level, const std::string& xml);

  // ── COM state ─────────────────────────────────────────────────────────

  LONG ref_count_{1};

  // ── Pipeline interfaces (valid during StartOperation) ─────────────────

  common::com_ptr<IInterFilterCommunicator> negotiation_;
  common::com_ptr<IPrintPipelineManagerControl> pipeline_control_;
  common::com_ptr<IPrintPipelinePropertyBag> property_bag_;

  // ── Capture state ─────────────────────────────────────────────────────

  std::string printer_name_;
  std::uint32_t job_id_{0};
  std::vector<std::string> captured_print_tickets_;
  std::vector<core::PageArtifactIndex> page_artifact_indices_;
  std::uint32_t forwarded_part_count_{0};
  std::string last_error_;

  // ── Persistence (optional) ────────────────────────────────────────────

  std::filesystem::path output_directory_;
  std::vector<storage::PageArtifactPayload> pending_payloads_;
};

/// COM class factory for XpsCaptureFilter.
class XpsCaptureFilterClassFactory final : public IClassFactory
{
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject) override;
  HRESULT STDMETHODCALLTYPE LockServer(BOOL fLock) override;

 private:
  LONG ref_count_{1};
};

}  // namespace wpce::filter
