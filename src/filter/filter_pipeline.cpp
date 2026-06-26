/// @file filter_pipeline.cpp
/// IPrintPipelineFilter implementation for WPCE XPS capture filter.

#include "filter/filter_pipeline.h"
#include "filter/print_ticket_extractor.h"
#include "ipc/named_event.h"
#include "parser/print_ticket_parser.h"
#include "storage/file_utils.h"
#include "storage/json_serializer.h"
#include "storage/page_artifact_writer.h"

#include <filterpipeline.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

namespace wpce::filter
{

namespace
{

void persist_pending_payloads(const std::vector<storage::PageArtifactPayload>& payloads,
                              const std::filesystem::path& output_dir)
{
  if (payloads.empty() || output_dir.empty())
  {
    return;
  }

  storage::PageArtifactWriterConfig writer_config;
  writer_config.output_directory = output_dir;

  storage::AsyncPageArtifactWriter writer(std::move(writer_config));
  writer.start();

  for (const storage::PageArtifactPayload& payload : payloads)
  {
    if (!writer.enqueue(payload))
    {
      break;
    }
  }

  writer.stop();
}

std::string safe_filename_token(std::string value)
{
  if (value.empty())
  {
    return "unknown-printer";
  }

  for (char& ch : value)
  {
    const bool keep =
      (ch >= 'a' && ch <= 'z') ||
      (ch >= 'A' && ch <= 'Z') ||
      (ch >= '0' && ch <= '9') ||
      ch == '-' || ch == '_';
    if (!keep)
    {
      ch = '_';
    }
  }
  return value;
}

}  // namespace

// ── XpsCaptureFilter ───────────────────────────────────────────────────

XpsCaptureFilter::XpsCaptureFilter() noexcept = default;

XpsCaptureFilter::~XpsCaptureFilter() noexcept
{
  // COM pointers are released by com_ptr destructors.
}

// ── IUnknown ────────────────────────────────────────────────────────────

HRESULT STDMETHODCALLTYPE XpsCaptureFilter::QueryInterface(REFIID riid, void** ppvObject)
{
  if (ppvObject == nullptr)
  {
    return E_POINTER;
  }

  if (riid == IID_IUnknown || riid == IID_IPrintPipelineFilter)
  {
    *ppvObject = static_cast<IPrintPipelineFilter*>(this);
    AddRef();
    return S_OK;
  }

  *ppvObject = nullptr;
  return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE XpsCaptureFilter::AddRef()
{
  return ::InterlockedIncrement(&ref_count_);
}

ULONG STDMETHODCALLTYPE XpsCaptureFilter::Release()
{
  const LONG count = ::InterlockedDecrement(&ref_count_);
  if (count == 0)
  {
    delete this;
  }
  return static_cast<ULONG>(count);
}

// ── IPrintPipelineFilter ───────────────────────────────────────────────

HRESULT STDMETHODCALLTYPE XpsCaptureFilter::InitializeFilter(
  IInterFilterCommunicator* pINegotiation,
  IPrintPipelinePropertyBag* pIPropertyBag,
  IPrintPipelineManagerControl* pIPipelineControl)
{
  if (pINegotiation == nullptr || pIPipelineControl == nullptr)
  {
    last_error_ = "InitializeFilter: null negotiation or pipeline control";
    return E_INVALIDARG;
  }

  // Hold references.
  negotiation_.attach(pINegotiation);
  negotiation_->AddRef();

  pipeline_control_.attach(pIPipelineControl);
  pipeline_control_->AddRef();

  if (pIPropertyBag != nullptr)
  {
    property_bag_.attach(pIPropertyBag);
    property_bag_->AddRef();

    // Read printer name from property bag.
    VARIANT var;
    VariantInit(&var);
    if (SUCCEEDED(property_bag_->GetProperty(L"PrinterName", &var)) && var.vt == VT_BSTR)
    {
      const int len = ::WideCharToMultiByte(CP_UTF8, 0, var.bstrVal, -1, nullptr, 0, nullptr, nullptr);
      if (len > 1)
      {
        printer_name_.resize(static_cast<std::size_t>(len) - 1);
        ::WideCharToMultiByte(CP_UTF8, 0, var.bstrVal, -1,
                              printer_name_.data(), len, nullptr, nullptr);
      }
      VariantClear(&var);
    }

    // Read job ID from property bag.
    VariantInit(&var);
    if (SUCCEEDED(property_bag_->GetProperty(L"PrintJobId", &var)) && var.vt == VT_I4)
    {
      job_id_ = static_cast<std::uint32_t>(var.lVal);
      VariantClear(&var);
    }
  }

  captured_print_tickets_.clear();
  page_artifact_indices_.clear();
  forwarded_part_count_ = 0;

  // Auto-detect output directory from environment variable.
  if (output_directory_.empty())
  {
    char* env_dir = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&env_dir, &len, "WPCE_OUTPUT_DIR") == 0 && env_dir != nullptr)
    {
      output_directory_ = std::filesystem::path(env_dir);
      free(env_dir);
    }
  }

