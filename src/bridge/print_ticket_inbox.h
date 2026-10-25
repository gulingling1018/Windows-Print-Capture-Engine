#pragma once
#include "core/data_contracts.h"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <unordered_map>
namespace wpce::bridge
{
struct PrintTicketMessage
{
  std::string message_id;
  std::string physical_identity;
  core::MetadataSnapshot metadata;
};
std::string serialize_pt_message(const PrintTicketMessage& message);
bool parse_metadata_json(const std::string& text, core::MetadataSnapshot& metadata);
bool parse_pt_message(const std::string& text, PrintTicketMessage& message);
struct InboxResult
{
  std::size_t delivered{0}, pending{0}, quarantined{0};
  std::vector<std::string> errors;
  std::vector<std::string> acknowledged_ids;
};
// Handler true means the application durably accepted this physical message.
// Budget applies to each distinct directory so a busy root cannot starve peers.
InboxResult consume_pt_inbox(const std::vector<std::filesystem::path>& roots,
    const std::function<bool(const PrintTicketMessage&)>& handler, std::size_t budget = 16);
std::vector<std::filesystem::path> pt_inbox_directories(const std::filesystem::path& output);
std::uint32_t match_pt_job(const PrintTicketMessage& message,
    const std::unordered_map<std::uint32_t, core::JobSnapshot>& jobs);
}
