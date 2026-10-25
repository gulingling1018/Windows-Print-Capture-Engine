/// @file printer_capability_scanner.cpp
/// Comprehensive printer capability scanner using DeviceCapabilitiesW.
///
/// Usage:
///   wpce_printer_scanner.exe                        # scan all local printers
///   wpce_printer_scanner.exe "小机器三" "封面机"      # scan specific printers
///
/// Queries the full set of printing parameter presets:
///   color, paper names/sizes, duplex, copies, orientation,
///   N-up, collate, media types, resolutions, paper bins,
///   printable area, driver version, dmFields bitmask.

#ifdef _WIN32

#include <windows.h>
#include <winspool.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// Reuse the paper-size lookup from wpce_core.
#include "common/paper_size_map.h"

// ── helpers ──────────────────────────────────────────────────────────

static std::string narrow(const std::wstring& w)
{
  if (w.empty()) return {};
  int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
  if (len <= 0) return {};
  std::string s(static_cast<size_t>(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                      s.data(), len, nullptr, nullptr);
  return s;
}


// ── printer enumeration ──────────────────────────────────────────────

static std::vector<std::wstring> enumerate_all_printer_names()
{
  std::vector<std::wstring> names;
  DWORD bytesNeeded = 0, count = 0;
  EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS,
                nullptr, 2, nullptr, 0, &bytesNeeded, &count);
  if (bytesNeeded == 0) return names;

  std::vector<BYTE> buffer(bytesNeeded);
  if (!EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS,
                     nullptr, 2,
                     buffer.data(),
                     static_cast<DWORD>(buffer.size()),
                     &bytesNeeded, &count))
    return names;

  const auto* info = reinterpret_cast<const PRINTER_INFO_2W*>(buffer.data());
  for (DWORD i = 0; i < count; ++i)
    if (info[i].pPrinterName)
      names.emplace_back(info[i].pPrinterName);
  return names;
}

// ── DeviceCapabilitiesW wrappers ─────────────────────────────────────

/// Get the port name for a printer by opening it and querying PRINTER_INFO_2W.
static std::wstring get_printer_port(const std::wstring& printer_name)
{
  HANDLE hPrinter = nullptr;
  if (!OpenPrinterW(const_cast<LPWSTR>(printer_name.c_str()), &hPrinter, nullptr))
    return {};
  std::wstring port;
  DWORD cbNeeded = 0;
  GetPrinterW(hPrinter, 2, nullptr, 0, &cbNeeded);
  if (cbNeeded > 0)
  {
    std::vector<BYTE> buf(cbNeeded);
    if (GetPrinterW(hPrinter, 2, buf.data(), cbNeeded, &cbNeeded))
    {
      auto* info = reinterpret_cast<PRINTER_INFO_2W*>(buf.data());
      if (info->pPortName)
        port = info->pPortName;
    }
  }
  ClosePrinter(hPrinter);
  return port;
}

/// Parse the fixed-width wchar_t array returned by DC_PAPERNAMES / DC_MEDIATYPENAMES / etc.
/// Each entry is exactly @p stride WCHARs wide, zero-padded (not null-delimited).
static std::vector<std::wstring> parse_fixed_width_array(const WCHAR* buf, int count,
                                                          int stride = 64)
{
  std::vector<std::wstring> result;
  if (!buf || count <= 0) return result;
  result.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i)
  {
    const WCHAR* entry = buf + static_cast<std::ptrdiff_t>(i) * stride;
    size_t len = wcsnlen(entry, static_cast<size_t>(stride));
    if (len == 0) continue;
    result.emplace_back(entry, len);
  }
  return result;
}


// ── dmFields decoding ────────────────────────────────────────────────

struct DmFieldName { DWORD bit; const char* name; };

