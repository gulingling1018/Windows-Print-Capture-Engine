#pragma once
#include "storage/file_utils.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wpce::storage
{
struct FileWrite { std::filesystem::path relative_path; std::string content; };
struct ReliableWriterConfig
{
  std::filesystem::path root;
  std::string queue_name;
  std::size_t max_item_bytes{64 * 1024 * 1024};
  std::chrono::milliseconds retry_delay{100};
  AtomicWriteHook worker_hook;
};
// Exclusive file handle locks release automatically on process termination.
class DirectoryLease
{
 public:
  explicit DirectoryLease(const std::filesystem::path& file);
  ~DirectoryLease();
  DirectoryLease(const DirectoryLease&) = delete;
  DirectoryLease& operator=(const DirectoryLease&) = delete;
  explicit operator bool() const noexcept;
 private: void* handle_{nullptr};
};
class ReliableFileWriter
{
 public:
  explicit ReliableFileWriter(ReliableWriterConfig config);
  ~ReliableFileWriter();
  void start();
  void stop() noexcept;
  void request_stop() noexcept;
  bool enqueue(std::vector<FileWrite> writes);
  bool running() const noexcept { return running_; }
  std::string last_error() const;
  std::size_t pending_count() const noexcept;
  std::uintmax_t pending_bytes() const noexcept;
  bool drained() const noexcept
  { if (pending_count()) return false; std::scoped_lock lock(error_mutex_); return error_.empty(); }
  void reject(std::string error) { set_error(std::move(error)); }
 private:
  void run() noexcept;
  void set_error(std::string error);
  ReliableWriterConfig config_;
  std::filesystem::path spool_;
  mutable std::mutex state_mutex_, error_mutex_;
  std::condition_variable cv_;
  std::thread worker_;
  std::atomic_bool running_{false}, stopping_{false};
  std::atomic_uint64_t required_sequence_{0};
  std::string error_;
};
std::filesystem::path checked_relative_target(const std::filesystem::path& root,
    const std::filesystem::path& relative);
std::string unique_storage_id();
std::string stable_digest(const std::string& value);
std::string printer_storage_token(const std::string& printer);
bool valid_artifact_filename(const std::string& value);
}
