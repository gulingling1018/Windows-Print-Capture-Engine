/// @file printer_profile.cpp
/// PrinterProfile implementation + Win32 capability detection.

#include "common/printer_profile.h"

#ifdef _WIN32
#include <windows.h>
#include <winspool.h>
#endif

#include <algorithm>
#include <cctype>

namespace wpce::common
{

bool printer_name_matches(std::string_view a, std::string_view b) noexcept
{
  if (a.size() != b.size()) return false;
  return std::equal(a.begin(), a.end(), b.begin(),
                    [](char ca, char cb) {
                      return std::tolower(static_cast<unsigned char>(ca)) ==
                             std::tolower(static_cast<unsigned char>(cb));
                    });
}

bool lookup_color_capability(
    const PrinterProfileConfig& config,
    std::string_view printer_name) noexcept
{
  if (printer_name.empty()) return config.default_color_capable;
  for (const auto& p : config.profiles)
  {
    if (printer_name_matches(p.printer_name, printer_name))
      return p.is_color_capable;
  }
  return config.default_color_capable;
}

#ifdef _WIN32

namespace
{

/// Enumerate local printer names via EnumPrintersW(PRINTER_ENUM_LOCAL).
std::vector<std::wstring> enumerate_local_printer_names()
{
  std::vector<std::wstring> names;
  DWORD bytesNeeded = 0;
  DWORD count = 0;
  EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS,
                nullptr, 2, nullptr, 0, &bytesNeeded, &count);
  if (bytesNeeded == 0) return names;

  std::vector<std::byte> buffer(bytesNeeded);
  if (!EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS,
                     nullptr, 2,
                     reinterpret_cast<LPBYTE>(buffer.data()),
                     static_cast<DWORD>(buffer.size()),
                     &bytesNeeded, &count))
  {
    return names;
  }

  const auto* info = reinterpret_cast<const PRINTER_INFO_2W*>(buffer.data());
  for (DWORD i = 0; i < count; ++i)
  {
    if (info[i].pPrinterName != nullptr)
      names.emplace_back(info[i].pPrinterName);
  }
  return names;
}

/// Query DeviceCapabilitiesW(DC_COLORDEVICE) for a printer.
/// Returns true if the printer reports it supports color output.
bool query_color_device(const std::wstring& printerName)
{
  // DC_COLORDEVICE returns the count of color-capable "devices" —
  // for a single printer this is 1 (color) or 0 (monochrome).
  const int result = DeviceCapabilitiesW(
      printerName.c_str(), nullptr, DC_COLORDEVICE, nullptr, nullptr);
  return result > 0;
}

}  // namespace

PrinterProfileConfig detect_printer_capabilities()
{
  PrinterProfileConfig config;
  for (const auto& name : enumerate_local_printer_names())
  {
    PrinterProfile profile;
    // Narrow UTF-16 → UTF-8 for storage.
    int len = WideCharToMultiByte(CP_UTF8, 0, name.c_str(),
                                  static_cast<int>(name.size()),
                                  nullptr, 0, nullptr, nullptr);
    profile.printer_name.resize(static_cast<std::size_t>(len));
    if (len > 0)
    {
      WideCharToMultiByte(CP_UTF8, 0, name.c_str(),
                          static_cast<int>(name.size()),
                          profile.printer_name.data(), len,
                          nullptr, nullptr);
    }
    profile.is_color_capable = query_color_device(name);
    config.profiles.push_back(std::move(profile));
  }
  return config;
}

#else  // non-Windows

PrinterProfileConfig detect_printer_capabilities()
{
  return {};
}

#endif

}  // namespace wpce::common
