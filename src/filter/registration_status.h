#pragma once
#include <windows.h>
namespace wpce::filter
{
inline constexpr const wchar_t* kClsidRegistryRoot = L"SOFTWARE\\Classes\\CLSID\\";
inline HRESULT registration_hresult(LONG status) noexcept
{ return status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(status); }
}
