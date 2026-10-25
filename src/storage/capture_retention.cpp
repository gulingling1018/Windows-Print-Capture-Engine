#include "storage/capture_retention.h"
#include "common/business_clock.h"
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#endif

namespace wpce::storage
{
namespace
{
bool linked(const std::filesystem::path& path)
{
  const auto state = std::filesystem::symlink_status(path);
  if (std::filesystem::is_symlink(state)) return true;
#ifdef _WIN32
  const auto attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) throw std::runtime_error("cannot read capture attributes");
  return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  return false;
#endif
}
bool contains_link(const std::filesystem::path& path)
{
  if (linked(path)) return true;
  for (const auto& child : std::filesystem::recursive_directory_iterator(path))
    if (linked(child.path())) return true;
  return false;
}
}
RetentionResult cleanup_expired_capture(const std::filesystem::path& root,
    const std::string& today, int retention_days)
{
  RetentionResult result;
  if (retention_days <= 0) return result;
  const auto current = common::parse_business_date(today);
  if (!current) { result.errors.push_back("invalid retention date"); return result; }
  try
  {
    if (!std::filesystem::exists(root) || linked(root)) return result;
    for (const auto& entry : std::filesystem::directory_iterator(root))
    {
      if (!entry.is_directory() || linked(entry.path())) continue;
      const auto date = common::parse_business_date(entry.path().filename().string());
      if (!date || *date >= *current - std::chrono::days{retention_days}) continue;
      for (const char* name : {"jobs", "latest", "pages"})
      {
        const auto target = entry.path() / name;
        if (!std::filesystem::exists(target)) continue;
        if (!std::filesystem::is_directory(target) || contains_link(target)) continue;
        std::filesystem::remove_all(target);
        ++result.removed_directories;
      }
    }
  }
  catch (const std::exception& error) { result.errors.push_back(error.what()); }
  return result;
}
}
