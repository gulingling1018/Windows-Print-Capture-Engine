#pragma once
#include <windows.h>
#include <filterpipeline.h>
#include <xpsprint.h>
#include <functional>
#include <limits>
#include <new>
#include <vector>

namespace wpce::test_support
{
class MockXpsProvider final : public IXpsDocumentProvider
{
 public:
  explicit MockXpsProvider(std::vector<IUnknown*> parts)
    : parts_(std::move(parts))
  {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
  {
    if (ppv == nullptr) return E_POINTER;
    if (riid == IID_IUnknown || riid == __uuidof(IXpsDocumentProvider))
    {
      *ppv = static_cast<IXpsDocumentProvider*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override
  {
    return InterlockedIncrement(&ref_);
  }

  ULONG STDMETHODCALLTYPE Release() override
  {
    const LONG c = InterlockedDecrement(&ref_);
    if (c == 0) delete this;
    return static_cast<ULONG>(c);
  }

  HRESULT STDMETHODCALLTYPE GetXpsPart(IUnknown** ppIXpsPart) override
  {
    if (ppIXpsPart == nullptr) return E_POINTER;
    if (before_read) before_read();
    if (cursor_ == fail_at) return read_failure;
    if (cursor_ >= parts_.size())
    {
      *ppIXpsPart = nullptr;
      return terminal_result;
    }
    *ppIXpsPart = parts_[cursor_];
    if (*ppIXpsPart != nullptr)
    {
      (*ppIXpsPart)->AddRef();
    }
    ++cursor_;
    return S_OK;
  }

  std::size_t fail_at{std::numeric_limits<std::size_t>::max()};
  HRESULT read_failure{E_FAIL};
  HRESULT terminal_result{S_FALSE};
  std::function<void()> before_read;
  ULONG references() const { return ref_; }
 private:
  std::vector<IUnknown*> parts_;
  std::size_t cursor_{0};
  LONG ref_{1};
};

// ── Mock IXpsDocumentConsumer ───────────────────────────────────────────

class MockXpsConsumer final : public IXpsDocumentConsumer
{
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
  {
    if (ppv == nullptr) return E_POINTER;
    if (riid == IID_IUnknown || riid == __uuidof(IXpsDocumentConsumer))
    {
      *ppv = static_cast<IXpsDocumentConsumer*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override
  {
    return InterlockedIncrement(&ref_);
  }

  ULONG STDMETHODCALLTYPE Release() override
  {
    const LONG c = InterlockedDecrement(&ref_);
    if (c == 0) delete this;
    return static_cast<ULONG>(c);
  }

  HRESULT STDMETHODCALLTYPE SendXpsUnknown(IUnknown* part) override
  {
    if (forwarded_count_ == fail_at) return send_failure;
    forwarded.push_back(part);
    ++forwarded_count_;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SendXpsDocument(IXpsDocument* pDoc) override
  {
    return SendXpsUnknown(pDoc);
  }

  HRESULT STDMETHODCALLTYPE SendFixedDocumentSequence(IFixedDocumentSequence* pSeq) override
  {
    return SendXpsUnknown(pSeq);
  }

  HRESULT STDMETHODCALLTYPE SendFixedDocument(IFixedDocument* pDoc) override
  {
    return SendXpsUnknown(pDoc);
  }

  HRESULT STDMETHODCALLTYPE SendFixedPage(IFixedPage* pPage) override
  {
    return SendXpsUnknown(pPage);
  }

  HRESULT STDMETHODCALLTYPE CloseSender() override
  {
    closed_ = true;
    ++close_count;
    return close_result;
  }

  HRESULT STDMETHODCALLTYPE GetNewEmptyPart(
    const wchar_t* /*uri*/,
    REFIID /*riid*/,
    void** /*ppNewObject*/,
    IPrintWriteStream** /*ppWriteStream*/) override
  {
    return E_NOTIMPL;
  }

  std::vector<IUnknown*> forwarded;
  DWORD fail_at{MAXDWORD};
  HRESULT send_failure{E_ACCESSDENIED};
  HRESULT close_result{S_OK};
  int close_count{0};
  DWORD forwarded_count_{0};
  bool closed_{false};

 private:
  LONG ref_{1};
};

// ── Mock IInterFilterCommunicator ──────────────────────────────────────

class MockCommunicator final : public IInterFilterCommunicator
{
 public:
  MockCommunicator(IXpsDocumentProvider* provider, IXpsDocumentConsumer* consumer)
    : provider_(provider), consumer_(consumer)
  {
    if (provider_) provider_->AddRef();
    if (consumer_) consumer_->AddRef();
  }

  ~MockCommunicator()
  {
    if (provider_) provider_->Release();
    if (consumer_) consumer_->Release();
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
  {
    if (ppv == nullptr) return E_POINTER;
    if (riid == IID_IUnknown || riid == __uuidof(IInterFilterCommunicator))
    {
      *ppv = static_cast<IInterFilterCommunicator*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override
  {
    return InterlockedIncrement(&ref_);
  }

  ULONG STDMETHODCALLTYPE Release() override
  {
    const LONG c = InterlockedDecrement(&ref_);
    if (c == 0) delete this;
    return static_cast<ULONG>(c);
  }

  HRESULT STDMETHODCALLTYPE RequestReader(void** ppIReader) override
  {
    if (ppIReader == nullptr || provider_ == nullptr) return E_FAIL;
    *ppIReader = static_cast<IXpsDocumentProvider*>(provider_);
    provider_->AddRef();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE RequestWriter(void** ppIWriter) override
  {
    if (ppIWriter == nullptr || consumer_ == nullptr) return E_FAIL;
    *ppIWriter = static_cast<IXpsDocumentConsumer*>(consumer_);
    consumer_->AddRef();
    return S_OK;
  }

 private:
  IXpsDocumentProvider* provider_;
  IXpsDocumentConsumer* consumer_;
  LONG ref_{1};
};

// ── Mock IPrintPipelinePropertyBag ─────────────────────────────────────

class MockPropertyBag final : public IPrintPipelinePropertyBag
{
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
  {
    if (ppv == nullptr) return E_POINTER;
    if (riid == IID_IUnknown || riid == __uuidof(IPrintPipelinePropertyBag))
    {
      *ppv = static_cast<IPrintPipelinePropertyBag*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
  ULONG STDMETHODCALLTYPE Release() override
  {
    const LONG c = InterlockedDecrement(&ref_);
    if (c == 0) delete this;
    return static_cast<ULONG>(c);
  }

  HRESULT STDMETHODCALLTYPE AddProperty(const wchar_t* /*pszName*/, const VARIANT* /*pVar*/) override
  {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE GetProperty(const wchar_t* pszName, VARIANT* pVar) override
  {
    if (pszName == nullptr || pVar == nullptr) return E_INVALIDARG;

    if (wcscmp(pszName, L"PrinterName") == 0)
    {
      pVar->vt = VT_BSTR;
      pVar->bstrVal = SysAllocString(L"Mock Test Printer");
      return S_OK;
    }
    if (wcscmp(pszName, L"PrintJobId") == 0)
    {
      pVar->vt = id_type;
      pVar->ulVal = job_id;
      return id_result;
    }
    return E_NOTIMPL;
  }

  VARTYPE id_type{VT_UI4};
  ULONG job_id{42};
  HRESULT id_result{S_OK};

  BOOL STDMETHODCALLTYPE DeleteProperty(const wchar_t* /*pszName*/) override
  {
    return FALSE;
  }

 private:
  LONG ref_{1};
};

// ── Mock IPrintPipelineManagerControl ──────────────────────────────────

class MockPipelineControl final : public IPrintPipelineManagerControl
{
 public:
  int finish_count{0};
  bool finished_{false};
  bool shutdown_{false};
  HRESULT shutdown_reason_{S_OK};

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
  {
    if (ppv == nullptr) return E_POINTER;
    if (riid == IID_IUnknown || riid == __uuidof(IPrintPipelineManagerControl))
    {
      *ppv = static_cast<IPrintPipelineManagerControl*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
  ULONG STDMETHODCALLTYPE Release() override
  {
    const LONG c = InterlockedDecrement(&ref_);
    if (c == 0) delete this;
    return static_cast<ULONG>(c);
  }

  HRESULT STDMETHODCALLTYPE RequestShutdown(HRESULT hrReason, IImgErrorInfo* /*pReason*/) override
  {
    shutdown_ = true;
    shutdown_reason_ = hrReason;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE FilterFinished() override
  {
    ++finish_count;
    finished_ = true;
    return S_OK;
  }

 private:
  LONG ref_{1};
};

// ── Mock part: IFixedDocumentSequence with PrintTicket ─────────────────

/// A minimal mock that implements IFixedDocumentSequence and returns a
/// dummy PrintTicket for testing the extraction path.
class MockFixedDocumentSequence final : public IFixedDocumentSequence
{
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
  {
    if (ppv == nullptr) return E_POINTER;
    if (riid == IID_IUnknown || riid == __uuidof(IFixedDocumentSequence))
    {
      *ppv = static_cast<IFixedDocumentSequence*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
  ULONG STDMETHODCALLTYPE Release() override
  {
    const LONG c = InterlockedDecrement(&ref_);
    if (c == 0) delete this;
    return static_cast<ULONG>(c);
  }

  HRESULT STDMETHODCALLTYPE GetUri(BSTR* /*uri*/) override
  {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE GetPrintTicket(IPartPrintTicket** ppPrintTicket) override
  {
    if (ppPrintTicket == nullptr) return E_POINTER;
    if (throw_capture) throw std::bad_alloc{};
    *ppPrintTicket = ticket;
    if (ticket) ticket->AddRef();
    return ticket ? S_OK : S_FALSE;
  }

  HRESULT STDMETHODCALLTYPE SetPrintTicket(IPartPrintTicket* /*pPrintTicket*/) override
  {
    return E_NOTIMPL;
  }

  bool throw_capture{false};
  IPartPrintTicket* ticket{nullptr};
 private:
  LONG ref_{1};
};

// ── Helpers ─────────────────────────────────────────────────────────────

}
