#include "storage/page_artifact_writer.h"

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

void write_text_file_atomic(const std::string& content, const std::filesystem::path& path)
{
  ensure_output_directory(path.parent_path());
  const std::filesystem::path temp_path = make_temp_path(path);

  try
  {
    {
      std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
      if (!output)
      {
        throw std::runtime_error("failed to open temporary file: " + path.string());
      }
      output.write(content.data(), static_cast<std::streamsize>(content.size()));
      output.flush();
      if (!output)
      {
        throw std::runtime_error("failed to write temporary file: " + path.string());
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
  const std::filesystem::path index_path = page_artifact_archive_path(config, payload.index);
  const std::filesystem::path temp_path = make_temp_path(index_path);
  ensure_output_directory(index_path.parent_path());

  try
  {
    {
      std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
      if (!output)
      {
        throw std::runtime_error("failed to open temporary index file");
      }
      output << to_json(payload.index);
      output.flush();
      if (!output)
      {
        throw std::runtime_error("failed to write temporary index file");
      }
    }
    replace_file(temp_path, index_path);
  }
  catch (...)
  {
    std::error_code ignored;
    std::filesystem::remove(temp_path, ignored);
    throw;
  }

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
  std::ostringstream name;
  name << "page-j" << index.job_id
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
    if (!running_ || stop_requested_ || queue_.size() >= config_.queue_capacity)
    {
      return false;
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
