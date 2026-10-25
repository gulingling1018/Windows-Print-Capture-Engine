/// @file filter_pipeline.cpp
/// IPrintPipelineFilter implementation for WPCE XPS capture filter.

#include "filter/filter_pipeline.h"
#include "filter/job_identity.h"
#include "filter/capture_identity.h"
#include "bridge/print_ticket_inbox.h"
#include <memory>
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
HRESULT forward_part(IXpsDocumentConsumer* consumer, IUnknown* part)
{
  if (auto value = common::query_interface<IXpsDocument>(part)) return consumer->SendXpsDocument(value.get());
  if (auto value = common::query_interface<IFixedDocumentSequence>(part)) return consumer->SendFixedDocumentSequence(value.get());
  if (auto value = common::query_interface<IFixedDocument>(part)) return consumer->SendFixedDocument(value.get());
  if (auto value = common::query_interface<IFixedPage>(part)) return consumer->SendFixedPage(value.get());
  return consumer->SendXpsUnknown(part);
}

}  // namespace

// XpsCaptureFilter
XpsCaptureFilter::XpsCaptureFilter() noexcept = default;

XpsCaptureFilter::~XpsCaptureFilter() noexcept
{
  // COM pointers are released by com_ptr destructors.
}

// IUnknown
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

// IPrintPipelineFilter
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

  std::scoped_lock lock(operation_mutex_);
  if (operation_started_) return E_UNEXPECTED;
  cancelled_ = false;
  finished_ = false;
  job_id_ = 0;
  printer_name_.clear();
  last_error_.clear();
  property_bag_.reset();

  try
  {
    // Hold references.
    negotiation_.attach(pINegotiation);
    negotiation_->AddRef();

    pipeline_control_.attach(pIPipelineControl);
    pipeline_control_->AddRef();

    if (pIPropertyBag != nullptr)
    {
      property_bag_.attach(pIPropertyBag);
      property_bag_->AddRef();

      // Variant ownership survives string allocation failures.
      struct PropertyValue
      {
        VARIANT value;
        PropertyValue() { VariantInit(&value); }
        ~PropertyValue() { VariantClear(&value); }
      } property;
      if (SUCCEEDED(property_bag_->GetProperty(XPS_FP_PRINTER_NAME, &property.value)) &&
          property.value.vt == VT_BSTR && property.value.bstrVal)
      {
        const auto name = property.value.bstrVal;
        const int len = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
        if (len > 1)
        {
          printer_name_.resize(static_cast<std::size_t>(len));
          WideCharToMultiByte(CP_UTF8, 0, name, -1, printer_name_.data(), len, nullptr, nullptr);
          printer_name_.pop_back();
        }
      }

      job_id_ = read_job_identity(property_bag_.get()).value_or(0);
    }
    if (job_id_ == 0)
    {
      last_error_ = "PrintJobId missing or invalid; capture disabled for this operation";
      ::OutputDebugStringA("[WPCE] Invalid PrintJobId; print-through remains enabled.\n");
    }

    capture_id_ = storage::unique_storage_id();
    const auto identity = pipeline_physical_snapshot(property_bag_.get(), job_id_);
    physical_identity_ = common::physical_job_identity(identity);
    if (!identity.printer_name.empty()) printer_name_ = identity.printer_name;
    captured_print_tickets_.clear();
    page_artifact_indices_.clear();
    forwarded_part_count_ = 0;

    // Auto-detect output directory from environment variable.
    if (output_directory_.empty())
    {
      const auto needed=GetEnvironmentVariableW(L"WPCE_OUTPUT_DIR",nullptr,0);
      if(needed)
      {
        std::wstring value(needed,L'\0');
        const auto copied=GetEnvironmentVariableW(L"WPCE_OUTPUT_DIR",value.data(),needed);
        if(copied && copied<needed) { value.resize(copied); output_directory_=value; }
      }
    }

    return S_OK;
  }
  catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
  catch (...) { return E_FAIL; }
}

void XpsCaptureFilter::finish_once(IPrintPipelineManagerControl* control) noexcept
{
  if (control && !finished_.exchange(true)) control->FilterFinished();
}

