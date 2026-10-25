/// @file print_job_diagnostic.cpp
/// Real-time print job diagnostic tool — exhaustive metadata capture.
///
/// Usage:
///   wpce_job_diagnostic.exe "小机器三"              # monitor one printer
///   wpce_job_diagnostic.exe "小机器三" "封面机"      # monitor multiple printers
///   wpce_job_diagnostic.exe --timeout 120           # stop after 120 seconds (default: 300)
///   wpce_job_diagnostic.exe --all                   # monitor all local printers
///
/// For each print job, dumps:
///   - All JOB_INFO_2W fields (including pDatatype, pDriverName)
///   - Full DEVMODE analysis (per-bit dmFields, all values, private hex dump)
///   - Spool file probe (%WINDIR%\System32\spool\PRINTERS\)
///   - PrintTicket JSON probe (XPS filter metadata file)
///   - Field change tracking across lifecycle stages
///   - Summary report at exit

#ifdef _WIN32

#include <windows.h>
#include <winspool.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "common/paper_size_map.h"
#include "parser/spool_file_parser.h"
#include "collector/metadata_collector.h"

// ── helpers ──────────────────────────────────────────────────────────

static std::string narrow(const wchar_t* w)
{
  if (!w || w[0] == L'\0') return {};
  int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 1) return {};
  std::string s(static_cast<size_t>(len) - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
  return s;
}

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

static std::string timestamp_now()
{
  SYSTEMTIME st;
  GetLocalTime(&st);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03d",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
  return buf;
}

static std::string systemtime_to_str(const SYSTEMTIME& st)
{
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond);
  return buf;
}

// ── box-drawing constants ────────────────────────────────────────────

static const char* BOX_H  = "\xe2\x95\x90";  // ═
static const char* BOX_V  = "\xe2\x95\x91";  // ║
static const char* BOX_TL = "\xe2\x95\x94";  // ╔
static const char* BOX_TR = "\xe2\x95\x97";  // ╗
static const char* BOX_BL = "\xe2\x95\x9a";  // ╚
static const char* BOX_BR = "\xe2\x95\x9d";  // ╝
static const char* SEP    = "------------------------------------------------------";

static void print_box(const std::string& title)
{
  std::string top = BOX_TL;
  for (int i = 0; i < 56; ++i) top += BOX_H;
  top += BOX_TR;
  std::printf("\n%s\n", top.c_str());
  std::printf("%s  %s\n", BOX_V, title.c_str());
  std::string bottom = BOX_BL;
  for (int i = 0; i < 56; ++i) bottom += BOX_H;
  bottom += BOX_BR;
  std::printf("%s\n", bottom.c_str());
}

// ── status flags decode ──────────────────────────────────────────────

struct StatusFlagName { DWORD flag; const char* name; };

static const StatusFlagName kStatusFlags[] = {
  { JOB_STATUS_PAUSED,             "PAUSED" },
  { JOB_STATUS_ERROR,              "ERROR" },
  { JOB_STATUS_DELETING,           "DELETING" },
  { JOB_STATUS_SPOOLING,           "SPOOLING" },
  { JOB_STATUS_PRINTING,           "PRINTING" },
  { JOB_STATUS_OFFLINE,            "OFFLINE" },
  { JOB_STATUS_PAPEROUT,           "PAPEROUT" },
  { JOB_STATUS_PRINTED,            "PRINTED" },
  { JOB_STATUS_DELETED,            "DELETED" },
  { JOB_STATUS_BLOCKED_DEVQ,       "BLOCKED_DEVQ" },
  { JOB_STATUS_USER_INTERVENTION,  "USER_INTERVENTION" },
  { JOB_STATUS_RESTART,            "RESTART" },
  { JOB_STATUS_COMPLETE,           "COMPLETE" },
#ifdef JOB_STATUS_RETAINED
  { JOB_STATUS_RETAINED,           "RETAINED" },
#endif
#ifdef JOB_STATUS_RENDERING_LOCALLY
  { JOB_STATUS_RENDERING_LOCALLY,  "RENDERING_LOCALLY" },
#endif
};

static std::string decode_status(DWORD status)
{
  if (status == 0) return "0x00000000 (accepted/queued)";
  char hex[16];
  std::snprintf(hex, sizeof(hex), "0x%08lX ", static_cast<unsigned long>(status));
  std::string result = hex;
  bool first = true;
  for (const auto& f : kStatusFlags)
  {
    if (status & f.flag)
    {
      if (!first) result += " | ";
      result += f.name;
      first = false;
    }
  }
  return result;
}

// ── dmFields decoding ────────────────────────────────────────────────

struct DmFieldEntry { DWORD bit; const char* name; };

static const DmFieldEntry kDmFields[] = {
  { DM_ORIENTATION,      "Orientation" },
  { DM_PAPERSIZE,        "PaperSize" },
  { DM_PAPERLENGTH,      "PaperLength" },
  { DM_PAPERWIDTH,       "PaperWidth" },
  { DM_SCALE,            "Scale" },
  { DM_POSITION,         "Position" },
  { DM_NUP,              "Nup" },
  { DM_COPIES,           "Copies" },
  { DM_DEFAULTSOURCE,    "DefaultSource" },
  { DM_PRINTQUALITY,     "PrintQuality" },
  { DM_COLOR,            "Color" },
  { DM_DUPLEX,           "Duplex" },
  { DM_YRESOLUTION,      "YResolution" },
  { DM_TTOPTION,         "TTOption" },
  { DM_COLLATE,          "Collate" },
  { DM_FORMNAME,         "FormName" },
  { DM_LOGPIXELS,        "LogPixels" },
  { DM_BITSPERPEL,       "BitsPerPel" },
  { DM_PELSWIDTH,        "PelsWidth" },
  { DM_PELSHEIGHT,       "PelsHeight" },
  { DM_DISPLAYFLAGS,     "DisplayFlags" },
  { DM_DISPLAYFREQUENCY, "DisplayFrequency" },
  { DM_ICMMETHOD,        "ICMMethod" },
  { DM_ICMINTENT,        "ICMIntent" },
  { DM_MEDIATYPE,        "MediaType" },
  { DM_DITHERTYPE,       "DitherType" },
};

