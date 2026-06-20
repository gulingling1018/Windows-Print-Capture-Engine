#pragma once

#include "core/data_contracts.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace wpce::storage
{

struct JobSnapshotWriterConfig
{
  std::filesystem::path output_directory{"wpce-output"};
  std::size_t queue_capacity{256};
};

/// A job snapshot paired with optional metadata for atomic persistence.
struct SnapshotPair
{
  core::JobSnapshot job;
  core::MetadataSnapshot metadata;
};

std::filesystem::path job_snapshot_path(const JobSnapshotWriterConfig& config);
std::filesystem::path job_snapshot_archive_path(const JobSnapshotWriterConfig& config, const core::JobSnapshot& snapshot);
std::filesystem::path metadata_snapshot_path(const JobSnapshotWriterConfig& config);
std::filesystem::path metadata_archive_path(const JobSnapshotWriterConfig& config, const core::MetadataSnapshot& snapshot);
std::filesystem::path lifecycle_summary_path(const JobSnapshotWriterConfig& config);
std::filesystem::path lifecycle_summary_archive_path(const JobSnapshotWriterConfig& config, const core::JobLifecycleSummary& summary);
void write_job_snapshot_json_atomic(const core::JobSnapshot& snapshot, const std::filesystem::path& path);
void write_metadata_json_atomic(const core::MetadataSnapshot& snapshot, const std::filesystem::path& path);
void write_lifecycle_summaries_atomic(const std::vector<core::JobLifecycleSummary>& summaries, const JobSnapshotWriterConfig& config);

class AsyncJobSnapshotWriter
{
 public:
  explicit AsyncJobSnapshotWriter(JobSnapshotWriterConfig config);
  ~AsyncJobSnapshotWriter();

  AsyncJobSnapshotWriter(const AsyncJobSnapshotWriter&) = delete;
  AsyncJobSnapshotWriter& operator=(const AsyncJobSnapshotWriter&) = delete;
  AsyncJobSnapshotWriter(AsyncJobSnapshotWriter&&) = delete;
  AsyncJobSnapshotWriter& operator=(AsyncJobSnapshotWriter&&) = delete;

  void start();
  void stop() noexcept;

  [[nodiscard]] bool enqueue(core::JobSnapshot job, core::MetadataSnapshot metadata);
  [[nodiscard]] bool running() const noexcept;
  [[nodiscard]] std::string last_error() const;

 private:
  void run();
  void set_last_error(std::string error);

  JobSnapshotWriterConfig config_;
  std::deque<SnapshotPair> queue_;
  std::atomic_bool stop_requested_{false};
  std::atomic_bool running_{false};
  std::thread worker_;
  mutable std::mutex lifecycle_mutex_;
  mutable std::mutex queue_mutex_;
  mutable std::mutex error_mutex_;
  std::condition_variable queue_cv_;
  std::string last_error_;
};

}  // namespace wpce::storage
