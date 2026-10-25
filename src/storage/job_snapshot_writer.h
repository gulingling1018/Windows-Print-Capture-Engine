#pragma once

#include "core/data_contracts.h"
#include "storage/reliable_file_writer.h"
#include <memory>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace wpce::storage
{

struct JobSnapshotWriterConfig
{
  std::filesystem::path output_directory{"wpce-output"};
  // Legacy sizing hint. Pending transactions live on disk, one is processed in RAM.
  std::size_t queue_capacity{256};
  AtomicWriteHook worker_hook;
  bool rotate_daily{false};
  std::function<std::string()> date_provider;
};

/// A job snapshot paired with optional metadata for atomic persistence.
struct SnapshotPair
{
  core::JobSnapshot job;
  core::MetadataSnapshot metadata;
  std::filesystem::path output_directory;
};

std::filesystem::path job_snapshot_path(const JobSnapshotWriterConfig& config, const core::JobSnapshot& snapshot);
std::filesystem::path job_snapshot_archive_path(const JobSnapshotWriterConfig& config, const core::JobSnapshot& snapshot);
std::filesystem::path metadata_snapshot_path(const JobSnapshotWriterConfig& config, const core::MetadataSnapshot& snapshot);
std::filesystem::path metadata_archive_path(const JobSnapshotWriterConfig& config, const core::MetadataSnapshot& snapshot);
std::filesystem::path latest_manifest_path(const JobSnapshotWriterConfig& config);
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
  void request_stop() noexcept;

  [[nodiscard]] bool enqueue(core::JobSnapshot job, core::MetadataSnapshot metadata, const std::string& capture_date = {});
  [[nodiscard]] bool running() const noexcept;
  [[nodiscard]] std::string last_error() const;

  /// Current depth of the internal work queue.
  [[nodiscard]] std::size_t queue_depth() const noexcept;
  [[nodiscard]] bool drained() const noexcept;
  [[nodiscard]] std::uintmax_t pending_bytes() const noexcept;

 private:
  JobSnapshotWriterConfig config_;
  std::unique_ptr<ReliableFileWriter> writer_;
};

}  // namespace wpce::storage
