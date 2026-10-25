/// @file spool_file_parser.h
/// Parse Windows EMF spool files to extract page count and DEVMODE.
///
/// When pDevMode in JOB_INFO_2W is NULL (v4 drivers) or when certain
/// applications (e.g. Microsoft Word) set dmCopies=1 while handling
/// multi-copy internally, the .SPL spool file in the Windows spooler
/// directory is often the only source of the true per-job DEVMODE.
///
/// EMF spool files (datatype "NT EMF 1.008") use a record-based format:
///   - EMRI_DEVMODE (0x09)          -> embedded per-job DEVMODE
///   - EMRI_METAFILE_DATA (0x01)    -> one record per rendered page
///   - EMRI_METAFILE_EXT (0x02)     -> extended metafile (also a page)
///
/// This parser extracts the DEVMODE and counts pages from the SPL file,
/// then maps the results to a MetadataSnapshot for merging.
///
/// Requires read access to %WINDIR%\System32\spool\PRINTERS\.
/// Degrades gracefully when access is denied.

#pragma once

#include "core/data_contracts.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace wpce::parser
{

/// Result of parsing one SPL spool file.
struct SpoolFileInfo
{
  std::filesystem::path path;
  std::uint64_t file_size = 0;

  /// Detected spool data format.
  enum class Format { Unknown, EMF, RAW, PostScript, PCL, XPS };
  Format format = Format::Unknown;

  /// Page count from EMF record parsing.  -1 = not available.
  int emf_page_count = -1;

  /// DEVMODE fields extracted from the spool file's EMRI_DEVMODE record.
  bool has_devmode = false;
  std::int16_t dm_copies = -1;
  std::int16_t dm_color = -1;
  std::int16_t dm_duplex = -1;
  std::int16_t dm_paper_size = -1;
  std::int16_t dm_orientation = -1;
  std::int32_t dm_nup = -1;
  std::int16_t dm_collate = -1;
  std::int16_t dm_print_quality = -1;
  std::uint32_t dm_fields = 0;

  /// Original document name extracted from the .SHD shadow file.
  /// May differ from JOB_INFO_2W.pDocument when the XPS filter pipeline
  /// renames the job to a localized "Local Downlevel Document" label.
  std::string shd_document_name;
};

/// Locate and parse the spool file for a given job.
///
/// Scans the Windows spool directory for active .SPL/.SHD pairs, matches
/// by job ID (via the .SHD shadow file), and parses any EMF-format spool
/// data found.
///
/// @param job_id        The spooler job ID to look for.
/// @param printer_name  UTF-8 printer name (used for SHD matching).
/// @return              Parsed spool file info, or std::nullopt if not found
///                      or access is denied.
std::optional<SpoolFileInfo> find_and_parse_spool_file(
    std::uint32_t job_id,
    const std::string& printer_name);

/// Parse an EMF spool file at the given path.
///
/// Reads the file, walks EMF spool records, counts pages, and extracts
/// the embedded DEVMODE.  Safe for partially-written files (job still
/// spooling): returns whatever records have been written so far.
///
/// @param spl_path  Path to the .SPL file.
/// @return          Parsed info (emf_page_count may be partial).
SpoolFileInfo parse_emf_spool_file(const std::filesystem::path& spl_path);

/// Build a partial MetadataSnapshot from spool file analysis.
///
/// Only populates fields that were successfully extracted from the spool
/// file's DEVMODE.  Fields not available retain FieldState::NotReported.
core::MetadataSnapshot metadata_from_spool(const SpoolFileInfo& info);

}  // namespace wpce::parser
