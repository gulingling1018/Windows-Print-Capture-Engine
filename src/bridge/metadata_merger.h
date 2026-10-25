/// @file metadata_merger.h
/// Merge PrintTicket-derived metadata with spooler lifecycle data.
///
/// The filter writes uniquely identified `<capture-id>.pt-message.json`
/// envelopes. Production delivery uses PrintTicketInbox with physical job
/// matching and durable business acknowledgement. This module retains
/// legacy read-only loading and inbox directory discovery.
///
/// This is the bridge between Layer 1 (spooler watcher) and
/// Layer 2 (XPS pipeline filter) of the two-layer architecture.

#pragma once

#include "core/data_contracts.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace wpce::bridge
{

/// Callback invoked when PrintTicket metadata is merged into a job.
/// Receives the updated MetadataSnapshot with PrintTicket fields populated.
using MergedMetadataCallback = std::function<void(core::MetadataSnapshot)>;

/// Scan the output directory for PrintTicket metadata files written by
/// the XPS filter DLL, parse them, and invoke the callback for each.
///
/// Thread-safe: may be called from any thread.
///
/// @param output_dir          The session output directory (e.g. ./data/capture-xxx/).
/// @param callback            Called once per metadata file found.
/// @param cleanup_after_load  If true, use the durable inbox protocol. A callback
///                            must persist delivery before returning; throwing
///                            retains the claim. Identity-less legacy input is
///                            quarantined. Prefer PrintTicketInbox in new code.
/// @return                    Number of files found and merged.
int scan_and_merge_pt_metadata(const std::filesystem::path& output_dir,
                               MergedMetadataCallback callback,
                               bool cleanup_after_load = false);

/// Parse a single PrintTicket metadata JSON file into a MetadataSnapshot.
/// Returns true on success.
bool load_pt_metadata(const std::filesystem::path& json_path,
                      core::MetadataSnapshot& out);

}  // namespace wpce::bridge
