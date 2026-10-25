/// @file filter_entry.cpp
/// COM DLL entry points for the WPCE XPS capture filter.
///
/// Exports:
///   DllMain              - no-op
///   DllGetClassObject    - returns IClassFactory for XpsCaptureFilter
///   DllCanUnloadNow      - locked-server check
///   DllRegisterServer    - register CLSID in the registry
///   DllUnregisterServer  - remove CLSID from the registry

#include "filter/filter_pipeline.h"
#include "filter/registration_status.h"

#include <windows.h>
#include <string>
#include <sstream>
#include <mutex>

namespace
{

/// Global lock count (incremented by IClassFactory::LockServer).
LONG g_lock_count = 0;

/// Global DLL instance handle.
HINSTANCE g_hinst = nullptr;

/// Write a CLSID registration key.
LONG write_clsid_key(const CLSID& clsid, const wchar_t* description)
{
  std::wostringstream clsid_str;
  {
    wchar_t buffer[39]{};
    ::StringFromGUID2(clsid, buffer, static_cast<int>(std::size(buffer)));
    clsid_str << wpce::filter::kClsidRegistryRoot << buffer;
  }

  HKEY key = nullptr;
  LONG result = ::RegCreateKeyExW(
    HKEY_LOCAL_MACHINE,
    clsid_str.str().c_str(),
    0, nullptr, REG_OPTION_NON_VOLATILE,
    KEY_WRITE, nullptr, &key, nullptr);

  if (result != ERROR_SUCCESS)
  {
    return result;
  }

  result = ::RegSetValueExW(key, nullptr, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(description),
                   static_cast<DWORD>((::wcslen(description) + 1) * sizeof(wchar_t)));
  if (result != ERROR_SUCCESS) { ::RegCloseKey(key); return result; }

  HKEY inproc_key = nullptr;
  result = ::RegCreateKeyExW(
    key, L"InProcServer32", 0, nullptr,
    REG_OPTION_NON_VOLATILE, KEY_WRITE,
    nullptr, &inproc_key, nullptr);

  if (result == ERROR_SUCCESS)
  {
    wchar_t module_path[32768]{};
    const DWORD length = ::GetModuleFileNameW(g_hinst, module_path, static_cast<DWORD>(std::size(module_path)));
    if (!length || length >= std::size(module_path))
    {
      const LONG error = length ? ERROR_INSUFFICIENT_BUFFER : ::GetLastError();
      ::RegCloseKey(inproc_key); ::RegCloseKey(key); return error;
    }

    result = ::RegSetValueExW(inproc_key, nullptr, 0, REG_SZ,
                     reinterpret_cast<const BYTE*>(module_path),
                     static_cast<DWORD>((::wcslen(module_path) + 1) * sizeof(wchar_t)));

    const wchar_t threading_model[] = L"Both";
    if (result == ERROR_SUCCESS) result = ::RegSetValueExW(inproc_key, L"ThreadingModel", 0, REG_SZ,
                     reinterpret_cast<const BYTE*>(threading_model),
                     static_cast<DWORD>((::wcslen(threading_model) + 1) * sizeof(wchar_t)));

    ::RegCloseKey(inproc_key);
  }

  ::RegCloseKey(key);
  return result;
}

LONG delete_clsid_key(const CLSID& clsid)
{
  std::wostringstream clsid_str;
  {
    wchar_t buffer[39]{};
    ::StringFromGUID2(clsid, buffer, static_cast<int>(std::size(buffer)));
    clsid_str << wpce::filter::kClsidRegistryRoot << buffer;
  }

  return ::RegDeleteTreeW(HKEY_LOCAL_MACHINE, clsid_str.str().c_str());
}

}  // namespace

// DLL entry point
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID /*lpvReserved*/)
{
  switch (fdwReason)
  {
    case DLL_PROCESS_ATTACH:
      g_hinst = hinstDLL;
      ::DisableThreadLibraryCalls(hinstDLL);
      break;
    case DLL_PROCESS_DETACH:
      break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
      break;
  }
  return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
  if (ppv == nullptr)
  {
    return E_POINTER;
  }

  if (rclsid != wpce::filter::CLSID_XpsCaptureFilter)
  {
    return CLASS_E_CLASSNOTAVAILABLE;
  }

  static wpce::filter::XpsCaptureFilterClassFactory factory;
  return factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow()
{
  return g_lock_count == 0 ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer()
{
  const wchar_t description[] = L"WPCE XPS Capture Filter";
  LONG result = write_clsid_key(wpce::filter::CLSID_XpsCaptureFilter, description);
  return wpce::filter::registration_hresult(result);
}

STDAPI DllUnregisterServer()
{
  LONG result = delete_clsid_key(wpce::filter::CLSID_XpsCaptureFilter);
  return wpce::filter::registration_hresult(result);
}

// IClassFactory implementation
namespace wpce::filter
{

HRESULT STDMETHODCALLTYPE XpsCaptureFilterClassFactory::QueryInterface(REFIID riid, void** ppvObject)
{
  if (ppvObject == nullptr)
  {
    return E_POINTER;
  }

  if (riid == IID_IUnknown || riid == IID_IClassFactory)
  {
    *ppvObject = static_cast<IClassFactory*>(this);
    AddRef();
    return S_OK;
  }

  *ppvObject = nullptr;
  return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE XpsCaptureFilterClassFactory::AddRef()
{
  return ::InterlockedIncrement(&ref_count_);
}

ULONG STDMETHODCALLTYPE XpsCaptureFilterClassFactory::Release()
{
  LONG count = ::InterlockedDecrement(&ref_count_);
  if (count == 0)
  {
    // Static instance - do not delete.
  }
  return static_cast<ULONG>(count);
}

HRESULT STDMETHODCALLTYPE XpsCaptureFilterClassFactory::CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject)
{
  if (ppvObject == nullptr)
  {
    return E_POINTER;
  }

  if (pUnkOuter != nullptr)
  {
    return CLASS_E_NOAGGREGATION;
  }

  XpsCaptureFilter* filter = new (std::nothrow) XpsCaptureFilter();
  if (filter == nullptr)
  {
    return E_OUTOFMEMORY;
  }

  HRESULT hr = filter->QueryInterface(riid, ppvObject);
  filter->Release();  // Balance constructor's implicit AddRef
  return hr;
}

HRESULT STDMETHODCALLTYPE XpsCaptureFilterClassFactory::LockServer(BOOL fLock)
{
  if (fLock)
  {
    ::InterlockedIncrement(&g_lock_count);
  }
  else
  {
    ::InterlockedDecrement(&g_lock_count);
  }
  return S_OK;
}

}  // namespace wpce::filter
