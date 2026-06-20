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
#include <vector>

namespace wpce::storage
{

// ── PageArtifactWriterConfig ──────────────────────────────────────────

struct PageArtifactWriterConfig
{
  std::filesystem::path output_directory{"wpce-output"};
  std::size_t queue_capacity{512};
};

// ── PageArtifactPayload ───────────────────────────────────────────────

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

// ── Path helpers ──────────────────────────────────────────────────────

std::filesystem::path page_artifact_path(const PageArtifactWriterConfig& config);
std::filesystem::path page_artifact_archive_path(
  const PageArtifactWriterConfig& config,
  const core::PageArtifactIndex& index);

// ── AsyncPageArtifactWriter ───────────────────────────────────────────

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

 private:
  void run();
  void set_last_error(std::string error);

  PageArtifactWriterConfig config_;
  std::deque<PageArtifactPayload> queue_;
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
