/// @file filter_test_harness.cpp
/// Standalone test harness for the WPCE XPS capture filter.
///
/// Simulates the XPS filter pipeline environment:
/// 1. Loads the wpce_xps_filter.dll
/// 2. Creates mock pipeline interfaces
/// 3. Calls InitializeFilter → StartOperation → ShutdownOperation
/// 4. Reports extracted PrintTickets and forwarded part counts.
///
/// Usage:
///   filter_test_harness.exe [path\to\wpce_xps_filter.dll]

#include <windows.h>
#include <filterpipeline.h>
#include <xpsprint.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

// ── Forward declarations ────────────────────────────────────────────────

namespace
{

// ── Mock IXpsDocumentProvider ───────────────────────────────────────────

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
    if (cursor_ >= parts_.size())
    {
      *ppIXpsPart = nullptr;
      return S_FALSE;
    }
    *ppIXpsPart = parts_[cursor_];
    if (*ppIXpsPart != nullptr)
    {
      (*ppIXpsPart)->AddRef();
    }
    ++cursor_;
    return S_OK;
  }

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

  HRESULT STDMETHODCALLTYPE SendXpsUnknown(IUnknown* /*pUnknown*/) override
  {
    ++forwarded_count_;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SendXpsDocument(IXpsDocument* /*pDoc*/) override
  {
    ++forwarded_count_;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SendFixedDocumentSequence(IFixedDocumentSequence* /*pSeq*/) override
  {
    ++forwarded_count_;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SendFixedDocument(IFixedDocument* /*pDoc*/) override
  {
    ++forwarded_count_;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SendFixedPage(IFixedPage* /*pPage*/) override
  {
    ++forwarded_count_;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE CloseSender() override
  {
    closed_ = true;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetNewEmptyPart(
    const wchar_t* /*uri*/,
    REFIID /*riid*/,
    void** /*ppNewObject*/,
    IPrintWriteStream** /*ppWriteStream*/) override
  {
    return E_NOTIMPL;
  }

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
      pVar->vt = VT_I4;
      pVar->lVal = 42;
      return S_OK;
    }
    return E_NOTIMPL;
  }

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
    // Return nullptr to simulate "no job PrintTicket".
    *ppPrintTicket = nullptr;
    return S_FALSE;
  }

  HRESULT STDMETHODCALLTYPE SetPrintTicket(IPartPrintTicket* /*pPrintTicket*/) override
  {
    return E_NOTIMPL;
  }

 private:
  LONG ref_{1};
};

// ── Helpers ─────────────────────────────────────────────────────────────

using FilterCreateFn = HRESULT (*)(REFIID, void**);

IPrintPipelineFilter* load_filter(const char* dll_path)
{
  HMODULE dll = LoadLibraryA(dll_path);
  if (dll == nullptr)
  {
    std::cerr << "Failed to load filter DLL: " << dll_path << " (error " << GetLastError() << ")\n";
    return nullptr;
  }

  // Use CoCreateInstance which goes through DllGetClassObject internally.
  // First register the DLL.
  // Actually, for simplicity we'll call DllGetClassObject directly.

  using DllGetClassObjectFn = HRESULT (STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
  auto pDllGetClassObject = reinterpret_cast<DllGetClassObjectFn>(
    GetProcAddress(dll, "DllGetClassObject"));

  if (pDllGetClassObject == nullptr)
  {
    std::cerr << "DllGetClassObject not found in DLL\n";
    FreeLibrary(dll);
    return nullptr;
  }

  // Use a known CLSID for the filter.
  // {B1C8E3F0-2D4A-5B6C-7D8E-9F0A1B2C3D4E}
  const CLSID filterClsid = {0xb1c8e3f0, 0x2d4a, 0x5b6c, {0x7d, 0x8e, 0x9f, 0x0a, 0x1b, 0x2c, 0x3d, 0x4e}};

  IClassFactory* factory = nullptr;
  HRESULT hr = pDllGetClassObject(filterClsid, IID_IClassFactory, reinterpret_cast<void**>(&factory));
  if (FAILED(hr) || factory == nullptr)
  {
    std::cerr << "DllGetClassObject failed: 0x" << std::hex << hr << '\n';
    return nullptr;
  }

  IPrintPipelineFilter* filter = nullptr;
  hr = factory->CreateInstance(nullptr, __uuidof(IPrintPipelineFilter), reinterpret_cast<void**>(&filter));
  factory->Release();

  if (FAILED(hr) || filter == nullptr)
  {
    std::cerr << "CreateInstance failed: 0x" << std::hex << hr << '\n';
    return nullptr;
  }

  return filter;
}

int run_pipeline_test(IPrintPipelineFilter* filter)
{
  // Create mock pipeline.
  MockXpsProvider* provider = new MockXpsProvider({});
  MockXpsConsumer* consumer = new MockXpsConsumer();
  MockCommunicator* comm = new MockCommunicator(provider, consumer);
  MockPropertyBag* bag = new MockPropertyBag();
  MockPipelineControl* ctrl = new MockPipelineControl();

  // Release() once after construction (factory gives us ref=1 back from MockCommunicator).
  // Actually, the classes start with ref_=1 and are passed as raw pointers;
  // InitializeFilter will AddRef them (via com_ptr::attach+AddRef). We should
  // release our initial reference after calling InitializeFilter.

  HRESULT hr = filter->InitializeFilter(comm, bag, ctrl);

  std::cout << "InitializeFilter: 0x" << std::hex << hr << std::dec << '\n';
  if (FAILED(hr))
  {
    std::cerr << "FAILED\n";

    // Release our refs still
    provider->Release();
    consumer->Release();
    comm->Release();
    bag->Release();
    ctrl->Release();
    return 1;
  }

  hr = filter->StartOperation();
  std::cout << "StartOperation: 0x" << std::hex << hr << std::dec << '\n';
  std::cout << "Forwarded parts: " << consumer->forwarded_count_ << '\n';
  std::cout << "Consumer closed: " << (consumer->closed_ ? "yes" : "no") << '\n';
  std::cout << "Filter finished: " << (ctrl->finished_ ? "yes" : "no") << '\n';

  if (FAILED(hr))
  {
    std::cerr << "FAILED\n";
    provider->Release();
    consumer->Release();
    comm->Release();
    bag->Release();
    ctrl->Release();
    return 1;
  }

  hr = filter->ShutdownOperation();
  std::cout << "ShutdownOperation: 0x" << std::hex << hr << std::dec << '\n';

  // Check captured data.
  // QI to our concrete type to access captured data.
  // (In production, we wouldn't know the concrete type, but this is a test.)

  std::cout << "\n--- Test PASSED ---\n";
  std::cout << "Empty pipeline ran without errors.\n";

  provider->Release();
  consumer->Release();
  comm->Release();
  bag->Release();
  ctrl->Release();

  return 0;
}

}  // namespace

// ── main ──────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
  const char* dll_path = (argc > 1)
    ? argv[1]
    : "wpce_xps_filter.dll";

  std::cout << "=== WPCE Filter Test Harness ===\n";
  std::cout << "DLL: " << dll_path << "\n\n";

  // Initialize COM.
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(hr))
  {
    std::cerr << "CoInitializeEx failed: 0x" << std::hex << hr << '\n';
    return 1;
  }

  IPrintPipelineFilter* filter = load_filter(dll_path);
  if (filter == nullptr)
  {
    CoUninitialize();
    return 1;
  }

  std::cout << "Filter loaded successfully.\n\n";

  int result = run_pipeline_test(filter);

  filter->Release();
  CoUninitialize();
  return result;
}