HRESULT STDMETHODCALLTYPE XpsCaptureFilter::StartOperation()
{
  common::com_ptr<IInterFilterCommunicator> negotiation;
  common::com_ptr<IPrintPipelineManagerControl> control;
  {
    std::scoped_lock lock(operation_mutex_);
    if (operation_started_) return cancelled_ ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : E_UNEXPECTED;
    if (!negotiation_) return E_UNEXPECTED;
    operation_started_ = true;
    negotiation = negotiation_;
    control = pipeline_control_;
  }
  struct Completion
  {
    XpsCaptureFilter* filter;
    IPrintPipelineManagerControl* control;
    ~Completion() { filter->finish_once(control); }
  } completion{this, control.get()};

  common::com_ptr<IXpsDocumentConsumer> consumer;
  HRESULT result = S_OK;
  std::unique_ptr<storage::AsyncPageArtifactWriter> artifact_writer;
  try
  {
    if (cancelled_) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    common::com_ptr<IUnknown> reader, writer;
    HRESULT hr = negotiation->RequestReader(reinterpret_cast<void**>(&reader));
    if (FAILED(hr) || !reader) return FAILED(hr) ? hr : E_POINTER;
    hr = negotiation->RequestWriter(reinterpret_cast<void**>(&writer));
    if (FAILED(hr) || !writer) return FAILED(hr) ? hr : E_POINTER;
    common::com_ptr<IXpsDocumentProvider> provider;
    hr = reader->QueryInterface(__uuidof(IXpsDocumentProvider), reinterpret_cast<void**>(&provider));
    if (FAILED(hr) || !provider) return FAILED(hr) ? hr : E_NOINTERFACE;
    hr = writer->QueryInterface(__uuidof(IXpsDocumentConsumer), reinterpret_cast<void**>(&consumer));
    if (FAILED(hr) || !consumer) return FAILED(hr) ? hr : E_NOINTERFACE;

    std::int32_t document_sequence = 0, page_sequence = 0;
    std::string current_job_pt, current_document_pt;
    if (job_id_ != 0 && !output_directory_.empty())
    {
      try
      {
        artifact_writer = std::make_unique<storage::AsyncPageArtifactWriter>(storage::PageArtifactWriterConfig{.output_directory=output_directory_});
        artifact_writer->start();
      }
      catch (...) { artifact_writer.reset(); }
    }
    pending_payloads_.clear();
    while (!cancelled_)
    {
      common::com_ptr<IUnknown> part;
      hr = provider->GetXpsPart(&part);
      if (FAILED(hr)) { result = hr; break; }
      if (!part) break;
      if (hr != S_OK) { result = E_UNEXPECTED; break; }
      if (cancelled_) break;
      // Capture is optional. Its failure must never skip the original part.
      try
      {
        if (job_id_ != 0)
          capture_part(part.get(), document_sequence, page_sequence, current_job_pt, current_document_pt);
      }
      catch (...)
      {
        ::OutputDebugStringA("[WPCE] Capture failed; forwarding original XPS part.\n");
      }
      if (cancelled_) break;
      hr = forward_part(consumer.get(), part.get());
      if (FAILED(hr)) { result = hr; break; }
      ++forwarded_part_count_;
      // The original part is already forwarded; only a bounded page payload is journaled.
      if (artifact_writer)
      {
        for (auto& payload : pending_payloads_)
          if (!artifact_writer->enqueue(std::move(payload)))
            ::OutputDebugStringA("[WPCE] Page transaction not accepted; capture unavailable.\n");
      }
      pending_payloads_.clear();
    }
    if (SUCCEEDED(result) && cancelled_) result = HRESULT_FROM_WIN32(ERROR_CANCELLED);

  }
  catch (const std::bad_alloc&) { result = E_OUTOFMEMORY; }
  catch (...) { result = E_FAIL; }
  if (artifact_writer)
  {
    artifact_writer->stop();
    if (!artifact_writer->drained()) ::OutputDebugStringA("[WPCE] Page transactions retained for retry.\n");
  }
  if (consumer)
  {
    try
    {
      const HRESULT close_result = consumer->CloseSender();
      if (SUCCEEDED(result) && FAILED(close_result)) result = close_result;
    }
    catch (...) { if (SUCCEEDED(result)) result = E_FAIL; }
  }
  if (SUCCEEDED(result) && cancelled_) result = HRESULT_FROM_WIN32(ERROR_CANCELLED);
  return result;
}

HRESULT STDMETHODCALLTYPE XpsCaptureFilter::ShutdownOperation()
{
  cancelled_ = true;
  common::com_ptr<IPrintPipelineManagerControl> control;
  {
    std::scoped_lock lock(operation_mutex_);
    if (!operation_started_)
    {
      operation_started_ = true;
      control = pipeline_control_;
    }
  }
  // An active operation retains its own references and performs its own cleanup.
  finish_once(control.get());
  return S_OK;
}

