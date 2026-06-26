#include "storage/page_artifact_writer.h"

#include "storage/file_utils.h"
#include "storage/json_serializer.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace wpce::storage
{
namespace
{

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

void persist_payload_files(const PageArtifactWriterConfig& config, const PageArtifactPayload& payload)
{
  const std::filesystem::path pages_dir = config.output_directory / "pages";

  // Write PrintTicket XML files.
  if (!payload.job_print_ticket_xml.empty())
  {
    write_text_file_atomic(payload.job_print_ticket_xml,
                           pages_dir / payload.index.job_print_ticket);
  }
  if (!payload.document_print_ticket_xml.empty())
  {
    write_text_file_atomic(payload.document_print_ticket_xml,
                           pages_dir / payload.index.document_print_ticket);
  }
  if (!payload.page_print_ticket_xml.empty())
  {
    write_text_file_atomic(payload.page_print_ticket_xml,
                           pages_dir / payload.index.page_print_ticket);
  }
  if (!payload.fixed_page_xml.empty())
  {
    write_text_file_atomic(payload.fixed_page_xml,
                           pages_dir / payload.index.fixed_page);
  }

  // Write the PageArtifactIndex JSON.
  write_text_file_atomic(to_json(payload.index), page_artifact_archive_path(config, payload.index));

  // Update the latest page artifact index.
  write_text_file_atomic(to_json(payload.index), page_artifact_path(config));
}

}  // namespace

// ── Path helpers ──────────────────────────────────────────────────────

std::filesystem::path page_artifact_path(const PageArtifactWriterConfig& config)
{
  return config.output_directory / "page-artifacts.json";
}

std::filesystem::path page_artifact_archive_path(
  const PageArtifactWriterConfig& config,
  const core::PageArtifactIndex& index)
{
  const std::string printer = safe_filename_token(index.printer_name);
  std::ostringstream name;
  name << "page-" << printer
       << "-j" << index.job_id
       << "-d" << index.document_sequence
       << "-p" << index.page_sequence << ".json";
  return config.output_directory / "pages" / name.str();
}

// ── AsyncPageArtifactWriter ───────────────────────────────────────────

AsyncPageArtifactWriter::AsyncPageArtifactWriter(PageArtifactWriterConfig config)
  : config_(std::move(config))
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

AsyncPageArtifactWriter::~AsyncPageArtifactWriter()
{
  stop();
}

void AsyncPageArtifactWriter::start()
{
  std::scoped_lock lock(lifecycle_mutex_);
  if (running_)
  {
    return;
  }

  stop_requested_ = false;
  set_last_error({});
  running_ = true;
  worker_ = std::thread(&AsyncPageArtifactWriter::run, this);
}

void AsyncPageArtifactWriter::stop() noexcept
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

bool AsyncPageArtifactWriter::enqueue(PageArtifactPayload payload)
{
  {
    std::scoped_lock lock(queue_mutex_);
    if (!running_ || stop_requested_)
    {
      return false;
    }
    if (queue_.size() >= config_.queue_capacity)
    {
      // Log drop so diagnostics can confirm queue backpressure under large-file load.
      (void)fprintf(stderr,
                    "[WPCE] PageArtifactWriter queue FULL (%zu/%zu), dropping payload\n",
                    queue_.size(), config_.queue_capacity);
      return false;
    }
    if (queue_.size() >= config_.queue_capacity * 8 / 10)
    {
      (void)fprintf(stderr,
                    "[WPCE] PageArtifactWriter queue high-water (%zu/%zu)\n",
                    queue_.size(), config_.queue_capacity);
    }

    queue_.push_back(std::move(payload));
  }

  queue_cv_.notify_one();
  return true;
}

bool AsyncPageArtifactWriter::running() const noexcept
{
  return running_;
}

std::string AsyncPageArtifactWriter::last_error() const
{
  std::scoped_lock lock(error_mutex_);
  return last_error_;
}

std::size_t AsyncPageArtifactWriter::queue_depth() const noexcept
{
  std::scoped_lock lock(queue_mutex_);
  return queue_.size();
}

void AsyncPageArtifactWriter::run()
{
  try
  {
    ensure_output_directory(config_.output_directory);
    ensure_output_directory(config_.output_directory / "pages");

    while (true)
    {
      PageArtifactPayload payload;
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

        payload = std::move(queue_.front());
        queue_.pop_front();
      }

      persist_payload_files(config_, payload);
    }
  }
  catch (const std::exception& error)
  {
    set_last_error(error.what());
  }
  catch (...)
  {
    set_last_error("unknown page artifact writer error");
  }

  running_ = false;
}

void AsyncPageArtifactWriter::set_last_error(std::string error)
{
  std::scoped_lock lock(error_mutex_);
  last_error_ = std::move(error);
}

}  // namespace wpce::storage