// ── diagnostic state per job ─────────────────────────────────────────

struct DiagnosticCapture
{
  std::string timestamp;
  DWORD status = 0;
  DWORD total_pages = 0;
  DWORD pages_printed = 0;
};

struct DiagnosticJobState
{
  DWORD job_id = 0;
  std::string printer_name;
  std::string document_name;
  bool full_dump_done = false;
  std::vector<DiagnosticCapture> captures;

  bool spool_devmode_found = false;
  int spool_copies = -1;
};

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
                     nullptr, 2, buffer.data(),
                     static_cast<DWORD>(buffer.size()),
                     &bytesNeeded, &count))
    return names;

  const auto* info = reinterpret_cast<const PRINTER_INFO_2W*>(buffer.data());
  for (DWORD i = 0; i < count; ++i)
    if (info[i].pPrinterName)
      names.emplace_back(info[i].pPrinterName);
  return names;
}

// ── dump JOB_INFO_2W ─────────────────────────────────────────────────

static void dump_job_info_2w(const JOB_INFO_2W& job)
{
  std::printf("  -- JOB_INFO_2W 原始字段 --\n");
  std::printf("    %-20s: %lu\n",          "JobId",          static_cast<unsigned long>(job.JobId));
  std::printf("    %-20s: %s\n",           "pPrinterName",   narrow(job.pPrinterName).c_str());
  std::printf("    %-20s: %s\n",           "pMachineName",   narrow(job.pMachineName).c_str());
  std::printf("    %-20s: %s\n",           "pUserName",      narrow(job.pUserName).c_str());
  std::printf("    %-20s: %s\n",           "pDocument",      narrow(job.pDocument).c_str());
  std::printf("    %-20s: %s\n",           "pNotifyName",    narrow(job.pNotifyName).c_str());
  std::printf("    %-20s: %s\n",           "pDatatype",      narrow(job.pDatatype).c_str());
  std::printf("    %-20s: %s\n",           "pPrintProcessor",narrow(job.pPrintProcessor).c_str());
  std::printf("    %-20s: %s\n",           "pParameters",    narrow(job.pParameters).c_str());
  std::printf("    %-20s: %s\n",           "pDriverName",    narrow(job.pDriverName).c_str());
  std::printf("    %-20s: %s\n",           "pStatus",        narrow(job.pStatus).c_str());
  std::printf("    %-20s: %s\n",           "Status",         decode_status(job.Status).c_str());
  std::printf("    %-20s: %lu\n",          "Priority",       static_cast<unsigned long>(job.Priority));
  std::printf("    %-20s: %lu\n",          "Position",       static_cast<unsigned long>(job.Position));
  std::printf("    %-20s: %lu\n",          "TotalPages",     static_cast<unsigned long>(job.TotalPages));
  std::printf("    %-20s: %lu\n",          "PagesPrinted",   static_cast<unsigned long>(job.PagesPrinted));
  std::printf("    %-20s: %lu bytes\n",    "Size",           static_cast<unsigned long>(job.Size));
  std::printf("    %-20s: %s\n",           "Submitted",      systemtime_to_str(job.Submitted).c_str());
  std::printf("    %-20s: %lu sec\n",      "Time",           static_cast<unsigned long>(job.Time));
  std::printf("    %-20s: %lu / %lu\n",    "StartTime/Until",
              static_cast<unsigned long>(job.StartTime),
              static_cast<unsigned long>(job.UntilTime));
  std::printf("    %-20s: %s\n",           "pDevMode",       job.pDevMode ? "非空" : "*** NULL ***");
  std::printf("    %-20s: %s\n",           "pSecurityDesc",  job.pSecurityDescriptor ? "非空" : "NULL");
}

// ── dump DEVMODE ─────────────────────────────────────────────────────

