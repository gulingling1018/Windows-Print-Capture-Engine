#include "bridge/metadata_merger.h"
#include "bridge/print_ticket_inbox.h"
#include "storage/reliable_file_writer.h"
#include <windows.h>
#include <optional>
#include <set>
#include <fstream>
#include <sstream>
namespace wpce::bridge
{
namespace
{
std::optional<std::filesystem::path> process_env_path(const wchar_t* name)
{
  const DWORD length=GetEnvironmentVariableW(name,nullptr,0);
  if(!length) return std::nullopt;
  std::wstring value(length,L'\0');
  const auto copied=GetEnvironmentVariableW(name,value.data(),length);
  if(!copied || copied>=length) return std::nullopt;
  value.resize(copied); return std::filesystem::path(value);
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

}
std::vector<std::filesystem::path> pt_inbox_directories(const std::filesystem::path& output)
{
  std::vector<std::filesystem::path> roots{output, output.parent_path()};
  if (auto path=process_env_path(L"WPCE_OUTPUT_DIR")) roots.push_back(*path);
  if (auto path=machine_env_path(L"WPCE_OUTPUT_DIR")) roots.push_back(*path);
  return roots;
}
bool load_pt_metadata(const std::filesystem::path& path, core::MetadataSnapshot& out)
{
  try
  {
    if (std::filesystem::file_size(path)>4*1024*1024) return false;
    std::ifstream input(path,std::ios::binary); std::ostringstream text; text << input.rdbuf();
    if (!input || input.bad()) return false;
    PrintTicketMessage message;
    if(parse_pt_message(text.str(),message)) { out=std::move(message.metadata); return true; }
    return parse_metadata_json(text.str(),out);
  }
  catch(...) { return false; }
}
int scan_and_merge_pt_metadata(const std::filesystem::path& output, MergedMetadataCallback callback, bool cleanup)
{
  if (cleanup)
  {
    const auto result=consume_pt_inbox(pt_inbox_directories(output),[&](const PrintTicketMessage& message) {
      if(!callback) return false; callback(message.metadata); return true;
    });
    return static_cast<int>(result.delivered);
  }
  int count=0;
  std::set<std::filesystem::path> visited;
  for(const auto& root:pt_inbox_directories(output))
  {
    std::error_code error; if(!std::filesystem::is_directory(root,error)) continue;
    const auto canonical=std::filesystem::weakly_canonical(root,error);
    if(error || !visited.insert(canonical).second) continue;
    for(const auto& entry:std::filesystem::directory_iterator(canonical,error))
    {
      const auto name=entry.path().filename().string();
      if(!name.ends_with("-pt-metadata.json") && !name.ends_with(".pt-message.json")) continue;
      core::MetadataSnapshot metadata;
      if(load_pt_metadata(entry.path(),metadata)) { if(callback) callback(std::move(metadata)); ++count; }
    }
  }
  return count;
}
}
