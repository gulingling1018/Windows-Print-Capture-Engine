#include "storage/job_snapshot_writer.h"

#include "storage/json_serializer.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace wpce::storage
{
namespace
{

std::filesystem::path ensure_output_directory(const std::filesystem::path& directory)
{
  std::filesystem::create_directories(directory);
  return directory;
}

std::filesystem::path make_temp_path(const std::filesystem::path& final_path)
{
  std::ostringstream suffix;
  suffix << ".tmp." << std::hash<std::thread::id>{}(std::this_thread::get_id());
  return final_path.string() + suffix.str();
}

void replace_file(const std::filesystem::path& temp_path, const std::filesystem::path& final_path)
{
#ifdef _WIN32
  if (!MoveFileExW(
        temp_path.wstring().c_str(),
        final_path.wstring().c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
  {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "MoveFileExW");
  }
#else
  std::filesystem::rename(temp_path, final_path);
#endif
}

void write_snapshot_files(const JobSnapshotWriterConfig& config, const SnapshotPair& pair)
{
  write_job_snapshot_json_atomic(pair.job, job_snapshot_path(config));
  write_job_snapshot_json_atomic(pair.job, job_snapshot_archive_path(config, pair.job));
  write_metadata_json_atomic(pair.metadata, metadata_snapshot_path(config));
  write_metadata_json_atomic(pair.metadata, metadata_archive_path(config, pair.metadata));
}

}  // namespace

std::filesystem::path job_snapshot_path(const JobSnapshotWriterConfig& config)
{
  return config.output_directory / "job.json";
}

std::filesystem::path job_snapshot_archive_path(
  const JobSnapshotWriterConfig& config,
  const core::JobSnapshot& snapshot)
{
  return config.output_directory / "jobs" / ("job-" + std::to_string(snapshot.job_id) + ".json");
}

std::filesystem::path metadata_snapshot_path(const JobSnapshotWriterConfig& config)
{
  return config.output_directory / "metadata.json";
}

std::filesystem::path metadata_archive_path(
  const JobSnapshotWriterConfig& config,
  const core::MetadataSnapshot& snapshot)
{
  return config.output_directory / "jobs" / ("job-" + std::to_string(snapshot.job_id) + "-metadata.json");
}

std::filesystem::path lifecycle_summary_path(const JobSnapshotWriterConfig& config)
{
  return config.output_directory / "lifecycle-summary.json";
}

std::filesystem::path lifecycle_summary_archive_path(
  const JobSnapshotWriterConfig& config,
  const core::JobLifecycleSummary& summary)
{
  return config.output_directory / "jobs" / ("job-" + std::to_string(summary.job_id) + "-lifecycle.json");
}

void write_lifecycle_summaries_atomic(
  const std::vector<core::JobLifecycleSummary>& summaries,
  const JobSnapshotWriterConfig& config)
{
  ensure_output_directory(config.output_directory);

  // Write per-job lifecycle archives.
  for (const core::JobLifecycleSummary& summary : summaries)
  {
    const std::filesystem::path archive_path = lifecycle_summary_archive_path(config, summary);
    const std::filesystem::path temp_path = make_temp_path(archive_path);
    ensure_output_directory(archive_path.parent_path());

    try
    {
      {
        std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
        if (!output)
        {
          throw std::runtime_error("failed to open temporary lifecycle summary file");
        }
        output << to_json(summary);
        output.flush();
        if (!output)
        {
          throw std::runtime_error("failed to write temporary lifecycle summary file");
        }
      }
      replace_file(temp_path, archive_path);
    }
    catch (...)
    {
      std::error_code ignored;
      std::filesystem::remove(temp_path, ignored);
      throw;
    }
  }

  // Write a session-level lifecycle manifest.
  const std::filesystem::path manifest_path = lifecycle_summary_path(config);
  const std::filesystem::path temp_path = make_temp_path(manifest_path);

  try
  {
    {
      std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
      if (!output)
      {
        throw std::runtime_error("failed to open temporary lifecycle manifest file");
      }

      output << "{\n  \"session_jobs\": [\n";
      for (std::size_t i = 0; i < summaries.size(); ++i)
      {
        output << "    {\n      \"job_id\": " << summaries[i].job_id
               << ",\n      \"final_phase\": \"" << core::to_string(summaries[i].final_phase)
               << "\",\n      \"event_count\": " << summaries[i].events.size()
               << ",\n      \"file\": \"jobs/job-" << summaries[i].job_id << "-lifecycle.json\"\n    }";
        if (i + 1 < summaries.size())
        {
          output << ',';
        }
        output << '\n';
      }
      output << "  ]\n}\n";
      output.flush();

      if (!output)
      {
        throw std::runtime_error("failed to write temporary lifecycle manifest file");
      }
    }
    replace_file(temp_path, manifest_path);
  }
  catch (...)
  {
    std::error_code ignored;
    std::filesystem::remove(temp_path, ignored);
    throw;
  }
}

void write_metadata_json_atomic(const core::MetadataSnapshot& snapshot, const std::filesystem::path& path)
{
  ensure_output_directory(path.parent_path());
  const std::filesystem::path temp_path = make_temp_path(path);

  try
  {
    {
      std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
      if (!output)
      {
        throw std::runtime_error("failed to open temporary metadata snapshot file");
      }

      output << to_json(snapshot);
      output.flush();
      if (!output)
      {
        throw std::runtime_error("failed to write temporary metadata snapshot file");
      }
    }

    replace_file(temp_path, path);
  }
  catch (...)
  {
    std::error_code ignored;
    std::filesystem::remove(temp_path, ignored);
    throw;
  }
}

void write_job_snapshot_json_atomic(const core::JobSnapshot& snapshot, const std::filesystem::path& path)
{
  ensure_output_directory(path.parent_path());
  const std::filesystem::path temp_path = make_temp_path(path);

  try
  {
    {
      std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
      if (!output)
      {
        throw std::runtime_error("failed to open temporary job snapshot file");
      }

      output << to_json(snapshot);
      output.flush();
      if (!output)
      {
        throw std::runtime_error("failed to write temporary job snapshot file");
      }
    }

    replace_file(temp_path, path);
  }
  catch (...)
  {
    std::error_code ignored;
    std::filesystem::remove(temp_path, ignored);
    throw;
  }
}

AsyncJobSnapshotWriter::AsyncJobSnapshotWriter(JobSnapshotWriterConfig config) : config_(std::move(config))
{
  if (config_.output_directory.empty())
  {
    throw std::invalid_argument("output_directory is required");
  }
  if (config_.queue_capacity == 0)
  {
    throw std::invalid_argument("queue_capacity must be greater than zero");
  }
}

AsyncJobSnapshotWriter::~AsyncJobSnapshotWriter()
{
  stop();
}

void AsyncJobSnapshotWriter::start()
{
  std::scoped_lock lock(lifecycle_mutex_);
  if (running_)
  {
    return;
  }

  stop_requested_ = false;
  set_last_error({});
  running_ = true;
  worker_ = std::thread(&AsyncJobSnapshotWriter::run, this);
}

void AsyncJobSnapshotWriter::stop() noexcept
{
  {
    std::scoped_lock lock(queue_mutex_);
    stop_requested_ = true;
  }
  queue_cv_.notify_all();

  std::thread worker;
  {
    std::scoped_lock lock(lifecycle_mutex_);
    if (worker_.joinable())
    {
      worker = std::move(worker_);
    }
  }

  if (worker.joinable())
  {
    worker.join();
  }
}

bool AsyncJobSnapshotWriter::enqueue(core::JobSnapshot job, core::MetadataSnapshot metadata)
{
  {
    std::scoped_lock lock(queue_mutex_);
    if (!running_ || stop_requested_ || queue_.size() >= config_.queue_capacity)
    {
      return false;
    }

    queue_.push_back(SnapshotPair{std::move(job), std::move(metadata)});
  }

  queue_cv_.notify_one();
  return true;
}

bool AsyncJobSnapshotWriter::running() const noexcept
{
  return running_;
}

std::string AsyncJobSnapshotWriter::last_error() const
{
  std::scoped_lock lock(error_mutex_);
  return last_error_;
}

void AsyncJobSnapshotWriter::run()
{
  try
  {
    ensure_output_directory(config_.output_directory);

    while (true)
    {
      SnapshotPair pair;
      {
        std::unique_lock lock(queue_mutex_);
        queue_cv_.wait(lock, [&]() {
          return stop_requested_ || !queue_.empty();
        });

        if (queue_.empty())
        {
          if (stop_requested_)
          {
            break;
          }
          continue;
        }

        pair = std::move(queue_.front());
        queue_.pop_front();
      }

      write_snapshot_files(config_, pair);
    }
  }
  catch (const std::exception& error)
  {
    set_last_error(error.what());
  }
  catch (...)
  {
    set_last_error("unknown job snapshot writer error");
  }

  running_ = false;
}

void AsyncJobSnapshotWriter::set_last_error(std::string error)
{
  std::scoped_lock lock(error_mutex_);
  last_error_ = std::move(error);
}

}  // namespace wpce::storage