static void dump_devmode(const DEVMODEW* dm, bool is_fallback)
{
  if (!dm)
  {
    std::printf("  -- DEVMODE 分析 --\n");
    std::printf("    pDevMode 为 NULL，无法分析\n");
    return;
  }

  std::printf("  -- DEVMODE 分析 %s--\n", is_fallback ? "(默认值, 非作业级) " : "");

  // Size info
  std::printf("    dmSize:          %u 字节 (标准区域)\n", dm->dmSize);
  std::printf("    dmDriverExtra:   %u 字节 (私有区域)\n", dm->dmDriverExtra);
  std::printf("    dmSpecVersion:   0x%04X\n", dm->dmSpecVersion);
  std::printf("    dmDriverVersion: 0x%04X\n", dm->dmDriverVersion);

  // Device name
  {
    char devName[64] = {};
    WideCharToMultiByte(CP_UTF8, 0, dm->dmDeviceName, CCHDEVICENAME,
                        devName, sizeof(devName) - 1, nullptr, nullptr);
    std::printf("    dmDeviceName:    %s\n", devName);
  }

  // dmFields bitmask
  std::printf("\n    dmFields:        0x%08lX\n", static_cast<unsigned long>(dm->dmFields));

  // Per-field analysis: show whether each bit is set, and the value
  struct FieldCheck {
    DWORD bit;
    const char* name;
    const char* desc;
    std::string value;
  };

  std::vector<FieldCheck> checks;

  auto add_check = [&](DWORD bit, const char* name, const char* desc, std::string val) {
    checks.push_back({bit, name, desc, std::move(val)});
  };

  // Color
  {
    std::string v;
    switch (dm->dmColor) {
      case DMCOLOR_MONOCHROME: v = "1 (MONOCHROME)"; break;
      case DMCOLOR_COLOR:      v = "2 (COLOR)"; break;
      default: v = std::to_string(dm->dmColor) + " (未知)"; break;
    }
    add_check(DM_COLOR, "DM_COLOR", "颜色", std::move(v));
  }

  // Copies
  add_check(DM_COPIES, "DM_COPIES", "份数", std::to_string(dm->dmCopies));

  // Duplex
  {
    std::string v;
    switch (dm->dmDuplex) {
      case DMDUP_SIMPLEX:    v = "1 (SIMPLEX/单面)"; break;
      case DMDUP_VERTICAL:   v = "2 (VERTICAL/长边)"; break;
      case DMDUP_HORIZONTAL: v = "3 (HORIZONTAL/短边)"; break;
      default: v = std::to_string(dm->dmDuplex) + " (未知)"; break;
    }
    add_check(DM_DUPLEX, "DM_DUPLEX", "双面", std::move(v));
  }

  // Paper size
  {
    std::string v = "DMPAPER_" + std::to_string(dm->dmPaperSize);
    auto canonical = wpce::common::paper_size_name(dm->dmPaperSize);
    if (!canonical.empty())
      v += " (" + std::string(canonical) + ")";
    add_check(DM_PAPERSIZE, "DM_PAPERSIZE", "纸张", std::move(v));
  }

  // Paper length/width (0.1mm)
  add_check(DM_PAPERLENGTH, "DM_PAPERLENGTH", "纸张长度",
            std::to_string(dm->dmPaperLength) + " (0.1mm = " +
            std::to_string(dm->dmPaperLength / 10) + "mm)");
  add_check(DM_PAPERWIDTH, "DM_PAPERWIDTH", "纸张宽度",
            std::to_string(dm->dmPaperWidth) + " (0.1mm = " +
            std::to_string(dm->dmPaperWidth / 10) + "mm)");

  // Orientation
  {
    std::string v;
    switch (dm->dmOrientation) {
      case DMORIENT_PORTRAIT:  v = "1 (PORTRAIT/纵向)"; break;
      case DMORIENT_LANDSCAPE: v = "2 (LANDSCAPE/横向)"; break;
      default: v = std::to_string(dm->dmOrientation) + " (未知)"; break;
    }
    add_check(DM_ORIENTATION, "DM_ORIENTATION", "方向", std::move(v));
  }

  // N-up
  add_check(DM_NUP, "DM_NUP", "多页", std::to_string(dm->dmNup) + "-up");

  // Collate
  {
    std::string v = (dm->dmCollate == DMCOLLATE_TRUE) ? "1 (TRUE/分页)"
                  : (dm->dmCollate == DMCOLLATE_FALSE) ? "0 (FALSE/不分页)"
                  : std::to_string(dm->dmCollate) + " (未知)";
    add_check(DM_COLLATE, "DM_COLLATE", "分页", std::move(v));
  }

  // Print quality
  {
    std::string v;
    if (dm->dmPrintQuality > 0)
      v = std::to_string(dm->dmPrintQuality) + " dpi";
    else {
      switch (dm->dmPrintQuality) {
        case DMRES_HIGH:   v = "-4 (HIGH)"; break;
        case DMRES_MEDIUM: v = "-3 (MEDIUM)"; break;
        case DMRES_LOW:    v = "-2 (LOW)"; break;
        case DMRES_DRAFT:  v = "-1 (DRAFT)"; break;
        default: v = std::to_string(dm->dmPrintQuality); break;
      }
    }
    add_check(DM_PRINTQUALITY, "DM_PRINTQUALITY", "质量", std::move(v));
  }

  // Y resolution
  add_check(DM_YRESOLUTION, "DM_YRESOLUTION", "Y分辨率",
            std::to_string(dm->dmYResolution) + " dpi");

  // Scale
  add_check(DM_SCALE, "DM_SCALE", "缩放", std::to_string(dm->dmScale) + "%");

  // Default source (bin)
  add_check(DM_DEFAULTSOURCE, "DM_DEFAULTSOURCE", "纸盒",
            "DMBIN_" + std::to_string(dm->dmDefaultSource));

  // Media type
  {
    std::string v;
    switch (dm->dmMediaType) {
      case DMMEDIA_STANDARD:     v = "1 (STANDARD/普通)"; break;
      case DMMEDIA_TRANSPARENCY: v = "2 (TRANSPARENCY/透明)"; break;
      case DMMEDIA_GLOSSY:       v = "3 (GLOSSY/光泽)"; break;
      default: v = std::to_string(dm->dmMediaType); break;
    }
    add_check(DM_MEDIATYPE, "DM_MEDIATYPE", "介质", std::move(v));
  }

  // TT option
  {
    std::string v;
    switch (dm->dmTTOption) {
      case DMTT_BITMAP:           v = "1 (BITMAP)"; break;
      case DMTT_DOWNLOAD:         v = "2 (DOWNLOAD)"; break;
      case DMTT_SUBDEV:           v = "3 (SUBDEV)"; break;
      case DMTT_DOWNLOAD_OUTLINE: v = "4 (DOWNLOAD_OUTLINE)"; break;
      default: v = std::to_string(dm->dmTTOption); break;
    }
    add_check(DM_TTOPTION, "DM_TTOPTION", "TrueType", std::move(v));
  }

  // Form name
  {
    char formName[64] = {};
    WideCharToMultiByte(CP_UTF8, 0, dm->dmFormName, CCHFORMNAME,
                        formName, sizeof(formName) - 1, nullptr, nullptr);
    add_check(DM_FORMNAME, "DM_FORMNAME", "表单名", std::string(formName));
  }

  // Print table
  std::printf("    %-18s %-8s %-8s %s\n", "字段", "已设置", "描述", "值");
  std::printf("    %-18s %-8s %-8s %s\n", "──────────────", "──────", "──────", "──────────────");
  for (const auto& c : checks)
  {
    bool set = (dm->dmFields & c.bit) != 0;
    std::printf("    %-18s %-8s %-8s %s\n",
                c.name,
                set ? "YES" : "no",
                c.desc,
                c.value.c_str());
  }

  // Private data hex dump
  if (dm->dmDriverExtra > 0)
  {
    std::printf("\n    DEVMODE 私有区域 (%u 字节):\n    ", dm->dmDriverExtra);
    const BYTE* p = reinterpret_cast<const BYTE*>(dm) + dm->dmSize;
    for (WORD i = 0; i < dm->dmDriverExtra; ++i)
    {
      std::printf("%02X ", p[i]);
      if ((i + 1) % 32 == 0 && (i + 1) < dm->dmDriverExtra)
        std::printf("\n    ");
    }
    std::printf("\n");
  }
}

