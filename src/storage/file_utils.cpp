#include "storage/file_utils.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <chrono>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>

#ifndef _WIN32
#include <unistd.h>  // fsync
#endif

namespace wpce::storage
{
namespace
{
thread_local AtomicWriteHook write_hook;
std::atomic_uint64_t temp_sequence{0};
void checkpoint(AtomicWriteStage stage, const std::filesystem::path& path)
{
  if (write_hook) write_hook(stage, path);
}
}

void set_atomic_write_hook(AtomicWriteHook hook) { write_hook = std::move(hook); }

std::filesystem::path ensure_output_directory(const std::filesystem::path& directory)
{
  std::filesystem::create_directories(directory);
  return directory;
}

std::filesystem::path make_temp_path(const std::filesystem::path& final_path)
{
  std::ostringstream suffix;
  suffix << ".tmp."
#ifdef _WIN32
         << GetCurrentProcessId()
#else
         << getpid()
#endif
         << '.' << ++temp_sequence;
  auto result = final_path;
  result += suffix.str();
  return result;
}

void replace_file(const std::filesystem::path& temp_path, const std::filesystem::path& final_path)
{
#ifdef _WIN32
  constexpr int kMaxAttempts = 8;
  DWORD last_error = ERROR_SUCCESS;

  for (int attempt = 0; attempt < kMaxAttempts; ++attempt)
  {
    if (MoveFileExW(
          temp_path.wstring().c_str(),
          final_path.wstring().c_str(),
          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
      return;
    }

    last_error = GetLastError();

    // Cross-device moves cannot preserve the atomic replacement contract.
    if (last_error == ERROR_NOT_SAME_DEVICE)
    {
      break;
    }

    if (last_error != ERROR_ACCESS_DENIED &&
        last_error != ERROR_SHARING_VIOLATION &&
        last_error != ERROR_LOCK_VIOLATION &&
        last_error != ERROR_FILE_NOT_FOUND)
    {
      break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10 * (attempt + 1)));
  }

  throw std::system_error(static_cast<int>(last_error), std::system_category(), "MoveFileExW");
#else
  std::filesystem::rename(temp_path, final_path);
#endif
}

void write_text_file_atomic(const std::string& content, const std::filesystem::path& path)
{
  if (!path.parent_path().empty()) ensure_output_directory(path.parent_path());
  const std::filesystem::path temp_path = make_temp_path(path);

  try
  {
    {
      std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
      if (!output)
      {
        throw std::runtime_error("failed to open temporary file: " + path.string());
      }
      checkpoint(AtomicWriteStage::Write, path);
      output.write(content.data(), static_cast<std::streamsize>(content.size()));
      if (!output)
      {
        throw std::runtime_error("failed to write temporary file: " + path.string());
      }
      // Explicit flush + close so that write errors during final flush
      // (e.g. disk full) are detected before the atomic rename.
      checkpoint(AtomicWriteStage::Flush, path);
      output.flush();
      if (!output)
      {
        throw std::runtime_error("failed to flush temporary file: " + path.string());
      }
      checkpoint(AtomicWriteStage::Close, path);
      output.close();
      if (!output)
      {
        throw std::runtime_error("failed to close temporary file: " + path.string());
      }
    }
#ifdef _WIN32
    // Force buffered data to disk before the atomic rename so that
    // a power loss between the rename and the next filesystem flush
    // cannot leave a zero-length or partially-written destination.
    {
      HANDLE hFile = CreateFileW(
          temp_path.wstring().c_str(),
          GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
          FILE_FLAG_WRITE_THROUGH, nullptr);
      if (hFile == INVALID_HANDLE_VALUE)
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "open for flush");
      const BOOL flushed = FlushFileBuffers(hFile);
      const DWORD error = flushed ? ERROR_SUCCESS : GetLastError();
      CloseHandle(hFile);
      if (!flushed)
        throw std::system_error(static_cast<int>(error), std::system_category(), "FlushFileBuffers");
    }
#else
    // POSIX: force buffered data to disk before atomic rename.
    {
      FILE* fp = std::fopen(temp_path.string().c_str(), "r+");
      if (fp == nullptr) throw std::runtime_error("open for fsync failed");
      const int synced = fsync(fileno(fp));
      const int closed = std::fclose(fp);
      if (synced != 0 || closed != 0) throw std::runtime_error("fsync failed");
    }
#endif
    checkpoint(AtomicWriteStage::Sync, path);
    checkpoint(AtomicWriteStage::Replace, path);
    replace_file(temp_path, path);
  }
  catch (...)
  {
    std::error_code ignored;
    std::filesystem::remove(temp_path, ignored);
    throw;
  }
}

std::string safe_filename_token(std::string value)
{
  if (value.empty())
  {
    return "unknown-printer";
  }

  for (char& ch : value)
  {
    const bool keep =
      (ch >= 'a' && ch <= 'z') ||
      (ch >= 'A' && ch <= 'Z') ||
      (ch >= '0' && ch <= '9') ||
      ch == '-' || ch == '_';
    if (!keep)
    {
      ch = '_';
    }
  }
  return value;
}

}  // namespace wpce::storage