  return S_OK;
}

HRESULT STDMETHODCALLTYPE XpsCaptureFilter::StartOperation()
{
  if (!negotiation_)
  {
    last_error_ = "StartOperation: negotiation not available (InitializeFilter not called?)";
    return E_FAIL;
  }

  // ── Acquire reader / writer ──────────────────────────────────────────

  IUnknown* reader_unk = nullptr;
  IUnknown* writer_unk = nullptr;

  HRESULT hr = negotiation_->RequestReader(reinterpret_cast<void**>(&reader_unk));
  if (FAILED(hr) || reader_unk == nullptr)
  {
    last_error_ = "RequestReader failed";
    return FAILED(hr) ? hr : E_FAIL;
  }

  hr = negotiation_->RequestWriter(reinterpret_cast<void**>(&writer_unk));
  if (FAILED(hr) || writer_unk == nullptr)
  {
    last_error_ = "RequestWriter failed";
    if (reader_unk) reader_unk->Release();
    return FAILED(hr) ? hr : E_FAIL;
  }

  IXpsDocumentProvider* provider = nullptr;
  hr = reader_unk->QueryInterface(__uuidof(IXpsDocumentProvider),
                                  reinterpret_cast<void**>(&provider));
  reader_unk->Release();

  if (FAILED(hr) || provider == nullptr)
  {
    last_error_ = "QI for IXpsDocumentProvider failed";
    if (writer_unk) writer_unk->Release();
    return FAILED(hr) ? hr : E_FAIL;
  }

  IXpsDocumentConsumer* consumer = nullptr;
  hr = writer_unk->QueryInterface(__uuidof(IXpsDocumentConsumer),
                                  reinterpret_cast<void**>(&consumer));
  writer_unk->Release();

  if (FAILED(hr) || consumer == nullptr)
  {
    last_error_ = "QI for IXpsDocumentConsumer failed";
    provider->Release();
    return FAILED(hr) ? hr : E_FAIL;
  }

  // ── Process XPS parts ────────────────────────────────────────────────

  std::int32_t document_sequence = 0;
  std::int32_t page_sequence = 0;
  std::string current_job_pt;
  std::string current_document_pt;

  pending_payloads_.clear();

  while (true)
  {
    IUnknown* part = nullptr;
    hr = provider->GetXpsPart(&part);
    if (hr != S_OK || part == nullptr)
    {
      break;
    }

    try
    {

    // ── Try to extract PrintTicket from known XPS part types ────────────

    // IFixedDocumentSequence → Job-level PrintTicket
    {
      IFixedDocumentSequence* doc_seq = nullptr;
      if (SUCCEEDED(part->QueryInterface(__uuidof(IFixedDocumentSequence),
                                         reinterpret_cast<void**>(&doc_seq))))
      {
        // Extract Job PrintTicket.
        IPartPrintTicket* pt = nullptr;
        if (SUCCEEDED(doc_seq->GetPrintTicket(&pt)) && pt != nullptr)
        {
          current_job_pt = extract_print_ticket_xml(pt);
          if (!current_job_pt.empty())
          {
            log_pt_capture("Job", current_job_pt);

            // ── Parse PrintTicket → MetadataSnapshot ──────────────────
            // This extracts the ACTUAL per-job print settings (colour,
            // duplex, paper, N-up, copies, etc.) from the PrintTicket
            // XML that flows through the XPS pipeline — the only
            // authoritative source for v4 drivers.
            core::MetadataSnapshot pt_meta;
            pt_meta.schema_version = 1;
            pt_meta.job_id = job_id_;
            pt_meta.printer_name = printer_name_;
            parser::parse_print_ticket_xml(current_job_pt, pt_meta);

            // ── Persist parsed metadata atomically ───────────────────────
            // The Bridge thread scans the output directory every ~3 s.
            // A direct write without temp+rename risks the Bridge
            // reading a truncated file → silent metadata loss.
            // write_text_file_atomic uses temp-file + MoveFileExW
            // with MOVEFILE_REPLACE_EXISTING so the metadata file
            // appears only when fully written.
            if (!output_directory_.empty())
            {
              const std::string printer_token = safe_filename_token(printer_name_);
              std::filesystem::path meta_path = output_directory_
                / ("job-" + printer_token + "-" + std::to_string(job_id_) + "-pt-metadata.json");
              try
              {
                const std::string json = storage::to_json(pt_meta);
                storage::write_text_file_atomic(json, meta_path);

                // ── Signal the UI process ──────────────────────────
                // The atomic file write above guarantees the file is
                // complete before we signal.  The watcher side wakes
                // up and immediately scans for new PT metadata files.
                try
                {
                  ipc::NamedEvent ready_event(
                    ipc::kPTMetadataReadyEventName, ipc::EventRole::Creator);
                  if (ready_event.valid())
                  {
                    ready_event.signal();
                  }
                }
                catch (...) { /* IPC best-effort, never fail print */ }
              }
              catch (...) { /* best-effort — don't fail print job */ }
            }
          }
          pt->Release();
        }
        doc_seq->Release();
      }
    }

    // IFixedDocument → Document-level PrintTicket
    {
      IFixedDocument* fixed_doc = nullptr;
      if (SUCCEEDED(part->QueryInterface(__uuidof(IFixedDocument),
                                         reinterpret_cast<void**>(&fixed_doc))))
      {
        ++document_sequence;

        IPartPrintTicket* pt = nullptr;
        if (SUCCEEDED(fixed_doc->GetPrintTicket(&pt)) && pt != nullptr)
        {
          current_document_pt = extract_print_ticket_xml(pt);
          if (!current_document_pt.empty())
          {
            log_pt_capture("Document", current_document_pt);
          }
          pt->Release();
        }
        fixed_doc->Release();
      }
    }

    // IFixedPage → Page-level PrintTicket
    {
      IFixedPage* fixed_page = nullptr;
      if (SUCCEEDED(part->QueryInterface(__uuidof(IFixedPage),
                                         reinterpret_cast<void**>(&fixed_page))))
      {
        ++page_sequence;

        // Get page URI for identification.
        BSTR uri = nullptr;
        std::string page_uri;
        if (SUCCEEDED(fixed_page->GetUri(&uri)) && uri != nullptr)
        {
          const int len = ::WideCharToMultiByte(CP_UTF8, 0, uri, -1, nullptr, 0, nullptr, nullptr);
          if (len > 1)
          {
            page_uri.resize(static_cast<std::size_t>(len) - 1);
            ::WideCharToMultiByte(CP_UTF8, 0, uri, -1, page_uri.data(), len, nullptr, nullptr);
          }
          ::SysFreeString(uri);
        }

        std::string page_pt;
        IPartPrintTicket* pt = nullptr;
        if (SUCCEEDED(fixed_page->GetPrintTicket(&pt)) && pt != nullptr)
        {
          page_pt = extract_print_ticket_xml(pt);
          if (!page_pt.empty())
          {
            log_pt_capture("Page", page_pt);
          }
          pt->Release();
        }

        // Build page artifact index entry.
        core::PageArtifactIndex index;
        index.schema_version = 1;
        index.job_id = job_id_;
        index.printer_name = printer_name_;
        index.document_sequence = document_sequence;
        index.page_sequence = page_sequence;
        index.source = "xps-filter";
        const std::string printer_token = safe_filename_token(printer_name_);

        // Set file references (actual files written later in Phase 4a).
        {
          std::ostringstream oss;
          oss << "job-" << printer_token << '-' << job_id_ << ".pt.xml";
          index.job_print_ticket = oss.str();
        }
        {
          std::ostringstream oss;
          oss << "job-" << printer_token << '-' << job_id_
              << "-document-" << std::setfill('0') << std::setw(3) << document_sequence << ".pt.xml";
          index.document_print_ticket = oss.str();
        }
        {
          std::ostringstream oss;
          oss << "job-" << printer_token << '-' << job_id_
              << "-page-" << std::setfill('0') << std::setw(3) << page_sequence << ".pt.xml";
          index.page_print_ticket = oss.str();
        }
        {
          std::ostringstream oss;
          oss << "job-" << printer_token << '-' << job_id_
              << "-page-" << std::setfill('0') << std::setw(3) << page_sequence << ".fpage";
          index.fixed_page = oss.str();
        }

        page_artifact_indices_.push_back(index);

        // Collect payload for persistence.
        if (!output_directory_.empty())
        {
          storage::PageArtifactPayload payload;
          payload.index = index;
          payload.job_print_ticket_xml = current_job_pt;
          payload.document_print_ticket_xml = current_document_pt;
          payload.page_print_ticket_xml = page_pt;
          pending_payloads_.push_back(std::move(payload));
        }

        fixed_page->Release();
      }
    }

    // ── Forward part downstream ─────────────────────────────────────────
    {
      hr = consumer->SendXpsUnknown(part);
      if (SUCCEEDED(hr))
      {
        ++forwarded_part_count_;
      }
    }

      part->Release();
    }
    catch (const std::bad_alloc&)
    {
      ::OutputDebugStringA("[WPCE] StartOperation: OOM processing XPS part, skipping part\n");
      if (part) { part->Release(); }
    }
    catch (const std::exception& ex)
    {
      char dbg[256];
      std::snprintf(dbg, sizeof(dbg), "[WPCE] StartOperation: exception: %s\n", ex.what());
      ::OutputDebugStringA(dbg);
      if (part) { part->Release(); }
    }
  }

  // ── Persist captured artifacts (synchronous, after pipeline loop) ──────

  if (!output_directory_.empty() && !pending_payloads_.empty())
  {
    persist_pending_payloads(pending_payloads_, output_directory_);
  }

  // ── Finish ─────────────────────────────────────────────────────────────

  consumer->CloseSender();
  consumer->Release();
  provider->Release();

  if (pipeline_control_)
  {
    pipeline_control_->FilterFinished();
  }

  return S_OK;
}

HRESULT STDMETHODCALLTYPE XpsCaptureFilter::ShutdownOperation()
{
  // Release pipeline references; we won't be called again.
  negotiation_.reset();
  property_bag_.reset();
  pipeline_control_.reset();
  return S_OK;
}

// ── Persistence ────────────────────────────────────────────────────────

void XpsCaptureFilter::set_output_directory(const std::filesystem::path& dir)
{
  output_directory_ = dir;
}

// ── Internal helpers ───────────────────────────────────────────────────

void XpsCaptureFilter::log_pt_capture(const char* /*level*/, const std::string& xml)
{
  captured_print_tickets_.push_back(xml);
}

}  // namespace wpce::filter