static const DmFieldName kDmFields[] = {
  { DM_ORIENTATION,   "Orientation" },
  { DM_PAPERSIZE,     "PaperSize" },
  { DM_PAPERLENGTH,   "PaperLength" },
  { DM_PAPERWIDTH,    "PaperWidth" },
  { DM_SCALE,         "Scale" },
  { DM_POSITION,      "Position" },
  { DM_NUP,           "Nup" },
  { DM_COPIES,        "Copies" },
  { DM_DEFAULTSOURCE, "DefaultSource" },
  { DM_PRINTQUALITY,  "PrintQuality" },
  { DM_COLOR,         "Color" },
  { DM_DUPLEX,        "Duplex" },
  { DM_YRESOLUTION,   "YResolution" },
  { DM_TTOPTION,      "TTOption" },
  { DM_COLLATE,       "Collate" },
  { DM_FORMNAME,      "FormName" },
  { DM_LOGPIXELS,     "LogPixels" },
  { DM_BITSPERPEL,    "BitsPerPel" },
  { DM_PELSWIDTH,     "PelsWidth" },
  { DM_PELSHEIGHT,    "PelsHeight" },
  { DM_DISPLAYFLAGS,  "DisplayFlags" },
  { DM_DISPLAYFREQUENCY, "DisplayFrequency" },
  { DM_ICMMETHOD,     "ICMMethod" },
  { DM_ICMINTENT,     "ICMIntent" },
  { DM_MEDIATYPE,     "MediaType" },
  { DM_DITHERTYPE,    "DitherType" },
#ifdef DM_RESERVED1
  { DM_RESERVED1,     "Reserved1" },
#endif
#ifdef DM_RESERVED2
  { DM_RESERVED2,     "Reserved2" },
#endif
#ifdef DM_PANNFIRST
  { DM_PANNFIRST,     "PannFirst" },
#endif
#ifdef DM_PANNLAST
  { DM_PANNLAST,      "PannLast" },
#endif
};

static std::string decode_dm_fields(DWORD fields)
{
  std::string result;
  char hex[16];
  std::snprintf(hex, sizeof(hex), "0x%08lX: ", static_cast<unsigned long>(fields));
  result = hex;

  bool first = true;
  for (const auto& f : kDmFields)
  {
    if (fields & f.bit)
    {
      if (!first) result += ", ";
      result += f.name;
      first = false;
    }
  }
  if (first) result += "(none)";
  return result;
}

// ── orientation / duplex helpers ─────────────────────────────────────

static std::string describe_orientation(int dc_result)
{
  // DC_ORIENTATION returns the number of degrees the device can rotate
  // a portrait page to print landscape. 0 = no rotation (portrait only);
  // 90 = 90° rotation (both portrait & landscape supported).
  // Some drivers return 0 for "both supported" as well — the dmFields
  // DM_ORIENTATION bit is a more reliable indicator of orientation support.
  if (dc_result == 0)
    return "仅纵向 (驱动未报告旋转角度)";
  else if (dc_result == 90)
    return "纵向 + 横向 (支持90°旋转)";
  else
  {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "旋转 %d°", dc_result);
    return std::string(buf);
  }
}

static const char* describe_duplex(int dc_result)
{
  // DC_DUPLEX return: 1 = supported, 0/negative = not supported
  return (dc_result > 0) ? "支持" : "不支持";
}

static const char* describe_collate(int dc_result)
{
  return (dc_result > 0) ? "支持" : "不支持";
}

// ── printer capability struct ────────────────────────────────────────

struct PrinterCapabilities
{
  std::string name;
  std::wstring wname;

  // Basic
  bool color_capable = false;
  DWORD driver_version = 0;

  // Features
  bool duplex_supported = false;
  int max_copies = 0;
  int orientation_mode = -1;      // 0=portrait, 1=landscape, 2=both
  int max_nup = 0;
  bool collate_supported = false;
  DWORD dm_fields = 0;
  int print_rate_ppm = 0;         // DC_PRINTRATEPPM: pages per minute
  int staple_support = 0;         // DC_STAPLE: stapling support

  // Printable area (0.1 mm units)
  LONG max_extent_x = 0, max_extent_y = 0;
  LONG min_extent_x = 0, min_extent_y = 0;

  // Lists
  std::vector<std::string> paper_names;           // canonical + driver names
  std::vector<WORD> paper_ids;                    // DMPAPER_* IDs
  std::vector<std::pair<LONG, LONG>> paper_sizes;  // width, height in 0.1mm
  std::vector<std::string> media_types;
  std::vector<std::string> media_ready;           // DC_MEDIAREADY: currently loaded
  std::vector<std::string> bin_names;
  std::vector<WORD> bin_ids;
  std::vector<std::pair<LONG, LONG>> resolutions;  // x, y in dpi
  std::vector<std::string> personalities;         // DC_PERSONALITY: supported PDLs