void XpsCaptureFilter::capture_part(IUnknown* part, std::int32_t& document_sequence,
    std::int32_t& page_sequence, std::string& current_job_pt, std::string& current_document_pt)
{
  // Try to extract PrintTicket from known XPS part types
  // IFixedDocumentSequence -> Job-level PrintTicket
  {
    auto doc_seq = common::query_interface<IFixedDocumentSequence>(part);
    if (doc_seq)
    {
      // Extract Job PrintTicket.
      common::com_ptr<IPartPrintTicket> pt;
      if (SUCCEEDED(doc_seq->GetPrintTicket(&pt)) && pt)
      {
        current_job_pt = extract_print_ticket_xml(pt.get());
        if (!current_job_pt.empty())
        {
          log_pt_capture("Job", current_job_pt);

          // Parse PrintTicket -> MetadataSnapshot
          // This extracts the ACTUAL per-job print settings (colour,
          // duplex, paper, N-up, copies, etc.) from the PrintTicket
          // XML that flows through the XPS pipeline - the only
          // authoritative source for v4 drivers.
          core::MetadataSnapshot pt_meta;
          pt_meta.schema_version = 1;
          pt_meta.job_id = job_id_;
          pt_meta.printer_name = printer_name_;
          parser::parse_print_ticket_xml(current_job_pt, pt_meta);

          // Persist parsed metadata atomically
          // The Bridge thread scans the output directory every ~3 s.
          // A direct write without temp+rename risks the Bridge
          // reading a truncated file -> silent metadata loss.
          // write_text_file_atomic uses temp-file + MoveFileExW
          // with MOVEFILE_REPLACE_EXISTING so the metadata file
          // appears only when fully written.
          if (!output_directory_.empty())
          {
            std::filesystem::path meta_path = output_directory_
              / (capture_id_ + ".pt-message.json");
            try
            {
              const std::string json = bridge::serialize_pt_message({capture_id_, physical_identity_, pt_meta});
              storage::write_text_file_atomic(json, meta_path);

              // Signal the UI process
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
            catch (...) { /* best-effort - don't fail print job */ }
          }
        }
      }
    }
  }

  // IFixedDocument -> Document-level PrintTicket
  {
    auto fixed_doc = common::query_interface<IFixedDocument>(part);
    if (fixed_doc)
    {
      ++document_sequence;
      page_sequence = 0;
      current_document_pt.clear();

      common::com_ptr<IPartPrintTicket> pt;
      if (SUCCEEDED(fixed_doc->GetPrintTicket(&pt)) && pt)
      {
        current_document_pt = extract_print_ticket_xml(pt.get());
        if (!current_document_pt.empty())
        {
          log_pt_capture("Document", current_document_pt);
        }
      }
    }
  }

  // IFixedPage -> Page-level PrintTicket
  {
    auto fixed_page = common::query_interface<IFixedPage>(part);
    if (fixed_page)
    {
      ++page_sequence;

      std::string page_pt;
      common::com_ptr<IPartPrintTicket> pt;
      if (SUCCEEDED(fixed_page->GetPrintTicket(&pt)) && pt)
      {
        page_pt = extract_print_ticket_xml(pt.get());
        if (!page_pt.empty())
        {
          log_pt_capture("Page", page_pt);
        }
      }

      storage::PageArtifactPayload payload;
      auto& index = payload.index;
      index.schema_version = 2;
      index.job_id = job_id_;
      index.printer_name = printer_name_;
      index.capture_id = capture_id_;
      index.document_sequence = document_sequence;
      index.page_sequence = page_sequence;
      const auto prefix = "job-" + storage::printer_storage_token(printer_name_) + "-" + std::to_string(job_id_) + "-" + capture_id_;
      if (!current_job_pt.empty()) { index.job_print_ticket = prefix + ".pt.xml"; payload.job_print_ticket_xml = current_job_pt; }
      if (!current_document_pt.empty()) { index.document_print_ticket = prefix + "-d" + std::to_string(document_sequence) + ".pt.xml"; payload.document_print_ticket_xml = current_document_pt; }
      if (!page_pt.empty()) { index.page_print_ticket = prefix + "-d" + std::to_string(document_sequence) + "-p" + std::to_string(page_sequence) + ".pt.xml"; payload.page_print_ticket_xml = page_pt; }
      common::com_ptr<IPrintReadStream> page_stream;
      const auto stream_result = fixed_page->GetStream(&page_stream);
      index.fixed_page_state = FAILED(stream_result) ? "error" : "unsupported";
      if (SUCCEEDED(stream_result) && page_stream)
      {
        ULONGLONG original_position = 0;
        const bool seekable = SUCCEEDED(page_stream->Seek(0, STREAM_SEEK_CUR, &original_position)) &&
            SUCCEEDED(page_stream->Seek(0, STREAM_SEEK_SET, nullptr));
        struct RestorePosition
        {
          IPrintReadStream* stream; ULONGLONG position; bool enabled;
          ~RestorePosition() { if (enabled) stream->Seek(static_cast<LONGLONG>(position), STREAM_SEEK_SET, nullptr); }
        } restore{page_stream.get(), original_position, seekable};
        if (seekable) payload.fixed_page_xml = read_print_stream(page_stream.get());
        if (!payload.fixed_page_xml.empty())
        {
          index.fixed_page = prefix + "-d" + std::to_string(document_sequence) + "-p" + std::to_string(page_sequence) + ".fpage";
          index.fixed_page_state = "reported";
        }
        else index.fixed_page_state = seekable ? "error" : "unsupported";
      }
      // Accessors are diagnostic samples; complete indices are persisted per page.
      if (page_artifact_indices_.size() >= 64) page_artifact_indices_.erase(page_artifact_indices_.begin());
      page_artifact_indices_.push_back(index);
      if (!output_directory_.empty()) pending_payloads_.push_back(std::move(payload));
    }
  }

}

// Persistence
void XpsCaptureFilter::set_output_directory(const std::filesystem::path& dir)
{
  output_directory_ = dir;
}

// Internal helpers
void XpsCaptureFilter::log_pt_capture(const char* /*level*/, const std::string& xml)
{
  if (captured_print_tickets_.size() >= 16) captured_print_tickets_.erase(captured_print_tickets_.begin());
  captured_print_tickets_.push_back(xml.substr(0, 64 * 1024));
}

}  // namespace wpce::filter
