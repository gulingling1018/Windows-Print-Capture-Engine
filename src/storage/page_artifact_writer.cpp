#include "storage/page_artifact_writer.h"

#include "storage/file_utils.h"
#include "storage/json_serializer.h"

#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace wpce::storage
{
// Path helpers
std::filesystem::path page_artifact_path(const PageArtifactWriterConfig& config)
{
  return config.output_directory / "page-artifacts.json";
}

std::filesystem::path page_artifact_archive_path(
  const PageArtifactWriterConfig& config,
  const core::PageArtifactIndex& index)
{
  const std::string printer = printer_storage_token(index.printer_name);
  std::ostringstream name;
  name << "page-" << printer
       << "-j" << index.job_id
       << "-" << (index.capture_id.empty() ? "legacy" : index.capture_id)
       << "-d" << index.document_sequence
       << "-p" << index.page_sequence << ".json";
  return config.output_directory / "pages" / name.str();
}

// AsyncPageArtifactWriter
AsyncPageArtifactWriter::AsyncPageArtifactWriter(PageArtifactWriterConfig config) : config_(std::move(config))
{
  if (config_.output_directory.empty()) throw std::invalid_argument("output directory required");
  config_.output_directory = std::filesystem::absolute(config_.output_directory).lexically_normal();
  writer_ = std::make_unique<ReliableFileWriter>(ReliableWriterConfig{
      .root=config_.output_directory, .queue_name="pages", .worker_hook=config_.worker_hook});
}
AsyncPageArtifactWriter::~AsyncPageArtifactWriter() { stop(); }
void AsyncPageArtifactWriter::start() { writer_->start(); }
void AsyncPageArtifactWriter::stop() noexcept { writer_->stop(); }
bool AsyncPageArtifactWriter::running() const noexcept { return writer_->running(); }
std::string AsyncPageArtifactWriter::last_error() const { return writer_->last_error(); }
std::size_t AsyncPageArtifactWriter::queue_depth() const noexcept { return writer_->pending_count(); }
std::uintmax_t AsyncPageArtifactWriter::pending_bytes() const noexcept { return writer_->pending_bytes(); }
bool AsyncPageArtifactWriter::drained() const noexcept { return writer_->drained(); }
void AsyncPageArtifactWriter::sync() { stop(); }
bool AsyncPageArtifactWriter::enqueue(PageArtifactPayload payload)
{
  std::vector<FileWrite> writes;
  auto add = [&](const std::string& name, const std::string& content) {
    if (content.empty()) { if (!name.empty()) throw std::runtime_error("index refers to missing payload"); return; }
    if (!valid_artifact_filename(name)) throw std::runtime_error("unsafe artifact name");
    writes.push_back({std::filesystem::path("pages") / name, content});
  };
  try
  {
    if (!payload.fixed_page_xml.empty()) payload.index.fixed_page_state = "reported";
    add(payload.index.job_print_ticket, payload.job_print_ticket_xml);
    add(payload.index.document_print_ticket, payload.document_print_ticket_xml);
    add(payload.index.page_print_ticket, payload.page_print_ticket_xml);
    add(payload.index.fixed_page, payload.fixed_page_xml);
    if (!payload.index.capture_id.empty() && !valid_artifact_filename(payload.index.capture_id)) throw std::runtime_error("unsafe capture identity");
    writes.push_back({page_artifact_archive_path(config_, payload.index).lexically_relative(config_.output_directory), to_json(payload.index)});
    writes.push_back({"page-artifacts.json", to_json(payload.index)});
    return writer_->enqueue(std::move(writes));
  }
  catch (const std::exception& error) { writer_->reject(error.what()); return false; }
}
}  // namespace wpce::storage