  // Default DEVMODE settings (from DocumentPropertiesW)
  std::map<std::string, std::string> device_defaults;

  // Driver info (from GetPrinterDriverW)
  std::string driver_path;
  std::string driver_data_file;
  std::string driver_config_file;
  std::string driver_environment;

  // DEVMODE size analysis
  DWORD devmode_total_size = 0;
  DWORD devmode_standard_size = 0;
  DWORD devmode_private_size = 0;
  std::string devmode_private_hex;
};

// ── detect full capabilities for one printer ─────────────────────────

static PrinterCapabilities detect_one(const std::wstring& printer_name)
{
  PrinterCapabilities cap;
  cap.wname = printer_name;
  cap.name = narrow(printer_name);

  // Get the port name — some drivers require it for accurate queries.
  std::wstring port = get_printer_port(printer_name);
  LPCWSTR pPort = port.empty() ? nullptr : port.c_str();

  // Open the printer to get a full DEVMODE via DocumentPropertiesW.
  // Many drivers (especially IPP Class Driver) only report complete
  // paper lists, duplex, N-up etc. when a valid DEVMODE is passed
  // to DeviceCapabilitiesW.
  HANDLE hPrinter = nullptr;
  std::vector<BYTE> fullDevMode;
  DEVMODEW* pDevMode = nullptr;
  if (OpenPrinterW(const_cast<LPWSTR>(printer_name.c_str()), &hPrinter, nullptr))
  {
    LONG dmSize = DocumentPropertiesW(nullptr, hPrinter,
                                       const_cast<LPWSTR>(printer_name.c_str()),
                                       nullptr, nullptr, 0);
    if (dmSize > 0)
    {
      fullDevMode.resize(static_cast<size_t>(dmSize), 0);
      LONG result = DocumentPropertiesW(nullptr, hPrinter,
                                         const_cast<LPWSTR>(printer_name.c_str()),
                                         reinterpret_cast<PDEVMODEW>(fullDevMode.data()),
                                         nullptr, DM_OUT_BUFFER);
      if (result >= 0)
        pDevMode = reinterpret_cast<DEVMODEW*>(fullDevMode.data());
    }
  }

  // Helper lambda: call DeviceCapabilitiesW with DEVMODE for count.
  auto dc_with_dm = [&](WORD cap_id) -> int {
    return DeviceCapabilitiesW(printer_name.c_str(), pPort, cap_id,
                               nullptr, pDevMode);
  };

  // Helper lambda: call DeviceCapabilitiesW with DEVMODE for buffered data.
  auto dc_buf = [&](WORD cap_id, LPWSTR outBuf) -> int {
    return DeviceCapabilitiesW(printer_name.c_str(), pPort, cap_id,
                               outBuf, pDevMode);
  };

  // ── DC_COLORDEVICE ─────────────────────────────────────────────
  cap.color_capable = (dc_with_dm(DC_COLORDEVICE) > 0);

  // ── DC_DRIVER ─────────────────────────────────────────────────
  cap.driver_version = static_cast<DWORD>(dc_with_dm(DC_DRIVER));

  // ── DC_DUPLEX ─────────────────────────────────────────────────
  cap.duplex_supported = (dc_with_dm(DC_DUPLEX) > 0);

  // ── DC_COPIES ─────────────────────────────────────────────────
  cap.max_copies = dc_with_dm(DC_COPIES);
  if (cap.max_copies < 0) cap.max_copies = 0;

  // ── DC_ORIENTATION ────────────────────────────────────────────
  cap.orientation_mode = dc_with_dm(DC_ORIENTATION);

  // ── DC_NUP ────────────────────────────────────────────────────
  cap.max_nup = dc_with_dm(DC_NUP);
  if (cap.max_nup < 1) cap.max_nup = 1;

  // ── DC_COLLATE ────────────────────────────────────────────────
  cap.collate_supported = (dc_with_dm(DC_COLLATE) > 0);

  // ── DC_FIELDS ─────────────────────────────────────────────────
  cap.dm_fields = static_cast<DWORD>(dc_with_dm(DC_FIELDS));

  // ── DC_PRINTRATEPPM ──────────────────────────────────────────
  cap.print_rate_ppm = dc_with_dm(DC_PRINTRATEPPM);
  if (cap.print_rate_ppm < 0) cap.print_rate_ppm = 0;

  // ── DC_STAPLE ────────────────────────────────────────────────
  cap.staple_support = dc_with_dm(DC_STAPLE);

  // ── DC_MAXEXTENT / DC_MINEXTENT ───────────────────────────────
  {
    LONG r = DeviceCapabilitiesW(printer_name.c_str(), pPort, DC_MAXEXTENT,
                                 nullptr, pDevMode);
    cap.max_extent_x = LOWORD(r);
    cap.max_extent_y = HIWORD(r);

    r = DeviceCapabilitiesW(printer_name.c_str(), pPort, DC_MINEXTENT,
                            nullptr, pDevMode);
    cap.min_extent_x = LOWORD(r);
    cap.min_extent_y = HIWORD(r);
  }

  // ── DC_PAPERNAMES + DC_PAPERS + DC_PAPERSIZE ──────────────────
  {
    int count = dc_with_dm(DC_PAPERNAMES);
    if (count > 0)
    {
      std::vector<WCHAR> nameBuf(static_cast<size_t>(count) * 64, L'\0');
      int actual = dc_buf(DC_PAPERNAMES, nameBuf.data());
      if (actual > 0)
      {
        std::vector<WORD> ids(actual, 0);
        dc_buf(DC_PAPERS, reinterpret_cast<LPWSTR>(ids.data()));

        std::vector<POINT> sizes(actual, {0, 0});
        dc_buf(DC_PAPERSIZE, reinterpret_cast<LPWSTR>(sizes.data()));

        auto names = parse_fixed_width_array(nameBuf.data(), actual, 64);
        for (int i = 0; i < actual && i < static_cast<int>(names.size()); ++i)
        {
          std::string cname;
          std::string_view canonical = wpce::common::paper_size_name(
              static_cast<int16_t>(ids[i]));
          if (!canonical.empty())
            cname = std::string(canonical);
          else
            cname = narrow(names[i]);

          cap.paper_names.push_back(std::move(cname));
          cap.paper_ids.push_back(ids[i]);
          cap.paper_sizes.emplace_back(sizes[i].x, sizes[i].y);
        }
      }
    }
  }

  // ── DC_MEDIATYPENAMES ─────────────────────────────────────────
  {
    int count = dc_with_dm(DC_MEDIATYPENAMES);
    if (count > 0)
    {
      std::vector<WCHAR> buf(static_cast<size_t>(count) * 64, L'\0');
      int actual = dc_buf(DC_MEDIATYPENAMES, buf.data());
      if (actual > 0)
      {
        auto names = parse_fixed_width_array(buf.data(), actual, 64);
        for (const auto& n : names)
          cap.media_types.push_back(narrow(n));
      }
    }
  }

  // ── DC_MEDIAREADY ───────────────────────────────────────────
  {
    int count = dc_with_dm(DC_MEDIAREADY);
    if (count > 0)
    {
      std::vector<WCHAR> buf(static_cast<size_t>(count) * 64, L'\0');
      int actual = dc_buf(DC_MEDIAREADY, buf.data());
      if (actual > 0)
      {
        auto names = parse_fixed_width_array(buf.data(), actual, 64);
        for (const auto& n : names)
          cap.media_ready.push_back(narrow(n));
      }
    }
  }

  // ── DC_BINNAMES + DC_BINS ─────────────────────────────────────
  {
    int count = dc_with_dm(DC_BINNAMES);
    if (count > 0)
    {
      // DC_BINNAMES uses 24-WCHAR fixed-width entries.
      std::vector<WCHAR> nameBuf(static_cast<size_t>(count) * 24, L'\0');
      int actual = dc_buf(DC_BINNAMES, nameBuf.data());
      if (actual > 0)
      {
        std::vector<WORD> ids(actual, 0);
        dc_buf(DC_BINS, reinterpret_cast<LPWSTR>(ids.data()));

        auto names = parse_fixed_width_array(nameBuf.data(), actual, 24);
        cap.bin_names.reserve(static_cast<size_t>(actual));
        for (int i = 0; i < actual && i < static_cast<int>(names.size()); ++i)
        {
          cap.bin_names.push_back(narrow(names[i]));
          cap.bin_ids.push_back(ids[i]);
        }
      }
    }
  }

  // ── DC_PERSONALITY ──────────────────────────────────────────
  {
    int count = dc_with_dm(DC_PERSONALITY);
    if (count > 0)
    {
      std::vector<WCHAR> buf(static_cast<size_t>(count) * 64, L'\0');
      int actual = dc_buf(DC_PERSONALITY, buf.data());
      if (actual > 0)
      {
        auto names = parse_fixed_width_array(buf.data(), actual, 64);
        for (const auto& n : names)
          cap.personalities.push_back(narrow(n));
      }
    }
  }

  // ── DC_ENUMRESOLUTIONS ────────────────────────────────────────
  {
    int count = dc_with_dm(DC_ENUMRESOLUTIONS);
    if (count > 0)
    {
      std::vector<LONG> resBuf(static_cast<size_t>(count) * 2, 0);
      int actual = dc_buf(DC_ENUMRESOLUTIONS,
                          reinterpret_cast<LPWSTR>(resBuf.data()));
      if (actual > 0)
      {
        cap.resolutions.reserve(static_cast<size_t>(actual));
        for (int i = 0; i < actual; ++i)
        {
          cap.resolutions.emplace_back(resBuf[i * 2], resBuf[i * 2 + 1]);
        }
      }
    }
  }

  // ── DEVMODE default settings ──────────────────────────────────
  if (pDevMode)
  {
    cap.dm_fields = pDevMode->dmFields;
    // dmDefault fields (the printer's current defaults)
    char buf[256];
    if (pDevMode->dmFields & DM_PAPERSIZE)
    {
      std::snprintf(buf, sizeof(buf), "DMPAPER_%hd", pDevMode->dmPaperSize);
      cap.device_defaults["默认纸张ID"] = buf;
    }
    if (pDevMode->dmFields & DM_COPIES)
    {
      std::snprintf(buf, sizeof(buf), "%hd", pDevMode->dmCopies);
      cap.device_defaults["默认份数"] = buf;
    }
    if (pDevMode->dmFields & DM_DUPLEX)
    {
      const char* d = "未知";
      switch (pDevMode->dmDuplex) {
        case DMDUP_SIMPLEX:  d = "单面"; break;
        case DMDUP_VERTICAL: d = "双面(长边翻转)"; break;
        case DMDUP_HORIZONTAL: d = "双面(短边翻转)"; break;
      }
      cap.device_defaults["默认双面"] = d;
    }
    if (pDevMode->dmFields & DM_COLOR)
    {
      cap.device_defaults["默认颜色"] = (pDevMode->dmColor == DMCOLOR_COLOR) ? "彩色" : "黑白";
    }
    if (pDevMode->dmFields & DM_ORIENTATION)
    {
      cap.device_defaults["默认方向"] = (pDevMode->dmOrientation == DMORIENT_PORTRAIT) ? "纵向" : "横向";
    }
    if (pDevMode->dmFields & DM_NUP)
    {
      std::snprintf(buf, sizeof(buf), "%d-up", static_cast<int>(pDevMode->dmNup));
      cap.device_defaults["默认多页"] = buf;
    }

    // DEVMODE size analysis and private data dump.
    cap.devmode_standard_size = pDevMode->dmSize;
    cap.devmode_private_size = pDevMode->dmDriverExtra;
    cap.devmode_total_size = pDevMode->dmSize + pDevMode->dmDriverExtra;
    if (pDevMode->dmDriverExtra > 0)
    {
      const BYTE* privateData = reinterpret_cast<const BYTE*>(pDevMode) + pDevMode->dmSize;
      std::ostringstream hex;
      for (WORD i = 0; i < pDevMode->dmDriverExtra; ++i)
      {
        char h[4];
        std::snprintf(h, sizeof(h), "%02X ", privateData[i]);
        hex << h;
        if ((i + 1) % 32 == 0 && (i + 1) < pDevMode->dmDriverExtra)
          hex << "\n    ";
      }
      cap.devmode_private_hex = hex.str();
    }
  }

  // ── GetPrinterDriverW (level 3) ─────────────────────────────
  if (hPrinter)
  {
    DWORD drvNeeded = 0;
    GetPrinterDriverW(hPrinter, nullptr, 3, nullptr, 0, &drvNeeded);
    if (drvNeeded > 0)
    {
      std::vector<BYTE> drvBuf(drvNeeded);
      if (GetPrinterDriverW(hPrinter, nullptr, 3, drvBuf.data(), drvNeeded, &drvNeeded))
      {
        auto* drvInfo = reinterpret_cast<DRIVER_INFO_3W*>(drvBuf.data());
        if (drvInfo->pDriverPath)  cap.driver_path = narrow(drvInfo->pDriverPath);
        if (drvInfo->pDataFile)    cap.driver_data_file = narrow(drvInfo->pDataFile);
        if (drvInfo->pConfigFile)  cap.driver_config_file = narrow(drvInfo->pConfigFile);
        if (drvInfo->pEnvironment) cap.driver_environment = narrow(drvInfo->pEnvironment);
      }
    }
  }

  if (hPrinter) ClosePrinter(hPrinter);
  return cap;
}

