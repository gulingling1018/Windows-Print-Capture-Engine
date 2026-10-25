#include "bridge/print_ticket_inbox.h"
#include "storage/reliable_file_writer.h"
#include "storage/json_reader.h"
#include "storage/json_serializer.h"
#include "common/physical_job_identity.h"
#include <windows.h>
#include <fstream>
#include <set>
#include <sstream>
#include <algorithm>

namespace wpce::bridge
{
std::uint32_t match_pt_job(const PrintTicketMessage& message,
    const std::unordered_map<std::uint32_t, core::JobSnapshot>& jobs)
{
  if (message.physical_identity.empty()) return 0;
  for (const auto& [id, job] : jobs)
    if ((job.external_job_id ? job.external_job_id : job.job_id) == message.metadata.job_id &&
        job.printer_name == message.metadata.printer_name && common::physical_job_identity(job) == message.physical_identity)
      return id;
  return 0;
}
namespace
{
core::FieldValue read_field(storage::JsonReader& reader)
{
  core::FieldValue field; reader.beginObject();
  while (reader.endObjectOrNext())
  {
    const auto key = reader.readKey();
    if (key == "value") field.value = reader.readString();
    else if (key == "state")
    {
      const auto state = reader.readString();
      if (state == "reported") field.state = core::FieldState::Reported;
      else if (state == "unsupported") field.state = core::FieldState::Unsupported;
      else if (state == "not_reported") field.state = core::FieldState::NotReported;
      else if (state == "error") field.state = core::FieldState::Error;
    }
    else reader.skipValue();
  }
  return field;
}
std::string read_file(const std::filesystem::path& path)
{
  if (std::filesystem::file_size(path) > 256 * 1024) throw std::runtime_error("PrintTicket message exceeds limit");
  std::ifstream input(path, std::ios::binary); std::ostringstream out; out << input.rdbuf();
  if (!input || input.bad()) throw std::runtime_error("PrintTicket message unreadable");
  return out.str();
}
bool regular_unlinked(const std::filesystem::path& path)
{
  const auto attr = GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && !(attr & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY));
}
}
bool parse_metadata_json(const std::string& text, core::MetadataSnapshot& output)
{
  try
  {
    core::MetadataSnapshot metadata; bool schema = false;
    std::istringstream input(text); storage::JsonReader reader(input); reader.beginObject();
    while (reader.endObjectOrNext())
    {
      const auto key = reader.readKey();
      if (key == "schema_version") { metadata.schema_version = reader.readInt(); schema = true; }
      else if (key == "job_id") metadata.job_id = reader.readUInt32();
      else if (key == "printer_name") metadata.printer_name = reader.readString();
      else if (key == "dm_fields_hex") metadata.dm_fields_hex = reader.readString();
      else if (key == "file_extension_source") metadata.file_extension_source = reader.readString();
      else if (key == "file_extension_confidence") metadata.file_extension_confidence = reader.readInt();
      else
      {
        core::FieldValue* field = nullptr;
        if (key == "total_pages") field = &metadata.total_pages;
        else if (key == "pages_printed") field = &metadata.pages_printed;
        else if (key == "copies") field = &metadata.copies;
        else if (key == "duplex") field = &metadata.duplex;
        else if (key == "color") field = &metadata.color;
        else if (key == "paper_size") field = &metadata.paper_size;
        else if (key == "orientation") field = &metadata.orientation;
        else if (key == "n_up") field = &metadata.n_up;
        else if (key == "collate") field = &metadata.collate;
        else if (key == "print_quality") field = &metadata.print_quality;
        else if (key == "media_type") field = &metadata.media_type;
        else if (key == "page_range") field = &metadata.page_range;
        else if (key == "file_extension") field = &metadata.file_extension;
        if (field) *field = read_field(reader); else reader.skipValue();
      }
    }
    reader.finish();
    if (!schema || metadata.schema_version != 1 || !metadata.job_id || metadata.printer_name.empty()) return false;
    output = std::move(metadata); return true;
  }
  catch (...) { return false; }
}
std::string serialize_pt_message(const PrintTicketMessage& message)
{
  return "{\"message_id\":" + storage::json_escape(message.message_id) +
      ",\"physical_identity\":" + storage::json_escape(message.physical_identity) +
      ",\"metadata\":" + storage::to_json(message.metadata) + "}\n";
}
bool parse_pt_message(const std::string& text, PrintTicketMessage& output)
{
  try
  {
    PrintTicketMessage message; bool metadata = false;
    std::istringstream input(text); storage::JsonReader reader(input); reader.beginObject();
    while (reader.endObjectOrNext())
    {
      const auto key = reader.readKey();
      if (key == "message_id") message.message_id = reader.readString();
      else if (key == "physical_identity") message.physical_identity = reader.readString();
      else if (key == "metadata") metadata = parse_metadata_json(reader.readRawValue(), message.metadata);
      else reader.skipValue();
    }
    reader.finish();
    if (!metadata || message.message_id.empty()) return false;
    output = std::move(message); return true;
  }
  catch (...) { return false; }
}
InboxResult consume_pt_inbox(const std::vector<std::filesystem::path>& roots,
    const std::function<bool(const PrintTicketMessage&)>& handler, std::size_t budget)
{
  InboxResult result; std::set<std::filesystem::path> visited;
  for (const auto& root : roots)
  {
    try
    {
      if (!budget || !std::filesystem::is_directory(root)) continue;
      const auto canonical = std::filesystem::weakly_canonical(root);
      if (!visited.insert(canonical).second) continue;
      storage::DirectoryLease lease(canonical / "pt-inbox.lock"); if (!lease) continue;
      const auto cursorFile = canonical / "_pt-cursor.txt";
      const auto cursor = std::filesystem::exists(cursorFile) ? read_file(cursorFile) : std::string{};
      std::vector<std::filesystem::path> candidates;
      std::vector<std::filesystem::path> wrapped;
      auto insert = [budget](auto& paths, const auto& path) {
        paths.insert(std::lower_bound(paths.begin(), paths.end(), path), path);
        if (paths.size() > budget) paths.pop_back();
      };
      for (const auto& entry : std::filesystem::directory_iterator(canonical))
      {
        const auto name = entry.path().filename().string();
        if (regular_unlinked(entry.path()) && (name.ends_with(".pt-message.json") || name.ends_with(".pt-message.json.claim") ||
            (name.starts_with("job-") && (name.ends_with("-pt-metadata.json") || name.ends_with("-pt-metadata.json.claim")))))
        {
          if (name > cursor) insert(candidates, entry.path());
          else insert(wrapped, entry.path());
        }
      }
      if (candidates.empty()) candidates.swap(wrapped);
      if (!candidates.empty()) storage::write_text_file_atomic(candidates.back().filename().string(), cursorFile);
      for (auto file : candidates)
      {
        if (!file.filename().string().ends_with(".claim"))
        {
          auto claim = file; claim += ".claim";
          if (!MoveFileExW(file.c_str(), claim.c_str(), 0)) { ++result.pending; continue; }
          file = std::move(claim);
        }
        PrintTicketMessage message;
        const auto text = read_file(file);
        auto quarantine = [&] {
          const auto directory = canonical / "_pt-quarantine"; storage::ensure_output_directory(directory);
          std::filesystem::rename(file, directory / (storage::unique_storage_id() + ".json")); ++result.quarantined;
        };
        if (!parse_pt_message(text, message) || message.physical_identity.empty()) { quarantine(); continue; }
        const auto digest = storage::stable_digest(text);
        const auto ack = canonical / "_pt-acks" / (storage::stable_digest(message.message_id) + ".ack");
        if (std::filesystem::exists(ack))
        {
          result.acknowledged_ids.push_back(message.message_id);
          if (read_file(ack) == digest) std::filesystem::remove(file);
          else quarantine();
          continue;
        }
        bool accepted = false;
        try { accepted = handler && handler(message); }
        catch (...) { result.errors.push_back("PrintTicket delivery failed; claim retained"); }
        if (!accepted) { ++result.pending; continue; }
        storage::write_text_file_atomic(digest, ack);
        result.acknowledged_ids.push_back(message.message_id);
        std::filesystem::remove(file); ++result.delivered;
      }
    }
    catch (const std::exception& error) { result.errors.push_back(error.what()); }
  }
  return result;
}
}
