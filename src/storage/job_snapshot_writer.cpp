#include "storage/job_snapshot_writer.h"

#include "storage/file_utils.h"
#include "storage/json_serializer.h"
#include "common/business_clock.h"

#include <sstream>
#include <stdexcept>
#include <utility>

namespace wpce::storage
{
namespace
{

std::filesystem::path latest_printer_dir(const JobSnapshotWriterConfig& config,
                                         const std::string& printer_name)
{
  return config.output_directory / "latest" / printer_storage_token(printer_name);
}

std::filesystem::path archive_printer_dir(const JobSnapshotWriterConfig& config,
                                          const std::string& printer_name)
{
  return config.output_directory / "jobs" / printer_storage_token(printer_name);
}

std::string relative_generic(const std::filesystem::path& base,
                             const std::filesystem::path& path)
{
  std::error_code ec;
  const auto rel = std::filesystem::relative(path, base, ec);
  return (ec ? path : rel).generic_string();
}

std::string latest_manifest_content(const JobSnapshotWriterConfig& config,
                           const SnapshotPair& pair)
{
  const auto job_path = job_snapshot_path(config, pair.job);
  const auto metadata_path = metadata_snapshot_path(config, pair.metadata);

  std::ostringstream manifest;
  manifest << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"job_id\": " << pair.job.job_id << ",\n"
           << "  \"printer_name\": " << json_escape(pair.job.printer_name) << ",\n"
           << "  \"job\": " << json_escape(relative_generic(config.output_directory, job_path)) << ",\n"
           << "  \"metadata\": " << json_escape(relative_generic(config.output_directory, metadata_path)) << ",\n"
           << "  \"submit_time_utc\": " << json_escape(pair.job.submit_time_utc) << "\n"
           << "}\n";
  return manifest.str();
}

}  // namespace

std::filesystem::path job_snapshot_path(
  const JobSnapshotWriterConfig& config,
  const core::JobSnapshot& snapshot)
{
  return latest_printer_dir(config, snapshot.printer_name) / "job.json";
}

std::filesystem::path job_snapshot_archive_path(
  const JobSnapshotWriterConfig& config,
  const core::JobSnapshot& snapshot)
{
  return archive_printer_dir(config, snapshot.printer_name) /
         ("job-" + std::to_string(snapshot.job_id) + ".json");
}

std::filesystem::path metadata_snapshot_path(
  const JobSnapshotWriterConfig& config,
  const core::MetadataSnapshot& snapshot)
{
  return latest_printer_dir(config, snapshot.printer_name) / "metadata.json";
}

std::filesystem::path metadata_archive_path(
  const JobSnapshotWriterConfig& config,
  const core::MetadataSnapshot& snapshot)
{
  return archive_printer_dir(config, snapshot.printer_name) /
         ("job-" + std::to_string(snapshot.job_id) + "-metadata.json");
}

std::filesystem::path latest_manifest_path(const JobSnapshotWriterConfig& config)
{
  return config.output_directory / "latest" / "manifest.json";
}

std::filesystem::path lifecycle_summary_path(const JobSnapshotWriterConfig& config)
{
  return config.output_directory / "lifecycle-summary.json";
}

std::filesystem::path lifecycle_summary_archive_path(
  const JobSnapshotWriterConfig& config,
  const core::JobLifecycleSummary& summary)
{
  return archive_printer_dir(config, summary.printer_name) /
         ("job-" + std::to_string(summary.job_id) + "-lifecycle.json");
}

void write_lifecycle_summaries_atomic(
  const std::vector<core::JobLifecycleSummary>& summaries,
  const JobSnapshotWriterConfig& config)
{
  ensure_output_directory(config.output_directory);

  // Write per-job lifecycle archives.
  for (const core::JobLifecycleSummary& summary : summaries)
  {
    write_text_file_atomic(to_json(summary), lifecycle_summary_archive_path(config, summary));
  }

  // Write a session-level lifecycle manifest.
  std::ostringstream manifest;
  manifest << "{\n  \"session_jobs\": [\n";
  for (std::size_t i = 0; i < summaries.size(); ++i)
  {
    const auto archive_path = lifecycle_summary_archive_path(config, summaries[i]);
    manifest << "    {\n      \"job_id\": " << summaries[i].job_id
             << ",\n      \"final_phase\": \"" << core::to_string(summaries[i].final_phase)
             << "\",\n      \"event_count\": " << summaries[i].events.size()
             << ",\n      \"file\": "
             << json_escape(relative_generic(config.output_directory, archive_path))
             << "\n    }";
    if (i + 1 < summaries.size())
    {
      manifest << ',';
    }
    manifest << '\n';
  }
  manifest << "  ]\n}\n";

  write_text_file_atomic(manifest.str(), lifecycle_summary_path(config));
}

void write_metadata_json_atomic(const core::MetadataSnapshot& snapshot, const std::filesystem::path& path)
{
  write_text_file_atomic(to_json(snapshot), path);
}

void write_job_snapshot_json_atomic(const core::JobSnapshot& snapshot, const std::filesystem::path& path)
{
  write_text_file_atomic(to_json(snapshot), path);
}

AsyncJobSnapshotWriter::AsyncJobSnapshotWriter(JobSnapshotWriterConfig config) : config_(std::move(config))
{
  if (config_.output_directory.empty()) throw std::invalid_argument("output directory required");
  config_.output_directory = std::filesystem::absolute(config_.output_directory).lexically_normal();
  writer_ = std::make_unique<ReliableFileWriter>(ReliableWriterConfig{
      .root = config_.rotate_daily ? config_.output_directory.parent_path() : config_.output_directory,
      .queue_name = "snapshots", .worker_hook = config_.worker_hook});
}
AsyncJobSnapshotWriter::~AsyncJobSnapshotWriter() { stop(); }
void AsyncJobSnapshotWriter::start() { writer_->start(); }
void AsyncJobSnapshotWriter::stop() noexcept { writer_->stop(); }
void AsyncJobSnapshotWriter::request_stop() noexcept { writer_->request_stop(); }
bool AsyncJobSnapshotWriter::running() const noexcept { return writer_->running(); }
std::string AsyncJobSnapshotWriter::last_error() const { return writer_->last_error(); }
std::size_t AsyncJobSnapshotWriter::queue_depth() const noexcept { return writer_->pending_count(); }
std::uintmax_t AsyncJobSnapshotWriter::pending_bytes() const noexcept { return writer_->pending_bytes(); }
bool AsyncJobSnapshotWriter::drained() const noexcept { return writer_->drained(); }
bool AsyncJobSnapshotWriter::enqueue(core::JobSnapshot job, core::MetadataSnapshot metadata, const std::string& capture_date)
{
  auto config = config_;
  const auto root = config_.rotate_daily ? config_.output_directory.parent_path() : config_.output_directory;
  if (config_.rotate_daily)
  {
    const auto date = !capture_date.empty() ? capture_date : (config_.date_provider ? config_.date_provider() : common::current_business_date().yyyy_mm_dd);
    if (!common::parse_business_date(date)) { writer_->reject("invalid capture date"); return false; }
    config.output_directory = root / date;
  }
  const SnapshotPair pair{std::move(job), std::move(metadata), {}};
  auto operation = [&](const auto& path, const auto& content) { return FileWrite{path.lexically_relative(root), content}; };
  return writer_->enqueue({operation(job_snapshot_path(config, pair.job), to_json(pair.job)),
      operation(job_snapshot_archive_path(config, pair.job), to_json(pair.job)),
      operation(metadata_snapshot_path(config, pair.metadata), to_json(pair.metadata)),
      operation(metadata_archive_path(config, pair.metadata), to_json(pair.metadata)),
      operation(latest_manifest_path(config), latest_manifest_content(config, pair))});
}
}  // namespace wpce::storage