// ── spool file probe ─────────────────────────────────────────────────

static void check_spool_files(DWORD job_id, const std::string& printer_name,
                              DiagnosticJobState& state)
{
  std::printf("  -- 后台打印文件检查 --\n");

  // Build spool directory path.
  wchar_t windir[MAX_PATH] = {};
  GetWindowsDirectoryW(windir, MAX_PATH);
  std::filesystem::path spool_dir = std::filesystem::path(windir)
      / L"System32" / L"spool" / L"PRINTERS";

  std::printf("    后台目录:   %s\n", spool_dir.string().c_str());

  std::error_code ec;
  if (!std::filesystem::exists(spool_dir, ec))
  {
    std::printf("    状态:       目录不存在或无权限访问\n");
    return;
  }

  int spl_count = 0, shd_count = 0;
  std::string matched_spl;

  try
  {
    for (const auto& entry : std::filesystem::directory_iterator(spool_dir, ec))
    {
      if (!entry.is_regular_file()) continue;
      auto ext = entry.path().extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(),
                     [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });

      if (ext == ".spl")
      {
        ++spl_count;
        auto size = entry.file_size(ec);
        auto ftime = entry.last_write_time(ec);
        std::printf("    SPL 文件:   %s (%llu 字节)\n",
                    entry.path().filename().string().c_str(),
                    static_cast<unsigned long long>(size));

        // Try to read first 8 bytes to detect format.
        HANDLE hFile = CreateFileW(entry.path().c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
        if (hFile != INVALID_HANDLE_VALUE)
        {
          BYTE header[16] = {};
          DWORD bytesRead = 0;
          if (ReadFile(hFile, header, sizeof(header), &bytesRead, nullptr) && bytesRead >= 8)
          {
            DWORD magic = *reinterpret_cast<DWORD*>(header);
            const char* format = "未知";
            if (magic == 0x00010000 || magic == 0x00000001)
              format = "EMF Spool";
            else if (header[0] == 0xD0 && header[1] == 0xCF)
              format = "OLE/Compound (可能为 RAW)";
            else if (header[0] == '%' && header[1] == '!')
              format = "PostScript (RAW)";
            else if (header[0] == 0x1B)
              format = "ESC/P 或 PCL (RAW)";
            else if (header[0] == 'P' && header[1] == 'K')
              format = "ZIP/XPS Package";

            std::printf("    SPL 格式:   %s (magic: %02X %02X %02X %02X)\n",
                        format, header[0], header[1], header[2], header[3]);

            // For EMF spool: try to count pages (EMRI_METAFILE_DATA records).
            if (magic == 0x00010000 || magic == 0x00000001)
            {
              LARGE_INTEGER fileSize;
              GetFileSizeEx(hFile, &fileSize);
              if (fileSize.QuadPart > 0 && fileSize.QuadPart < 512 * 1024 * 1024)
              {
                SetFilePointer(hFile, 0, nullptr, FILE_BEGIN);
                std::vector<BYTE> data(static_cast<size_t>(fileSize.QuadPart));
                DWORD totalRead = 0;
                if (ReadFile(hFile, data.data(), static_cast<DWORD>(data.size()), &totalRead, nullptr))
                {
                  int page_count = 0;
                  int devmode_count = 0;
                  size_t offset = 0;
                  while (offset + 8 <= totalRead)
                  {
                    DWORD recType = *reinterpret_cast<DWORD*>(data.data() + offset);
                    DWORD recSize = *reinterpret_cast<DWORD*>(data.data() + offset + 4);

                    if (recSize == 0 || offset + 8 + recSize > totalRead) break;

                    if (recType == 0x00000001) ++page_count;     // EMRI_METAFILE_DATA
                    if (recType == 0x00000002) ++page_count;     // EMRI_METAFILE_EXT
                    if (recType == 0x00000009) ++devmode_count;  // EMRI_DEVMODE

                    offset += 8 + recSize;
                    // Align to 4 bytes
                    offset = (offset + 3) & ~static_cast<size_t>(3);
                  }
                  std::printf("    EMF 页数:   %d 页 (%d 个 DEVMODE 记录)\n",
                              page_count, devmode_count);
                }
              }
            }
          }
          CloseHandle(hFile);
        }
      }
      else if (ext == ".shd")
      {
        ++shd_count;
        std::printf("    SHD 文件:   %s\n", entry.path().filename().string().c_str());
      }
    }
  }
  catch (const std::exception& e)
  {
    std::printf("    错误:       遍历失败 (%s)\n", e.what());
  }

  if (spl_count == 0 && shd_count == 0)
    std::printf("    状态:       未发现活跃的 SPL/SHD 文件 (作业可能已完成或无权限)\n");
  else
    std::printf("    合计:       %d 个 SPL, %d 个 SHD 文件\n", spl_count, shd_count);

  // ── Deep spool DEVMODE analysis via parser module ──────────────────
  std::printf("\n  -- Spool DEVMODE 深度解析 --\n");
  auto spool_result = wpce::parser::find_and_parse_spool_file(job_id, printer_name);
  if (!spool_result.has_value())
  {
    std::printf("    状态:       未找到匹配的 SPL 文件 (job_id=%lu)\n",
                static_cast<unsigned long>(job_id));
  }
  else
  {
    const auto& info = spool_result.value();
    const char* fmt_name = "未知";
    switch (info.format)
    {
      case wpce::parser::SpoolFileInfo::Format::EMF:        fmt_name = "EMF Spool"; break;
      case wpce::parser::SpoolFileInfo::Format::RAW:        fmt_name = "RAW"; break;
      case wpce::parser::SpoolFileInfo::Format::PostScript: fmt_name = "PostScript"; break;
      case wpce::parser::SpoolFileInfo::Format::PCL:        fmt_name = "PCL"; break;
      case wpce::parser::SpoolFileInfo::Format::XPS:        fmt_name = "XPS"; break;
      default: break;
    }
    std::printf("    SPL 路径:   %s\n", info.path.string().c_str());
    std::printf("    文件大小:   %llu 字节\n", static_cast<unsigned long long>(info.file_size));
    std::printf("    检测格式:   %s\n", fmt_name);
    std::printf("    EMF 页数:   %d\n", info.emf_page_count);
    std::printf("    DEVMODE:    %s\n", info.has_devmode ? "已提取" : "未找到");

    if (info.has_devmode)
    {
      state.spool_devmode_found = true;
      if (info.dm_copies > 0)
        state.spool_copies = info.dm_copies;

      std::printf("    dm_fields:  0x%08X\n", info.dm_fields);
      std::printf("    ┌─────────────────────────────────────────────────┐\n");
      std::printf("    │ Spool DEVMODE 字段值（与 JOB_INFO_2W 对比用）  │\n");
      std::printf("    ├──────────────┬──────────────┬───────────────────┤\n");
      std::printf("    │ 字段         │ 原始值       │ 解释              │\n");
      std::printf("    ├──────────────┼──────────────┼───────────────────┤\n");

      // Copies
      if (info.dm_copies > 0)
      {
        std::printf("    │ dmCopies     │ %-12d │ %-17s │\n",
                    info.dm_copies,
                    std::to_string(info.dm_copies).c_str());
      }
      else
      {
        std::printf("    │ dmCopies     │ %-12s │ %-17s │\n", "-", "未设置");
      }

      // Color
      if (info.dm_color > 0)
      {
        auto name = wpce::collector::color_name(info.dm_color);
        std::printf("    │ dmColor      │ %-12d │ %-17.*s │\n",
                    info.dm_color,
                    static_cast<int>(name.size()), name.data());
      }
      else
      {
        std::printf("    │ dmColor      │ %-12s │ %-17s │\n", "-", "未设置");
      }

      // Duplex
      if (info.dm_duplex > 0)
      {
        auto name = wpce::collector::duplex_name(info.dm_duplex);
        std::printf("    │ dmDuplex     │ %-12d │ %-17.*s │\n",
                    info.dm_duplex,
                    static_cast<int>(name.size()), name.data());
      }
      else
      {
        std::printf("    │ dmDuplex     │ %-12s │ %-17s │\n", "-", "未设置");
      }

      // Paper size
      if (info.dm_paper_size > 0)
      {
        auto name = wpce::common::paper_size_name(info.dm_paper_size);
        std::printf("    │ dmPaperSize  │ %-12d │ %-17.*s │\n",
                    info.dm_paper_size,
                    static_cast<int>(name.size()), name.data());
      }
      else
      {
        std::printf("    │ dmPaperSize  │ %-12s │ %-17s │\n", "-", "未设置");
      }

      // Orientation
      if (info.dm_orientation > 0)
      {
        auto name = wpce::collector::orientation_name(info.dm_orientation);
        std::printf("    │ dmOrient     │ %-12d │ %-17.*s │\n",
                    info.dm_orientation,
                    static_cast<int>(name.size()), name.data());
      }
      else
      {
        std::printf("    │ dmOrient     │ %-12s │ %-17s │\n", "-", "未设置");
      }

      // N-up
      if (info.dm_nup > 0)
      {
        auto name = wpce::collector::n_up_name(info.dm_nup);
        std::printf("    │ dmNup        │ %-12d │ %-17.*s │\n",
                    info.dm_nup,
                    static_cast<int>(name.size()), name.data());
      }
      else
      {
        std::printf("    │ dmNup        │ %-12s │ %-17s │\n", "-", "未设置");
      }

      // Collate
      if (info.dm_collate >= 0)
      {
        auto name = wpce::collector::collate_name(info.dm_collate);
        std::printf("    │ dmCollate    │ %-12d │ %-17.*s │\n",
                    info.dm_collate,
                    static_cast<int>(name.size()), name.data());
      }
      else
      {
        std::printf("    │ dmCollate    │ %-12s │ %-17s │\n", "-", "未设置");
      }

      // Print quality
      if (info.dm_print_quality != 0)
      {
        auto name = wpce::collector::print_quality_name(info.dm_print_quality);
        std::printf("    │ dmQuality    │ %-12d │ %-17.*s │\n",
                    info.dm_print_quality,
                    static_cast<int>(name.size()), name.data());
      }
      else
      {
        std::printf("    │ dmQuality    │ %-12s │ %-17s │\n", "-", "未设置");
      }

      std::printf("    └──────────────┴──────────────┴───────────────────┘\n");

      // Highlight copies discrepancy for Word debugging
      if (info.dm_copies > 1)
      {
        std::printf("    ★ Spool DEVMODE 份数 = %d — 若 JOB_INFO_2W 显示 1，\n"
                    "      说明应用 (如 Word) 在 spool 阶段正确设置了份数。\n",
                    info.dm_copies);
      }
    }
  }
}

