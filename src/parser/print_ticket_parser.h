/// @file print_ticket_parser.h
/// Parse PrintTicket XML into MetadataSnapshot.
///
/// The PrintTicket is the ONLY authoritative source for per-job print
/// settings on Windows 10/11 (v4 drivers).  JOB_INFO_2W::pDevMode is
/// null for v4 drivers, and DocumentPropertiesW returns printer defaults
/// - not the actual job settings.
///
/// This parser extracts the standard Print Schema Keywords (psk:*) from
/// captured PrintTicket XML and maps them to the MetadataSnapshot fields
/// used throughout the UI and serialization layers.

#pragma once

#include "core/data_contracts.h"

#include <string>
#include <string_view>

namespace wpce::parser
{

/// Parse a PrintTicket XML string and fill a MetadataSnapshot.
///
/// Only populates fields that are present in the PrintTicket.
/// Any field NOT found retains its original state (typically NotReported).
///
/// Handles both job-level and page-level PrintTicket XML.
///   - Job-level: copies, duplex, N-up, collate, page order, input bin
///   - Page-level: media size, orientation, color, media type, resolution
///
/// The snapshot's job_id is NOT set - the caller must assign it.
///
/// @param pt_xml   The raw PrintTicket XML string.
/// @param snapshot Existing MetadataSnapshot to merge into.
void parse_print_ticket_xml(std::string_view pt_xml,
                            core::MetadataSnapshot& snapshot);

inline void parse_print_ticket_xml(const std::string& pt_xml,
                                   core::MetadataSnapshot& snapshot)
{
  parse_print_ticket_xml(std::string_view(pt_xml), snapshot);
}

}  // namespace wpce::parser
