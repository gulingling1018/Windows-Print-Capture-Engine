/// @file printer_profile.cpp
/// PrinterProfile implementation + Win32 capability detection.

#include "common/printer_profile.h"
#include "common/paper_size_map.h"

#include <cctype>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winspool.h>
#endif

#include <algorithm>
#include <cctype>
#include <set>

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
  // DC_COLORDEVICE returns the count of color-capable "devices" -
  // for a single printer this is 1 (color) or 0 (monochrome).
  const int result = DeviceCapabilitiesW(
      printerName.c_str(), nullptr, DC_COLORDEVICE, nullptr, nullptr);
  return result > 0;
}

/// Query DeviceCapabilitiesW(DC_PAPERNAMES) for a printer.
/// Returns a list of supported paper size names (human-readable).
std::vector<std::string> query_paper_sizes(const std::wstring& printerName)
{
  std::vector<std::string> sizes;

  // First pass: get the required buffer size.
  const int count = DeviceCapabilitiesW(
      printerName.c_str(), nullptr, DC_PAPERNAMES, nullptr, nullptr);
  if (count <= 0) return sizes;

  // DC_PAPERNAMES returns an array of null-terminated wchar_t strings
  // packed contiguously in a single buffer.
  const DWORD bufSize = static_cast<DWORD>(count) * 64;  // generous per-name buffer
  std::vector<WCHAR> nameBuf(bufSize, L'\0');
  const int actualCount = DeviceCapabilitiesW(
      printerName.c_str(), nullptr, DC_PAPERNAMES,
      nameBuf.data(), nullptr);
  if (actualCount <= 0) return sizes;

  // Also query the corresponding paper IDs via DC_PAPERS.
  std::vector<WORD> paperIDs(static_cast<std::size_t>(actualCount), 0);
  DeviceCapabilitiesW(
      printerName.c_str(), nullptr, DC_PAPERS,
      reinterpret_cast<LPWSTR>(paperIDs.data()), nullptr);

  // Parse the packed string buffer.
  const WCHAR* p = nameBuf.data();
  for (int i = 0; i < actualCount && p < nameBuf.data() + bufSize; ++i)
  {
    // Each entry is a null-terminated wide string.
    std::wstring wname(p);
    if (wname.empty()) break;

    // Try mapping via DMPAPER ID first for canonical names.
    std::string name;
    if (i < static_cast<int>(paperIDs.size()) && paperIDs[i] != 0)
    {
      std::string_view canonical = paper_size_name(static_cast<std::int16_t>(paperIDs[i]));
      if (!canonical.empty())
      {
        name = std::string(canonical);
      }
    }

    // Fallback: convert the driver-reported name.
    if (name.empty())
    {
      int len = WideCharToMultiByte(CP_UTF8, 0, wname.c_str(),
                                    static_cast<int>(wname.size()),
                                    nullptr, 0, nullptr, nullptr);
      if (len > 0)
      {
        name.resize(static_cast<std::size_t>(len));
        WideCharToMultiByte(CP_UTF8, 0, wname.c_str(),
                            static_cast<int>(wname.size()),
                            name.data(), len, nullptr, nullptr);
      }
    }

    if (!name.empty()) sizes.push_back(name);

    // Advance past the null terminator.
    p += wname.size() + 1;
  }

  return sizes;
}

}  // namespace

PrinterProfileConfig detect_printer_capabilities()
{
  PrinterProfileConfig config;
  for (const auto& name : enumerate_local_printer_names())
  {
    PrinterProfile profile;
    // Narrow UTF-16 -> UTF-8 for storage.
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
    profile.supported_paper_sizes = query_paper_sizes(name);
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

const PrinterProfile* lookup_printer_profile(
    const PrinterProfileConfig& config,
    std::string_view printer_name) noexcept
{
  if (printer_name.empty()) return nullptr;
  for (const auto& p : config.profiles)
  {
    if (printer_name_matches(p.printer_name, printer_name))
      return &p;
  }
  return nullptr;
}

std::vector<std::string> get_paper_sizes_for_printer(
    const PrinterProfileConfig& config,
    std::string_view printer_name)
{
  const auto* profile = lookup_printer_profile(config, printer_name);
  if (profile != nullptr)
    return profile->supported_paper_sizes;
  return {};
}

std::vector<std::string> get_all_paper_sizes(
    const PrinterProfileConfig& config)
{
  // Case-insensitive dedup using a set with lowercase keys.
  // We store the original-cased name for display but use lowercase for dedup.
  std::unordered_map<std::string, std::string> lower_to_original;
  std::set<std::string> sorted_keys;  // sorted lowercased for consistent UI order
  for (const auto& p : config.profiles)
  {
    for (const auto& size : p.supported_paper_sizes)
    {
      if (size.empty()) continue;
      std::string lower;
      lower.reserve(size.size());
      for (char c : size) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (lower_to_original.find(lower) == lower_to_original.end())
      {
        lower_to_original[lower] = size;  // preserve first-seen casing
        sorted_keys.insert(lower);
      }
    }
  }
  std::vector<std::string> result;
  result.reserve(sorted_keys.size());
  for (const auto& key : sorted_keys)
    result.push_back(lower_to_original[key]);
  return result;
}

}  // namespace wpce::common