// ── PrintTicket JSON probe ───────────────────────────────────────────

static void check_print_ticket_json(const std::string& printer_name, DWORD job_id)
{
  std::printf("  -- PrintTicket (XPS Filter) 检查 --\n");

  // Build expected filename pattern: job-<printer>-<jobid>-pt-metadata.json
  std::string expected_prefix = "job-" + printer_name + "-" + std::to_string(job_id);

  // Search in common locations.
  std::vector<std::filesystem::path> search_dirs;

  // WPCE_OUTPUT_DIR environment variable
  const char* output_dir = std::getenv("WPCE_OUTPUT_DIR");
  if (output_dir && output_dir[0])
    search_dirs.emplace_back(output_dir);

  // %TEMP%
  wchar_t tempDir[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tempDir);
  search_dirs.emplace_back(tempDir);

  // Current directory
  search_dirs.emplace_back(".");

  // Exe directory
  wchar_t exePath[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exePath, MAX_PATH);
  search_dirs.emplace_back(std::filesystem::path(exePath).parent_path());

  bool found = false;
  for (const auto& dir : search_dirs)
  {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) continue;

    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
    {
      if (!entry.is_regular_file()) continue;
      auto fname = entry.path().filename().string();
      if (fname.find(expected_prefix) != std::string::npos &&
          fname.find("pt-metadata") != std::string::npos)
      {
        std::printf("    找到:       %s\n", entry.path().string().c_str());
        auto size = entry.file_size(ec);
        std::printf("    大小:       %llu 字节\n", static_cast<unsigned long long>(size));
        found = true;

        // Read first 512 bytes to show content.
        HANDLE hFile = CreateFileW(entry.path().c_str(), GENERIC_READ,
                                    FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
        if (hFile != INVALID_HANDLE_VALUE)
        {
          char buf[513] = {};
          DWORD bytesRead = 0;
          ReadFile(hFile, buf, 512, &bytesRead, nullptr);
          if (bytesRead > 0)
          {
            buf[bytesRead] = '\0';
            std::printf("    内容预览:\n      %s\n", buf);
          }
          CloseHandle(hFile);
        }
      }
    }
  }

  if (!found)
    std::printf("    状态:       未找到 (XPS Filter 可能未加载)\n");
}