// ── print helpers ────────────────────────────────────────────────────

static const char* BOX_H  = "═";
static const char* BOX_V  = "║";
static const char* BOX_TL = "╔";
static const char* BOX_TR = "╗";
static const char* BOX_BL = "╚";
static const char* BOX_BR = "╝";
static const char* SEP    = "──────────────────────────────────────────────────";

static void print_separator()
{
  std::printf("%s\n", SEP);
}

static void print_box_header(const std::string& title)
{
  std::string top = BOX_TL;
  for (size_t i = 0; i < 48; ++i) top += BOX_H;
  top += BOX_TR;
  std::printf("\n%s\n", top.c_str());
  std::printf("%s  打印机: %s\n", BOX_V, title.c_str());
  std::string bottom = BOX_BL;
  for (size_t i = 0; i < 48; ++i) bottom += BOX_H;
  bottom += BOX_BR;
  std::printf("%s\n", bottom.c_str());
}

static void print_field(const char* label, const std::string& value)
{
  constexpr int kLabelWidth = 20;
  int pad = kLabelWidth - static_cast<int>(std::strlen(label));
  if (pad < 1) pad = 1;
  std::printf("  %s", label);
  for (int i = 0; i < pad; ++i) std::putchar(' ');
  std::printf("%s\n", value.c_str());
}

