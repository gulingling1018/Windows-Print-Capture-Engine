#pragma once

#ifdef _WIN32
#include <windows.h>
#include <filterpipeline.h>
#endif

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wpce::filter
{

/// Read the entire content of an IPrintReadStream into a string.
std::string read_print_stream(IUnknown* pStream);

/// Extract PrintTicket XML from an IPartPrintTicket.
/// Returns the XML string, or empty string on failure.
std::string extract_print_ticket_xml(IPartPrintTicket* pPrintTicket);

/// Read all bytes from a generic IStream (standard COM stream).
std::string read_istream_to_string(IStream* pStream);

}  // namespace wpce::filter