// ── print change history for a job ───────────────────────────────────

static void print_change_history(const DiagnosticJobState& state)
{
  if (state.captures.size() <= 1) return;

  std::printf("  -- 字段变更历史 --\n");
  std::printf("    %-8s %-28s %-12s %-12s %-12s\n",
              "捕获#", "时间戳", "Status", "TotalPages", "PgPrinted");
  std::printf("    %-8s %-28s %-12s %-12s %-12s\n",
              "------", "--------------------------", "----------", "----------", "----------");

  for (size_t i = 0; i < state.captures.size(); ++i)
  {
    const auto& c = state.captures[i];
    char status_hex[16];
    std::snprintf(status_hex, sizeof(status_hex), "0x%08lX",
                  static_cast<unsigned long>(c.status));
    std::printf("    %-8zu %-28s %-12s %-12lu %-12lu\n",
                i + 1, c.timestamp.c_str(), status_hex,
                static_cast<unsigned long>(c.total_pages),
                static_cast<unsigned long>(c.pages_printed));
  }
}

// ── main monitoring logic ────────────────────────────────────────────

struct PrinterMonitor
{
  std::wstring name;
  HANDLE printer_handle = nullptr;
  HANDLE notification_handle = nullptr;
};

static volatile bool g_running = true;

static BOOL WINAPI console_handler(DWORD)
{
  g_running = false;
  return TRUE;
}

