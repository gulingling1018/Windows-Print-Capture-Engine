/// @file metadata_merger.h
/// Merge PrintTicket-derived metadata with spooler lifecycle data.
///
/// The wpce_xps_filter.dll writes per-job PrintTicket metadata as
/// `job-<printer>-N-pt-metadata.json` in the output directory.  This module
/// scans for those files and merges their contents into the table
/// model's existing job rows.
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
/// @param cleanup_after_load  If true, delete the file after successful load.
///                            Recommended: true — prevents file accumulation
///                            and avoids re-processing after watcher restart.
/// @return                    Number of files found and merged.
int scan_and_merge_pt_metadata(const std::filesystem::path& output_dir,
                               MergedMetadataCallback callback,
                               bool cleanup_after_load = false);

/// Parse a single PrintTicket metadata JSON file into a MetadataSnapshot.
/// Returns true on success.
bool load_pt_metadata(const std::filesystem::path& json_path,
                      core::MetadataSnapshot& out);

}  // namespace wpce::bridge
