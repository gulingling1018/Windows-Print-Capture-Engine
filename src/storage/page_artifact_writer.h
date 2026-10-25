#pragma once

#include "core/data_contracts.h"
#include "storage/reliable_file_writer.h"
#include <memory>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace wpce::storage
{

// PageArtifactWriterConfig
struct PageArtifactWriterConfig
{
  std::filesystem::path output_directory{"wpce-output"};
  // Legacy sizing hint; pending transactions are durable files.
  std::size_t queue_capacity{512};
  AtomicWriteHook worker_hook;
};

// PageArtifactPayload
/// A single page-level capture payload.
struct PageArtifactPayload
{
  core::PageArtifactIndex index;

  /// Raw PrintTicket XML bytes (empty if not available for this level).
  std::string job_print_ticket_xml;
  std::string document_print_ticket_xml;
  std::string page_print_ticket_xml;

  /// FixedPage markup bytes (empty in lightweight/reference-only mode).
  std::string fixed_page_xml;
};

// Path helpers
std::filesystem::path page_artifact_path(const PageArtifactWriterConfig& config);
std::filesystem::path page_artifact_archive_path(
  const PageArtifactWriterConfig& config,
  const core::PageArtifactIndex& index);

// AsyncPageArtifactWriter
/// Async writer for page artifact payloads.
///
/// Concurrent writers share a durable queue and an exclusive processing lease.
class AsyncPageArtifactWriter
{
 public:
  explicit AsyncPageArtifactWriter(PageArtifactWriterConfig config);
  ~AsyncPageArtifactWriter();

  AsyncPageArtifactWriter(const AsyncPageArtifactWriter&) = delete;
  AsyncPageArtifactWriter& operator=(const AsyncPageArtifactWriter&) = delete;
  AsyncPageArtifactWriter(AsyncPageArtifactWriter&&) = delete;
  AsyncPageArtifactWriter& operator=(AsyncPageArtifactWriter&&) = delete;

  void start();
  void stop() noexcept;

  [[nodiscard]] bool enqueue(PageArtifactPayload payload);
  [[nodiscard]] bool running() const noexcept;
  [[nodiscard]] std::string last_error() const;

  /// Current depth of the internal work queue.
  [[nodiscard]] std::size_t queue_depth() const noexcept;
  [[nodiscard]] bool drained() const noexcept;
  [[nodiscard]] std::uintmax_t pending_bytes() const noexcept;

  /// Block until all queued items have been persisted to disk.
  /// Useful before reading page-artifacts.json to ensure the latest
  /// data is on disk.  For best results, call after stop().
  void sync();

 private:
  PageArtifactWriterConfig config_;
  std::unique_ptr<ReliableFileWriter> writer_;
};

}  // namespace wpce::storage
