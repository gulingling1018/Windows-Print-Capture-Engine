/// @file metadata_merger.cpp
/// Implementation — scans for PrintTicket metadata files and merges.

#include "bridge/metadata_merger.h"
#include "storage/json_serializer.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace wpce::bridge
{
namespace
{

/// Minimal JSON number extractor — finds "key": VALUE where VALUE is an integer.
/// This avoids needing a full JSON parser for the simple metadata files.
int extract_json_int(const std::string& json, const char* key)
{
  std::string pattern = std::string("\"") + key + "\": ";
  auto pos = json.find(pattern);
  if (pos == std::string::npos) return -1;
  pos += pattern.size();
  // Skip whitespace
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
  if (pos >= json.size()) return -1;

  char* end = nullptr;
  long val = std::strtol(json.data() + pos, &end, 10);
  if (end == json.data() + pos) return -1;
  return static_cast<int>(val);
}

std::string extract_json_string(const std::string& json, const char* key)
{
  std::string pattern = std::string("\"") + key + "\": \"";
  auto pos = json.find(pattern);
  if (pos == std::string::npos) return {};
  pos += pattern.size();

  std::string value;
  bool escaped = false;
  for (; pos < json.size(); ++pos)
  {
    const char ch = json[pos];
    if (escaped)
    {
      switch (ch)
      {
        case '"': value.push_back('"'); break;
        case '\\': value.push_back('\\'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        default: value.push_back(ch); break;
      }
      escaped = false;
      continue;
    }
    if (ch == '\\')
    {
      escaped = true;
      continue;
    }
    if (ch == '"')
    {
      return value;
    }
    value.push_back(ch);
  }
  return {};
}

core::FieldState field_state_from_string(const std::string& state)
{
  if (state == "reported") return core::FieldState::Reported;
  if (state == "unsupported") return core::FieldState::Unsupported;
  if (state == "not_reported") return core::FieldState::NotReported;
  if (state == "error") return core::FieldState::Error;
  return core::FieldState::Unknown;
}

bool load_field_value(const std::string& json, const char* key,
                      core::FieldValue& out)
{
  // Look for "key": { "value": "...", "state": "..." }
  std::string fpattern = std::string("\"") + key + "\": {";
  auto fpos = json.find(fpattern);
  if (fpos == std::string::npos) return false;
  auto fend = json.find('}', fpos);
  if (fend == std::string::npos) return false;

  // Extract value
  auto vpos = json.find("\"value\": \"", fpos);
  if (vpos == std::string::npos || vpos > fend)
    return false;
  vpos += 10;
  std::string val;
  bool escaped = false;
  std::size_t vend = vpos;
  for (; vend < json.size(); ++vend)
  {
    const char ch = json[vend];
    if (escaped)
    {
      switch (ch)
      {
        case '"': val.push_back('"'); break;
        case '\\': val.push_back('\\'); break;
        case 'n': val.push_back('\n'); break;
        case 'r': val.push_back('\r'); break;
        case 't': val.push_back('\t'); break;
        default: val.push_back(ch); break;
      }
      escaped = false;
      continue;
    }
    if (ch == '\\')
    {
      escaped = true;
      continue;
    }
    if (ch == '"') break;
    val.push_back(ch);
  }
  if (vend == json.size()) return false;

  std::string state = "unknown";
  auto spos = json.find("\"state\": \"", fpos);
  if (spos != std::string::npos && spos < fend)
  {
    spos += 10;
    auto send = json.find('"', spos);
    if (send != std::string::npos && send <= fend)
      state = json.substr(spos, send - spos);
  }

  out.value = std::move(val);
  out.state = field_state_from_string(state);
  return true;
}

std::optional<std::filesystem::path> process_env_path(const char* name)
{
  char* value = nullptr;
  std::size_t len = 0;
  if (_dupenv_s(&value, &len, name) != 0 || value == nullptr)
  {
    return std::nullopt;
  }

  std::filesystem::path result(value);
  free(value);
  if (result.empty())
  {
    return std::nullopt;
  }
  return result;
}

std::optional<std::filesystem::path> machine_env_path(const wchar_t* name)
{
#ifdef _WIN32
  HKEY key = nullptr;
  if (RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
        0,
        KEY_QUERY_VALUE,
        &key) != ERROR_SUCCESS)
  {
    return std::nullopt;
  }

  DWORD type = 0;
  DWORD bytes = 0;
  LONG rc = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
  if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes == 0)
  {
    RegCloseKey(key);
    return std::nullopt;
  }

  std::wstring value(bytes / sizeof(wchar_t), L'\0');
  rc = RegQueryValueExW(
    key,
    name,
    nullptr,
    &type,
    reinterpret_cast<LPBYTE>(value.data()),
    &bytes);
  RegCloseKey(key);

  if (rc != ERROR_SUCCESS)
  {
    return std::nullopt;
  }

  while (!value.empty() && value.back() == L'\0')
  {
    value.pop_back();
  }
  if (value.empty())
  {
    return std::nullopt;
  }

  if (type == REG_EXPAND_SZ)
  {
    DWORD needed = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (needed > 0)
    {
      std::wstring expanded(needed, L'\0');
      DWORD written = ExpandEnvironmentStringsW(value.c_str(), expanded.data(), needed);
      if (written > 0)
      {
        while (!expanded.empty() && expanded.back() == L'\0')
        {
          expanded.pop_back();
        }
        value = std::move(expanded);
      }
    }
  }

  return std::filesystem::path(value);
#else
  (void)name;
  return std::nullopt;
#endif
}

}  // namespace

