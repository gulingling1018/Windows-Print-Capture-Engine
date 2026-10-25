#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace wpce::storage
{
struct RetentionResult
{
  std::size_t removed_directories{0};
  std::vector<std::string> errors;
};
/// Only dated capture subtrees (jobs/latest/pages) are removed. Never follows
/// links/reparse points or deletes the date bucket, orders, backups or state.
RetentionResult cleanup_expired_capture(const std::filesystem::path& root,
    const std::string& today, int retention_days);
}
