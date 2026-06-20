#pragma once

#include <filesystem>
#include <string>

namespace wpce::storage
{

/// Ensure a directory exists, creating it if needed.
/// Returns the directory path for call chaining.
std::filesystem::path ensure_output_directory(const std::filesystem::path& directory);

/// Generate a thread-unique temporary path next to the target file.
/// Format: `<final_path>.tmp.<thread_id_hash>`
std::filesystem::path make_temp_path(const std::filesystem::path& final_path);

/// Atomically replace `final_path` with `temp_path`.
/// On Windows uses MoveFileExW with MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH.
/// On other platforms uses std::filesystem::rename.
/// Throws std::system_error on failure.
void replace_file(const std::filesystem::path& temp_path, const std::filesystem::path& final_path);

/// Write string content to a file atomically (temp file + replace).
/// Creates parent directories as needed. Cleans up temp file on failure.
void write_text_file_atomic(const std::string& content, const std::filesystem::path& path);

}  // namespace wpce::storage