static std::string mm_str(LONG v_01mm)
{
  // Convert 0.1 mm to mm with one decimal.
  char buf[32];
  double mm = static_cast<double>(v_01mm) / 10.0;
  std::snprintf(buf, sizeof(buf), "%.1f", mm);
  return std::string(buf);
}

static void print_capabilities(const PrinterCapabilities& cap)
{
  print_box_header(cap.name);

  // ── basic info ─────────────────────────────────────────────────
  print_field("彩色支持:", cap.color_capable ? "是 (彩色)" : "否 (黑白)");

  {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(cap.driver_version));
    print_field("驱动版本:", buf);
  }

  // ── features ───────────────────────────────────────────────────
  print_field("双面打印:", describe_duplex(cap.duplex_supported ? 1 : 0));

  {
    std::string v = (cap.max_copies > 0)
        ? ("1 - " + std::to_string(cap.max_copies))
        : "未报告";
    print_field("最大份数:", v);
  }

  print_field("方向支持:", describe_orientation(cap.orientation_mode));

  {
    std::string v = (cap.max_nup > 1)
        ? std::to_string(cap.max_nup) + "-up"
        : "未报告 (仅 1-up)";
    print_field("多页(N-up):", v);
  }

  print_field("分页(Collate):", describe_collate(cap.collate_supported ? 1 : 0));

  {
    std::string v = (cap.print_rate_ppm > 0)
        ? std::to_string(cap.print_rate_ppm) + " ppm"
        : "未报告";
    print_field("打印速度:", v);
  }

  print_field("装订(Staple):", (cap.staple_support > 0) ? "支持" : "不支持");

  print_field("dmFields 字段:", decode_dm_fields(cap.dm_fields));

  {
    std::string v = mm_str(cap.min_extent_x) + " x " + mm_str(cap.min_extent_y)
                  + " mm  ~  "
                  + mm_str(cap.max_extent_x) + " x " + mm_str(cap.max_extent_y)
                  + " mm (0.1mm单位)";
    print_field("可打印区域:", v);
  }

  // ── DEVMODE defaults ──────────────────────────────────────────
  if (!cap.device_defaults.empty())
  {
    std::printf("\n  [驱动默认设置] (DocumentPropertiesW)\n");
    for (const auto& [k, v] : cap.device_defaults)
      std::printf("    %-20s: %s\n", k.c_str(), v.c_str());
  }

  // ── paper table ────────────────────────────────────────────────
  if (!cap.paper_names.empty())
  {
    std::printf("\n  [纸张列表] (%zu 种)\n", cap.paper_names.size());
    std::printf("  %-28s %10s %10s   %s\n",
                "纸张名称", "宽度(mm)", "高度(mm)", "DMPAPER ID");
    std::printf("  %-28s %10s %10s   %s\n",
                "────────────", "────────", "────────", "──────────");
    for (size_t i = 0; i < cap.paper_names.size(); ++i)
    {
      std::string w = (i < cap.paper_sizes.size()) ? mm_str(cap.paper_sizes[i].first) : "-";
      std::string h = (i < cap.paper_sizes.size()) ? mm_str(cap.paper_sizes[i].second) : "-";
      std::printf("  %-28s %10s %10s   %u\n",
                  cap.paper_names[i].c_str(),
                  w.c_str(), h.c_str(),
                  static_cast<unsigned>(i < cap.paper_ids.size() ? cap.paper_ids[i] : 0));
    }
  }
  else
  {
    std::printf("\n  [纸张列表] 无 (驱动未报告)\n");
  }

  // ── media types ────────────────────────────────────────────────
  if (!cap.media_types.empty())
  {
    std::printf("\n  [介质类型] (%zu 种)\n", cap.media_types.size());
    size_t col = 0;
    for (const auto& mt : cap.media_types)
    {
      if (col == 0) std::printf("  ");
      std::printf("%-24s", mt.c_str());
      if (++col >= 3) { std::printf("\n"); col = 0; }
    }
    if (col != 0) std::printf("\n");
  }
  else
  {
    std::printf("\n  [介质类型] 无 (驱动未报告)\n");
  }

  // ── paper bins ─────────────────────────────────────────────────
  if (!cap.bin_names.empty())
  {
    std::printf("\n  [纸张来源/纸盒] (%zu 种)\n", cap.bin_names.size());
    for (size_t i = 0; i < cap.bin_names.size(); ++i)
    {
      unsigned bid = (i < cap.bin_ids.size()) ? static_cast<unsigned>(cap.bin_ids[i]) : 0;
      std::printf("    %2zu. %-32s  (DMBIN ID: %u)\n", i + 1, cap.bin_names[i].c_str(), bid);
    }
  }
  else
  {
    std::printf("\n  [纸张来源/纸盒] 无 (驱动未报告)\n");
  }

  // ── resolutions ────────────────────────────────────────────────
  if (!cap.resolutions.empty())
  {
    std::printf("\n  [分辨率列表] (%zu 种)\n", cap.resolutions.size());
    for (const auto& [x, y] : cap.resolutions)
    {
      if (x == y)
        std::printf("    %ld dpi\n", static_cast<long>(x));
      else
        std::printf("    %ld x %ld dpi\n", static_cast<long>(x), static_cast<long>(y));
    }
  }
  else
  {
    std::printf("\n  [分辨率列表] 无 (驱动未报告)\n");
  }

  // ── currently loaded media ────────────────────────────────────
  if (!cap.media_ready.empty())
  {
    std::printf("\n  [当前装载介质] (DC_MEDIAREADY) (%zu 种)\n", cap.media_ready.size());
    for (size_t i = 0; i < cap.media_ready.size(); ++i)
      std::printf("    %2zu. %s\n", i + 1, cap.media_ready[i].c_str());
  }
  else
  {
    std::printf("\n  [当前装载介质] 无 (驱动未报告)\n");
  }

  // ── supported PDLs ────────────────────────────────────────────
  if (!cap.personalities.empty())
  {
    std::printf("\n  [页面描述语言] (DC_PERSONALITY) (%zu 种)\n", cap.personalities.size());
    for (size_t i = 0; i < cap.personalities.size(); ++i)
      std::printf("    %2zu. %s\n", i + 1, cap.personalities[i].c_str());
  }
  else
  {
    std::printf("\n  [页面描述语言] 无 (驱动未报告)\n");
  }

  // ── driver info ───────────────────────────────────────────────
  if (!cap.driver_path.empty())
  {
    std::printf("\n  [驱动信息] (GetPrinterDriverW)\n");
    std::printf("    驱动路径:   %s\n", cap.driver_path.c_str());
    if (!cap.driver_data_file.empty())
      std::printf("    数据文件:   %s\n", cap.driver_data_file.c_str());
    if (!cap.driver_config_file.empty())
      std::printf("    配置文件:   %s\n", cap.driver_config_file.c_str());
    if (!cap.driver_environment.empty())
      std::printf("    驱动环境:   %s\n", cap.driver_environment.c_str());
  }

  // ── DEVMODE size analysis ─────────────────────────────────────
  if (cap.devmode_total_size > 0)
  {
    std::printf("\n  [DEVMODE 大小分析]\n");
    std::printf("    标准区域:   %lu 字节\n", static_cast<unsigned long>(cap.devmode_standard_size));
    std::printf("    私有区域:   %lu 字节\n", static_cast<unsigned long>(cap.devmode_private_size));
    std::printf("    总大小:     %lu 字节\n", static_cast<unsigned long>(cap.devmode_total_size));
    if (!cap.devmode_private_hex.empty())
    {
      std::printf("    私有数据 (hex):\n    %s\n", cap.devmode_private_hex.c_str());
    }
  }

  std::printf("\n");
}