bool load_pt_metadata(const std::filesystem::path& json_path,
                      core::MetadataSnapshot& out)
{
  std::ifstream file(json_path, std::ios::in | std::ios::binary);
  if (!file) return false;

  std::ostringstream buf;
  buf << file.rdbuf();
  std::string json = buf.str();
  if (json.empty()) return false;

  // Extract job_id
  int jid = extract_json_int(json, "job_id");
  if (jid > 0) out.job_id = static_cast<std::uint32_t>(jid);

  out.printer_name = extract_json_string(json, "printer_name");

  // Extract each known field
  load_field_value(json, "total_pages", out.total_pages);
  load_field_value(json, "pages_printed", out.pages_printed);
  load_field_value(json, "copies", out.copies);
  load_field_value(json, "duplex", out.duplex);
  load_field_value(json, "color", out.color);
  load_field_value(json, "paper_size", out.paper_size);
  load_field_value(json, "orientation", out.orientation);
  load_field_value(json, "n_up", out.n_up);
  load_field_value(json, "collate", out.collate);
  load_field_value(json, "print_quality", out.print_quality);
  load_field_value(json, "media_type", out.media_type);
  load_field_value(json, "page_range", out.page_range);
  out.dm_fields_hex = extract_json_string(json, "dm_fields_hex");

  return true;
}

/// Scan a single directory for PT metadata files.
static int scan_one_dir(const std::filesystem::path& dir,
                        MergedMetadataCallback& callback,
                        bool cleanup)
{
  if (!std::filesystem::exists(dir)) return 0;
  int count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir))
  {
    if (!entry.is_regular_file()) continue;
    auto filename = entry.path().filename().string();
    if (filename.starts_with("job-") && filename.ends_with("-pt-metadata.json"))
    {
      core::MetadataSnapshot meta;
      meta.schema_version = 1;
      if (load_pt_metadata(entry.path(), meta))
      {
        if (callback) callback(std::move(meta));
        ++count;
      }

      // ── Cleanup ──────────────────────────────────────────────────
      // Delete the file after loading to prevent:
      //   1. Accumulation of stale files over long-running sessions
      //   2. Re-processing old files after watcher thread restart
      //   3. Dual-scan finding the same file in both directories
      if (cleanup)
      {
        std::error_code ignored;
        std::filesystem::remove(entry.path(), ignored);
      }
    }
  }
  return count;
}

int scan_and_merge_pt_metadata(const std::filesystem::path& output_dir,
                               MergedMetadataCallback callback,
                               bool cleanup_after_load)
{
  int count = 0;
  std::vector<std::filesystem::path> candidates;
  candidates.push_back(output_dir);

  auto parent = output_dir.parent_path();
  if (!parent.empty() && parent != output_dir)
  {
    candidates.push_back(parent);
  }

  if (auto env = process_env_path("WPCE_OUTPUT_DIR"); env.has_value())
  {
    candidates.push_back(*env);
  }
  if (auto env = machine_env_path(L"WPCE_OUTPUT_DIR"); env.has_value())
  {
    candidates.push_back(*env);
  }

  std::set<std::filesystem::path> seen_dirs;
  for (const auto& candidate : candidates)
  {
    if (candidate.empty()) continue;
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(candidate, ec);
    if (ec) canonical = candidate;
    if (!seen_dirs.insert(canonical).second) continue;

    count += scan_one_dir(canonical, callback, cleanup_after_load);
  }
  return count;
}

}  // namespace wpce::bridge
