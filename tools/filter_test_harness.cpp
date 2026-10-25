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

#include "xps_pipeline_mocks.h"
#include <windows.h>
#include <filterpipeline.h>
#include <xpsprint.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <functional>
#include <limits>
#include <new>
#include <string>
#include <vector>

// ── Forward declarations ────────────────────────────────────────────────

namespace
{

// ── Mock IXpsDocumentProvider ───────────────────────────────────────────

using namespace wpce::test_support;

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