// ── main ─────────────────────────────────────────────────────────────

int main()
{
  // Use Windows wide-character command-line parsing to handle
  // Chinese/CJK printer names correctly regardless of code page.
  int argc = 0;
  LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);

  // Collect requested printer names directly as wide strings.
  std::vector<std::wstring> requested;
  for (int i = 1; i < argc; ++i)
  {
    if (wargv[i] && wargv[i][0])
      requested.emplace_back(wargv[i]);
  }
  if (wargv) LocalFree(wargv);

  // Discover all local printers.
  const auto all = enumerate_all_printer_names();
  if (all.empty())
  {
    std::fprintf(stderr, "未检测到任何打印机。请检查打印服务是否运行。\n");
    return 1;
  }

  std::printf("\n");
  std::printf("  检测到 %zu 台本地打印机\n", all.size());
  print_separator();

  if (requested.empty())
  {
    // Scan all printers.
    for (const auto& name : all)
    {
      print_capabilities(detect_one(name));
      print_separator();
    }
  }
  else
  {
    // Scan only requested printers (case-insensitive match).
    int scanned = 0;
    for (const auto& rn : requested)
    {
      bool found = false;
      std::string rn_narrow = narrow(rn);
      for (const auto& name : all)
      {
        // Case-insensitive comparison.
        if (rn.size() != name.size()) continue;
        bool match = true;
        for (size_t i = 0; i < rn.size(); ++i)
        {
          if (::towlower(rn[i]) != ::towlower(name[i]))
          { match = false; break; }
        }
        if (match)
        {
          print_capabilities(detect_one(name));
          print_separator();
          found = true;
          ++scanned;
          break;
        }
      }
      if (!found)
        std::fprintf(stderr, "警告: 打印机 \"%s\" 未找到，已跳过。\n", rn_narrow.c_str());
    }
    if (scanned == 0)
    {
      std::fprintf(stderr, "没有匹配的打印机。可用打印机列表:\n");
      for (const auto& name : all)
        std::fprintf(stderr, "  - %s\n", narrow(name).c_str());
      return 1;
    }
  }

  std::printf("扫描完成。\n\n");
  return 0;
}

#else  // non-Windows stub
#include <cstdio>
int main() {
  std::printf("此工具仅在 Windows 平台可用。\n");
  return 1;
}
#endif