int main()
{
  SetConsoleCtrlHandler(console_handler, TRUE);
  SetConsoleOutputCP(CP_UTF8);

  int argc = 0;
  LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);

  std::vector<std::wstring> requested;
  int timeout_seconds = 300;
  bool scan_all = false;

  for (int i = 1; i < argc; ++i)
  {
    if (!wargv[i]) continue;
    std::wstring arg = wargv[i];
    if (arg == L"--timeout" && i + 1 < argc)
    {
      timeout_seconds = static_cast<int>(wcstol(wargv[++i], nullptr, 10));
      if (timeout_seconds <= 0) timeout_seconds = 300;
    }
    else if (arg == L"--all")
    {
      scan_all = true;
    }
    else if (arg[0] != L'-')
    {
      requested.push_back(arg);
    }
  }
  if (wargv) LocalFree(wargv);

  // Discover printers.
  const auto all_printers = enumerate_all_printer_names();
  if (all_printers.empty())
  {
    std::fprintf(stderr, "未检测到任何打印机。\n");
    return 1;
  }

  // Resolve target printers.
  std::vector<std::wstring> targets;
  if (scan_all || requested.empty())
  {
    targets = all_printers;
  }
  else
  {
    for (const auto& rn : requested)
    {
      bool found = false;
      for (const auto& pn : all_printers)
      {
        if (rn.size() != pn.size()) continue;
        bool match = true;
        for (size_t i = 0; i < rn.size(); ++i)
        {
          if (::towlower(rn[i]) != ::towlower(pn[i]))
          { match = false; break; }
        }
        if (match) { targets.push_back(pn); found = true; break; }
      }
      if (!found)
        std::fprintf(stderr, "警告: 打印机 \"%s\" 未找到。\n", narrow(rn).c_str());
    }
  }

  if (targets.empty())
  {
    std::fprintf(stderr, "没有可监听的打印机。可用:\n");
    for (const auto& p : all_printers)
      std::fprintf(stderr, "  - %s\n", narrow(p).c_str());
    return 1;
  }

  // Set up printer monitors.
  std::vector<PrinterMonitor> monitors;
  std::vector<HANDLE> wait_handles;

  WORD notify_fields[] = {
    JOB_NOTIFY_FIELD_PRINTER_NAME,
    JOB_NOTIFY_FIELD_USER_NAME,
    JOB_NOTIFY_FIELD_STATUS,
    JOB_NOTIFY_FIELD_DOCUMENT,
    JOB_NOTIFY_FIELD_SUBMITTED,
    JOB_NOTIFY_FIELD_TOTAL_PAGES,
    JOB_NOTIFY_FIELD_PAGES_PRINTED,
    JOB_NOTIFY_FIELD_TOTAL_BYTES,
    JOB_NOTIFY_FIELD_TIME,
  };

  PRINTER_NOTIFY_OPTIONS_TYPE notify_type = {};
  notify_type.Type = JOB_NOTIFY_TYPE;
  notify_type.Count = static_cast<DWORD>(std::size(notify_fields));
  notify_type.pFields = notify_fields;

  PRINTER_NOTIFY_OPTIONS notify_options = {};
  notify_options.Version = 2;
  notify_options.Count = 1;
  notify_options.pTypes = &notify_type;

  for (const auto& pname : targets)
  {
    PrinterMonitor m;
    m.name = pname;

    if (!OpenPrinterW(const_cast<LPWSTR>(pname.c_str()), &m.printer_handle, nullptr))
    {
      std::fprintf(stderr, "无法打开打印机 \"%s\" (err=%lu)\n",
                   narrow(pname).c_str(), GetLastError());
      continue;
    }

    m.notification_handle = FindFirstPrinterChangeNotification(
        m.printer_handle,
        PRINTER_CHANGE_ADD_JOB | PRINTER_CHANGE_SET_JOB |
        PRINTER_CHANGE_WRITE_JOB | PRINTER_CHANGE_DELETE_JOB,
        0, &notify_options);

    if (!m.notification_handle || m.notification_handle == INVALID_HANDLE_VALUE)
    {
      std::fprintf(stderr, "无法设置通知 \"%s\" (err=%lu)\n",
                   narrow(pname).c_str(), GetLastError());
      ClosePrinter(m.printer_handle);
      continue;
    }

    wait_handles.push_back(m.notification_handle);
    monitors.push_back(m);
  }

  if (monitors.empty())
  {
    std::fprintf(stderr, "没有成功初始化的打印机监听器。\n");
    return 1;
  }

  // Print startup info.
  std::printf("\n%s\n", SEP);
  std::printf("  打印作业诊断工具 v1.0\n");
  std::printf("  监听 %zu 台打印机，超时 %d 秒\n", monitors.size(), timeout_seconds);
  std::printf("  按 Ctrl+C 提前退出\n");
  std::printf("%s\n", SEP);
  for (const auto& m : monitors)
    std::printf("  - %s\n", narrow(m.name).c_str());
  std::printf("%s\n\n", SEP);
  std::printf("  等待打印作业...\n\n");

  // Tracking state.
  std::map<DWORD, DiagnosticJobState> job_states;   // job_id → state
  int total_jobs_observed = 0;

  auto start_time = std::chrono::steady_clock::now();
  auto last_rescan = start_time;

  // ── main event loop ────────────────────────────────────────────
  while (g_running)
  {
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start_time).count();
    if (elapsed >= timeout_seconds) break;

    DWORD result = WaitForMultipleObjects(
        static_cast<DWORD>(wait_handles.size()),
        wait_handles.data(), FALSE, 500);

    auto now = std::chrono::steady_clock::now();

    // Process notification.
    if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + wait_handles.size())
    {
      size_t idx = result - WAIT_OBJECT_0;
      auto& mon = monitors[idx];

      DWORD change_flags = 0;
      PRINTER_NOTIFY_INFO* pInfo = nullptr;
      if (!FindNextPrinterChangeNotification(
              mon.notification_handle, &change_flags, nullptr,
              reinterpret_cast<LPVOID*>(&pInfo)))
      {
        std::fprintf(stderr, "[%s] FindNextPrinterChangeNotification 失败 (err=%lu)\n",
                     narrow(mon.name).c_str(), GetLastError());
        continue;
      }

      // Collect job IDs from notification.
      std::set<DWORD> notified_jobs;
      if (pInfo)
      {
        for (DWORD i = 0; i < pInfo->Count; ++i)
        {
          auto& data = pInfo->aData[i];
          if (data.Type == JOB_NOTIFY_TYPE)
            notified_jobs.insert(data.Id);
        }
        FreePrinterNotifyInfo(pInfo);
      }

      // For each notified job, fetch full JOB_INFO_2W.
      for (DWORD jid : notified_jobs)
      {
        DWORD needed = 0;
        GetJobW(mon.printer_handle, jid, 2, nullptr, 0, &needed);
        if (needed == 0) continue;

        std::vector<BYTE> buf(needed);
        if (!GetJobW(mon.printer_handle, jid, 2, buf.data(), needed, &needed))
          continue;

        auto* job = reinterpret_cast<JOB_INFO_2W*>(buf.data());

        // Track state.
        auto& state = job_states[jid];
        if (state.job_id == 0)
        {
          state.job_id = jid;
          state.printer_name = narrow(mon.name);
          state.document_name = narrow(job->pDocument);
          ++total_jobs_observed;
        }

        DiagnosticCapture cap;
        cap.timestamp = timestamp_now();
        cap.status = job->Status;
        cap.total_pages = job->TotalPages;
        cap.pages_printed = job->PagesPrinted;

        bool status_changed = state.captures.empty() ||
            state.captures.back().status != cap.status;
        bool pages_changed = state.captures.empty() ||
            state.captures.back().total_pages != cap.total_pages ||
            state.captures.back().pages_printed != cap.pages_printed;

        state.captures.push_back(cap);

        // On first capture or status change, print full dump.
        if (!state.full_dump_done || status_changed || pages_changed)
        {
          std::string status_label = status_changed ? " (状态变更)" : "";
          print_box("作业 #" + std::to_string(jid) + " @ " +
                    state.printer_name + status_label);
          std::printf("  [%s] 捕获 #%zu\n\n", cap.timestamp.c_str(),
                      state.captures.size());

          dump_job_info_2w(*job);
          std::printf("\n");

          // DEVMODE
          if (job->pDevMode)
          {
            dump_devmode(job->pDevMode, false);
          }
          else
          {
            // Attempt fallback via DocumentPropertiesW.
            LONG dmSize = DocumentPropertiesW(nullptr, mon.printer_handle,
                                              const_cast<LPWSTR>(mon.name.c_str()),
                                              nullptr, nullptr, 0);
            if (dmSize > 0)
            {
              std::vector<BYTE> fb(static_cast<size_t>(dmSize));
              LONG res = DocumentPropertiesW(nullptr, mon.printer_handle,
                                             const_cast<LPWSTR>(mon.name.c_str()),
                                             reinterpret_cast<PDEVMODEW>(fb.data()),
                                             nullptr, DM_OUT_BUFFER);
              if (res >= 0)
                dump_devmode(reinterpret_cast<DEVMODEW*>(fb.data()), true);
              else
                dump_devmode(nullptr, false);
            }
            else
            {
              dump_devmode(nullptr, false);
            }
          }

          std::printf("\n");
          check_spool_files(jid, state.printer_name, state);
          std::printf("\n");
          check_print_ticket_json(state.printer_name, jid);
          std::printf("\n");

          if (state.captures.size() > 1)
            print_change_history(state);

          state.full_dump_done = true;
        }
      }
    }

    // Periodic rescan (every 5 seconds).
    auto since_rescan = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_rescan).count();
    if (since_rescan >= 5)
    {
      last_rescan = now;
      for (auto& mon : monitors)
      {
        DWORD needed = 0, returned = 0;
        EnumJobsW(mon.printer_handle, 0, 256, 2, nullptr, 0, &needed, &returned);
        if (needed == 0) continue;

        std::vector<BYTE> buf(needed);
        if (!EnumJobsW(mon.printer_handle, 0, 256, 2, buf.data(), needed,
                       &needed, &returned))
          continue;

        auto* jobs = reinterpret_cast<JOB_INFO_2W*>(buf.data());
        for (DWORD i = 0; i < returned; ++i)
        {
          auto& job = jobs[i];
          auto& state = job_states[job.JobId];
          if (state.job_id == 0)
          {
            state.job_id = job.JobId;
            state.printer_name = narrow(mon.name);
            state.document_name = narrow(job.pDocument);
            ++total_jobs_observed;
          }

          // Check for page count changes (especially TotalPages 0 → N).
          if (!state.captures.empty())
          {
            auto& last = state.captures.back();
            if (last.total_pages != job.TotalPages ||
                last.pages_printed != job.PagesPrinted)
            {
              DiagnosticCapture cap;
              cap.timestamp = timestamp_now();
              cap.status = job.Status;
              cap.total_pages = job.TotalPages;
              cap.pages_printed = job.PagesPrinted;
              state.captures.push_back(cap);

              std::printf("  [%s] rescan: 作业 #%lu TotalPages %lu->%lu, "
                          "PagesPrinted %lu->%lu\n",
                          cap.timestamp.c_str(),
                          static_cast<unsigned long>(job.JobId),
                          static_cast<unsigned long>(last.total_pages),
                          static_cast<unsigned long>(job.TotalPages),
                          static_cast<unsigned long>(last.pages_printed),
                          static_cast<unsigned long>(job.PagesPrinted));
            }
          }
        }
      }
    }
  }

  // ── summary report ─────────────────────────────────────────────
  std::printf("\n%s\n", SEP);
  std::printf("  诊断汇总报告\n");
  std::printf("%s\n", SEP);
  std::printf("  总观察作业数: %d\n\n", total_jobs_observed);

  for (const auto& [jid, state] : job_states)
  {
    std::printf("  作业 #%lu: \"%s\" @ %s (%zu 次捕获)\n",
                static_cast<unsigned long>(jid),
                state.document_name.c_str(),
                state.printer_name.c_str(),
                state.captures.size());

    if (state.captures.size() > 1)
    {
      const auto& first = state.captures.front();
      const auto& last = state.captures.back();

      std::printf("    TotalPages:   %lu -> %lu\n",
                  static_cast<unsigned long>(first.total_pages),
                  static_cast<unsigned long>(last.total_pages));
      std::printf("    PagesPrinted: %lu -> %lu\n",
                  static_cast<unsigned long>(first.pages_printed),
                  static_cast<unsigned long>(last.pages_printed));

      // Check if TotalPages was ever 0
      bool was_zero = false;
      bool recovered = false;
      for (const auto& c : state.captures)
      {
        if (c.total_pages == 0) was_zero = true;
        if (was_zero && c.total_pages > 0) { recovered = true; break; }
      }
      if (was_zero && !recovered)
        std::printf("    *** TotalPages 始终为 0 — 需要 SPL 解析或重试机制 ***\n");
      else if (was_zero && recovered)
        std::printf("    TotalPages 从 0 恢复 — 重试机制可解决此问题\n");
    }
    std::printf("\n");
  }

  // Recommendations.
  std::printf("  -- 建议 --\n");
  bool any_zero_pages = false;
  bool any_spool_devmode = false;
  bool any_spool_copies_gt1 = false;
  for (const auto& [jid, state] : job_states)
  {
    for (const auto& c : state.captures)
      if (c.total_pages == 0) any_zero_pages = true;
    if (state.spool_devmode_found) any_spool_devmode = true;
    if (state.spool_copies > 1) any_spool_copies_gt1 = true;
  }
  if (any_zero_pages)
    std::printf("  * TotalPages=0 问题: 建议启用 TotalPages 重试机制 (Part 3a)\n");
  if (any_spool_devmode)
    std::printf("  * SPL DEVMODE 可用: spool 文件中包含 DEVMODE，可作为备用数据源\n");
  if (any_spool_copies_gt1)
    std::printf("  * ★ SPL 份数>1: spool DEVMODE 中检测到多份打印，"
                "JOB_INFO_2W 若显示 1 则应优先使用 spool 值\n");
  std::printf("  * 运行 wpce_printer_scanner.exe 检查 DM_COLOR 位是否被驱动支持\n");
  std::printf("  * 检查 pDatatype 字段判断驱动类型 (RAW=绕过XPS, NT EMF=可解析SPL)\n");

  std::printf("\n%s\n", SEP);

  // Cleanup.
  for (auto& mon : monitors)
  {
    if (mon.notification_handle && mon.notification_handle != INVALID_HANDLE_VALUE)
      FindClosePrinterChangeNotification(mon.notification_handle);
    if (mon.printer_handle)
      ClosePrinter(mon.printer_handle);
  }

  std::printf("  诊断结束。\n\n");
  return 0;
}

#else  // non-Windows stub
#include <cstdio>
int main() {
  std::printf("此工具仅在 Windows 平台可用。\n");
  return 1;
}
#endif
