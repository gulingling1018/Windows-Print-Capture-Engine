/// @file spool_file_parser.cpp
/// Parse Windows EMF spool files for page count and DEVMODE extraction.

#include "parser/spool_file_parser.h"
#include "collector/metadata_collector.h"
#include "common/paper_size_map.h"
#include "common/pricing_engine.h"
#include "common/file_type_detector.h"

#ifdef _WIN32
#include <windows.h>
#include <winspool.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace wpce::parser
{

#ifdef _WIN32

// EMF spool record types (from Windows internals)
//
// The Windows EMF spool format ("NT EMF 1.008") uses 8-byte record headers:
//   struct { DWORD record_type; DWORD record_size; };
// followed by record_size bytes of payload.  Records are 4-byte aligned.

namespace emri
{
constexpr DWORD METAFILE_DATA    = 0x00000001;  // One per rendered page
constexpr DWORD METAFILE_EXT     = 0x00000002;  // Extended metafile (also a page)
constexpr DWORD ENGINE_FONT      = 0x00000003;
constexpr DWORD TYPE1_FONT       = 0x00000006;
constexpr DWORD DESIGNVECTOR     = 0x00000007;
constexpr DWORD SUBSET_FONT      = 0x00000008;
constexpr DWORD DEVMODE           = 0x00000009;  // Per-job DEVMODE
constexpr DWORD BW_METAFILE_EXT  = 0x0000000A;
constexpr DWORD BW_METAFILE_DATA = 0x0000000B;
constexpr DWORD HEADER_RECORD    = 0x0000000C;
constexpr DWORD FORM_METAFILE    = 0x0000000D;
constexpr DWORD EOF_RECORD       = 0x0000000E;
constexpr DWORD PRESTARTPAGE     = 0x00000013;
constexpr DWORD STARTPAGE        = 0x00000014;
constexpr DWORD ENDPAGE          = 0x00000015;
}  // namespace emri

// SHD shadow file structure
//
// The .SHD companion file contains job metadata including the job ID.
// This is a well-reverse-engineered structure (SHADOWFILE_3 on x64 Windows 10+).
// We only read the first portion to extract the job ID for correlation.

#pragma pack(push, 1)
struct ShadowFileHeader
{
  DWORD signature;      // Usually 0x00000003 (version 3)
  DWORD job_id;         // The spooler job ID
  // There are more fields (offsets to strings, etc.) but we only need job_id.
};
#pragma pack(pop)

bool icase_contains(std::string_view haystack, std::string_view needle)
{
  if (needle.empty() || needle.size() > haystack.size()) return false;
  auto it = std::search(haystack.begin(), haystack.end(),
                        needle.begin(), needle.end(),
                        [](char a, char b) {
                          return std::tolower(static_cast<unsigned char>(a)) ==
                                 std::tolower(static_cast<unsigned char>(b));
                        });
  return it != haystack.end();
}

bool is_system_shd_string(std::string_view value)
{
  return icase_contains(value, "\\drivers\\") ||
         icase_contains(value, "\\spool\\") ||
         icase_contains(value, "winprint");
}

int shd_document_candidate_score(std::string_view value, std::string_view ext)
{
  if (ext.empty()) return std::numeric_limits<int>::min();
  if (is_system_shd_string(value)) return std::numeric_limits<int>::min();

  int score = 100;
  if (ext == "dwg" || ext == "dwt")
    score += 1000;
  if (value.find_first_of("/\\") != std::string_view::npos)
    score += 50;
  if (icase_contains(value, "xwechat_files") ||
      icase_contains(value, "\\msg\\file\\") ||
      icase_contains(value, "/msg/file/"))
  {
    score += 75;
  }
  return score;
}

// SHD wide-string extraction
//
// The SHD file embeds wide-character strings (printer name, driver name,
// document name, user name, etc.).  Rather than reverse-engineering the
// exact SHADOWFILE_3 layout (which varies by Windows version and arch),
// we scan for null-terminated wide strings and run file-extension
// detection on each to find the original document name.

static std::string extract_document_name_from_shd(const std::vector<BYTE>& data)
{
  // Skip the fixed header (at least 8 bytes: signature + job_id).
  // Strings are embedded in the latter part of the file.
  if (data.size() < 64) return {};

  // Scan for wide strings starting after the fixed header area.
  // We look for sequences of wchar_t that end with L'\0' and have
  // at least 3 printable characters (to skip padding/noise).
  const size_t start = 64;
  std::string best_doc_name;
  int best_score = std::numeric_limits<int>::min();

  size_t pos = start;
  while (pos + sizeof(wchar_t) <= data.size())
  {
    // Ensure alignment for wchar_t.
    if (pos % sizeof(wchar_t) != 0)
    {
      ++pos;
      continue;
    }

    // Try to read a wide string starting at this position.
    const wchar_t* wstr = reinterpret_cast<const wchar_t*>(data.data() + pos);
    const size_t max_chars = (data.size() - pos) / sizeof(wchar_t);

    // Find the null terminator.
    size_t len = 0;
    while (len < max_chars && wstr[len] != L'\0')
      ++len;

    if (len < 3 || len >= max_chars)
    {
      pos += sizeof(wchar_t);
      continue;
    }

    // Convert to UTF-8.
    int utf8_size = WideCharToMultiByte(CP_UTF8, 0, wstr, static_cast<int>(len),
                                         nullptr, 0, nullptr, nullptr);
    if (utf8_size <= 0)
    {
      pos += (len + 1) * sizeof(wchar_t);
      continue;
    }

    std::string utf8(static_cast<size_t>(utf8_size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, static_cast<int>(len),
                        utf8.data(), utf8_size, nullptr, nullptr);

    const std::string ext = common::extract_file_extension(utf8);
    const int score = shd_document_candidate_score(utf8, ext);
    if (score == std::numeric_limits<int>::min())
    {
      pos += (len + 1) * sizeof(wchar_t);
      continue;
    }
    if (score > best_score ||
        (score == best_score && utf8.size() > best_doc_name.size()))
    {
      best_score = score;
      best_doc_name = std::move(utf8);
    }

    pos += (len + 1) * sizeof(wchar_t);
  }

  return best_doc_name;
}

// Helper: detect spool file format from magic bytes
static SpoolFileInfo::Format detect_format(const BYTE* header, size_t header_size)
{
  if (header_size < 4)
    return SpoolFileInfo::Format::Unknown;

  DWORD magic = 0;
  std::memcpy(&magic, header, sizeof(DWORD));

  // EMF spool files: the first record type is typically EMRI_HEADER_RECORD (0x0C)
  // or one of the EMRI_* types.  The record_type value is in the low range.
  // A heuristic: if the first DWORD is a known EMRI type (1-0x20) and the
  // second DWORD (record_size) is reasonable, it's likely EMF spool.
  if (header_size >= 8)
  {
    DWORD rec_size = 0;
    std::memcpy(&rec_size, header + 4, sizeof(DWORD));
    if (magic >= 1 && magic <= 0x20 && rec_size > 0 && rec_size < 256 * 1024 * 1024)
      return SpoolFileInfo::Format::EMF;
  }

  // PostScript: starts with "%!"
  if (header_size >= 2 && header[0] == '%' && header[1] == '!')
    return SpoolFileInfo::Format::PostScript;

  // PCL: starts with ESC character (0x1B)
  if (header[0] == 0x1B)
    return SpoolFileInfo::Format::PCL;

  // XPS/ZIP: starts with "PK" (ZIP package)
  if (header_size >= 2 && header[0] == 'P' && header[1] == 'K')
    return SpoolFileInfo::Format::XPS;

  return SpoolFileInfo::Format::Unknown;
}

// Parse EMF spool file
SpoolFileInfo parse_emf_spool_file(const std::filesystem::path& spl_path)
{
  SpoolFileInfo info;
  info.path = spl_path;

  HANDLE hFile = CreateFileW(
      spl_path.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

  if (hFile == INVALID_HANDLE_VALUE)
    return info;

  LARGE_INTEGER fileSize;
  if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart <= 0)
  {
    CloseHandle(hFile);
    return info;
  }

  info.file_size = static_cast<std::uint64_t>(fileSize.QuadPart);

  // Safety cap: don't try to read files larger than 512 MB into memory.
  constexpr LONGLONG kMaxSize = 512LL * 1024 * 1024;
  if (fileSize.QuadPart > kMaxSize)
  {
    CloseHandle(hFile);
    return info;
  }

  std::vector<BYTE> data(static_cast<size_t>(fileSize.QuadPart));
  DWORD bytesRead = 0;
  if (!ReadFile(hFile, data.data(), static_cast<DWORD>(data.size()), &bytesRead, nullptr)
      || bytesRead < 8)
  {
    CloseHandle(hFile);
    return info;
  }
  CloseHandle(hFile);

  // Detect format.
  info.format = detect_format(data.data(), bytesRead);
  if (info.format != SpoolFileInfo::Format::EMF)
    return info;

  // Walk EMF spool records.
  int page_count = 0;
  size_t offset = 0;

  while (offset + 8 <= bytesRead)
  {
    DWORD rec_type = 0;
    DWORD rec_size = 0;
    std::memcpy(&rec_type, data.data() + offset, sizeof(DWORD));
    std::memcpy(&rec_size, data.data() + offset + 4, sizeof(DWORD));

    if (rec_size == 0 || offset + 8 + rec_size > bytesRead)
      break;  // Truncated or corrupt - stop here (partial results are OK).

    const BYTE* payload = data.data() + offset + 8;

    switch (rec_type)
    {
      case emri::METAFILE_DATA:
      case emri::METAFILE_EXT:
      case emri::BW_METAFILE_DATA:
      case emri::BW_METAFILE_EXT:
        ++page_count;
        break;

      case emri::STARTPAGE:
        // Some drivers use STARTPAGE/ENDPAGE pairs instead of METAFILE_DATA.
        // Count these only if we haven't counted a METAFILE_DATA record for this page.
        // Heuristic: if we haven't seen any METAFILE_DATA yet, start counting STARTPAGE.
        if (page_count == 0)
          ++page_count;
        break;

      case emri::DEVMODE:
      {
        // The payload is a complete DEVMODEW structure.
        if (rec_size >= sizeof(DEVMODEW))
        {
          const auto* dm = reinterpret_cast<const DEVMODEW*>(payload);
          info.has_devmode = true;
          info.dm_fields = dm->dmFields;

          if (dm->dmFields & DM_COPIES)
            info.dm_copies = dm->dmCopies;
          if (dm->dmFields & DM_COLOR)
            info.dm_color = dm->dmColor;
          if (dm->dmFields & DM_DUPLEX)
            info.dm_duplex = dm->dmDuplex;
          if (dm->dmFields & DM_PAPERSIZE)
            info.dm_paper_size = dm->dmPaperSize;
          if (dm->dmFields & DM_ORIENTATION)
            info.dm_orientation = dm->dmOrientation;
          if (dm->dmFields & DM_NUP)
            info.dm_nup = static_cast<std::int32_t>(dm->dmNup);
          if (dm->dmFields & DM_COLLATE)
            info.dm_collate = dm->dmCollate;
          if (dm->dmFields & DM_PRINTQUALITY)
            info.dm_print_quality = dm->dmPrintQuality;
        }
        break;
      }

      case emri::EOF_RECORD:
        goto done_walking;  // Clean end of spool data.

      default:
        break;  // Skip unknown record types.
    }

    // Advance past header + payload, aligned to 4 bytes.
    offset += 8 + rec_size;
    offset = (offset + 3) & ~static_cast<size_t>(3);
  }

done_walking:
  info.emf_page_count = page_count;
  return info;
}

// Find spool file by job ID via SHD matching
static std::filesystem::path get_spool_directory()
{
  wchar_t windir[MAX_PATH] = {};
  GetWindowsDirectoryW(windir, MAX_PATH);
  return std::filesystem::path(windir) / L"System32" / L"spool" / L"PRINTERS";
}

std::optional<SpoolFileInfo> find_and_parse_spool_file(
    std::uint32_t job_id,
    const std::string& /*printer_name*/)
{
  const auto spool_dir = get_spool_directory();
  std::error_code ec;
  if (!std::filesystem::exists(spool_dir, ec))
    return std::nullopt;

  // Strategy 1: Match via .SHD shadow file (contains job_id).
  // SHD and SPL files share the same numeric prefix (e.g. 00042.SHD, 00042.SPL).
  for (const auto& entry : std::filesystem::directory_iterator(spool_dir, ec))
  {
    if (!entry.is_regular_file(ec)) continue;

    auto ext = entry.path().extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    if (ext != L".shd") continue;

    // Read the SHD file to extract both job_id and original document name.
    HANDLE hShd = CreateFileW(
        entry.path().c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (hShd == INVALID_HANDLE_VALUE) continue;

    LARGE_INTEGER shd_size = {};
    if (!GetFileSizeEx(hShd, &shd_size) ||
        shd_size.QuadPart < static_cast<LONGLONG>(sizeof(ShadowFileHeader)) ||
        shd_size.QuadPart > 16LL * 1024LL * 1024LL)
    {
      CloseHandle(hShd);
      continue;
    }

    std::vector<BYTE> shd_data(static_cast<std::size_t>(shd_size.QuadPart));
    DWORD bytesRead = 0;
    bool ok = ReadFile(hShd, shd_data.data(), static_cast<DWORD>(shd_data.size()),
                       &bytesRead, nullptr) &&
              bytesRead >= sizeof(ShadowFileHeader);
    CloseHandle(hShd);

    if (!ok) continue;

    ShadowFileHeader shd_header = {};
    std::memcpy(&shd_header, shd_data.data(), sizeof(shd_header));

    // Match job_id.
    if (shd_header.job_id != job_id) continue;

    const std::string shd_document_name = extract_document_name_from_shd(shd_data);

    // Found the SHD - now look for the matching SPL file.
    auto spl_path = entry.path();
    spl_path.replace_extension(L".SPL");
    if (!std::filesystem::exists(spl_path, ec))
    {
      // Try lowercase.
      spl_path.replace_extension(L".spl");
      if (!std::filesystem::exists(spl_path, ec))
      {
        if (!shd_document_name.empty())
        {
          SpoolFileInfo info;
          info.path = entry.path();
          info.shd_document_name = shd_document_name;
          return info;
        }
        continue;
      }
    }

    auto info = parse_emf_spool_file(spl_path);
    info.shd_document_name = shd_document_name;
    if (info.format != SpoolFileInfo::Format::Unknown ||
        !info.shd_document_name.empty())
    {
      return info;
    }
  }

  // Strategy 2: If no SHD match, try all SPL files and pick the most recent.
  // This handles cases where the SHD file has already been cleaned up.
  std::filesystem::path best_spl;
  std::filesystem::file_time_type best_time{};

  for (const auto& entry : std::filesystem::directory_iterator(spool_dir, ec))
  {
    if (!entry.is_regular_file(ec)) continue;

    auto ext = entry.path().extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    if (ext != L".spl") continue;

    auto ftime = entry.last_write_time(ec);
    if (!ec && ftime > best_time)
    {
      best_time = ftime;
      best_spl = entry.path();
    }
  }

  if (!best_spl.empty())
  {
#ifdef _WIN32
    {
      char buf[512];
      std::snprintf(buf, sizeof(buf),
                    "[WPCE] spool: no SHD match for job %u; falling back to "
                    "most-recent SPL (unverified): %s\n",
                    static_cast<unsigned>(job_id),
                    best_spl.string().c_str());
      OutputDebugStringA(buf);
    }
#else
    (void)fprintf(stderr,
                  "[WPCE] spool: no SHD match for job %u; falling back to "
                  "most-recent SPL (unverified): %s\n",
                  static_cast<unsigned>(job_id),
                  best_spl.string().c_str());
#endif
    auto info = parse_emf_spool_file(best_spl);
    if (info.format != SpoolFileInfo::Format::Unknown)
      return info;
  }

  return std::nullopt;
}

// Build MetadataSnapshot from spool info
core::MetadataSnapshot metadata_from_spool(const SpoolFileInfo& info)
{
  core::MetadataSnapshot snapshot;
  snapshot.schema_version = 1;
  snapshot.dm_fields_hex = "SpoolFile";

  if (!info.shd_document_name.empty())
  {
    auto detected = common::detect_file_type(info.shd_document_name, "shd_path");
    if (!detected.extension.empty())
    {
      snapshot.file_extension = core::FieldValue{std::move(detected.extension), core::FieldState::Reported};
      snapshot.file_extension_source = std::move(detected.source);
      snapshot.file_extension_confidence = detected.confidence;
    }
  }

  // Page count from EMF parsing.
  if (info.emf_page_count > 0)
  {
    snapshot.total_pages = core::FieldValue{
        std::to_string(info.emf_page_count), core::FieldState::Reported};
  }

  if (!info.has_devmode)
    return snapshot;

  // Map DEVMODE fields using the same name mappers as metadata_collector.
  if (info.dm_copies > 0)
  {
    snapshot.copies = core::FieldValue{
        std::to_string(info.dm_copies), core::FieldState::Reported};
  }

  if (info.dm_color > 0)
  {
    auto name = collector::color_name(info.dm_color);
    if (!name.empty())
      snapshot.color = core::FieldValue{std::string(name), core::FieldState::Reported};
  }

  if (info.dm_duplex > 0)
  {
    auto name = collector::duplex_name(info.dm_duplex);
    if (!name.empty())
      snapshot.duplex = core::FieldValue{std::string(name), core::FieldState::Reported};
  }

  if (info.dm_paper_size > 0)
  {
    auto name = common::paper_size_name(info.dm_paper_size);
    if (!name.empty())
      snapshot.paper_size = core::FieldValue{std::string(name), core::FieldState::Reported};
  }

  if (info.dm_orientation > 0)
  {
    auto name = collector::orientation_name(info.dm_orientation);
    if (!name.empty())
      snapshot.orientation = core::FieldValue{std::string(name), core::FieldState::Reported};
  }

  if (info.dm_nup > 0)
  {
    auto name = collector::n_up_name(info.dm_nup);
    if (!name.empty())
      snapshot.n_up = core::FieldValue{std::string(name), core::FieldState::Reported};
  }

  if (info.dm_collate >= 0)
  {
    auto name = collector::collate_name(info.dm_collate);
    if (!name.empty())
      snapshot.collate = core::FieldValue{std::string(name), core::FieldState::Reported};
  }

  if (info.dm_print_quality != 0)
  {
    auto name = collector::print_quality_name(info.dm_print_quality);
    if (!name.empty())
      snapshot.print_quality = core::FieldValue{std::string(name), core::FieldState::Reported};
  }

  return snapshot;
}

#else  // non-Windows

std::optional<SpoolFileInfo> find_and_parse_spool_file(std::uint32_t, const std::string&)
{
  return std::nullopt;
}

SpoolFileInfo parse_emf_spool_file(const std::filesystem::path&)
{
  return {};
}

core::MetadataSnapshot metadata_from_spool(const SpoolFileInfo&)
{
  return {};
}

#endif

}  // namespace wpce::parser
