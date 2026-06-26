#include "collector/metadata_collector.h"
#include "common/paper_size_map.h"
#include "common/pricing_engine.h"

#ifdef _WIN32
#include <windows.h>
#include <winspool.h>
#endif

#include <array>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace wpce::collector
{
namespace
{

// ── Field helpers ──────────────────────────────────────────────────────

core::FieldValue make_reported(std::string value)
{
  return core::FieldValue{std::move(value), core::FieldState::Reported};
}

core::FieldValue make_not_reported()
{
  return core::FieldValue{{}, core::FieldState::NotReported};
}

core::FieldValue make_unknown(std::int16_t raw_value)
{
  return core::FieldValue{std::to_string(raw_value), core::FieldState::Unknown};
}

std::string to_utf8(const wchar_t* value)
{
  if (value == nullptr || value[0] == L'\0')
  {
    return {};
  }

  const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1)
  {
    return {};
  }

  std::string result(static_cast<std::size_t>(size) - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
  return result;
}

bool has_dm_field(const DWORD dm_fields, const DWORD field_bit) noexcept
{
  return (dm_fields & field_bit) != 0;
}

// ── DEVMODE helpers ────────────────────────────────────────────────────

const DEVMODEW* devmode_from_job(const JOB_INFO_2W& job) noexcept
{
  return static_cast<const DEVMODEW*>(job.pDevMode);
}

core::FieldValue extract_total_pages(const JOB_INFO_2W& job)
{
  return make_reported(std::to_string(job.TotalPages));
}

core::FieldValue extract_pages_printed(const JOB_INFO_2W& job)
{
  return make_reported(std::to_string(job.PagesPrinted));
}

core::FieldValue extract_copies(const DEVMODEW& dm)
{
  if (!has_dm_field(dm.dmFields, DM_COPIES))
  {
    return make_not_reported();
  }
  return make_reported(std::to_string(dm.dmCopies));
}

core::FieldValue extract_duplex(const DEVMODEW& dm)
{
  if (!has_dm_field(dm.dmFields, DM_DUPLEX))
  {
    return make_not_reported();
  }
  const std::string_view name = duplex_name(dm.dmDuplex);
  if (name.empty())
  {
    return make_unknown(dm.dmDuplex);
  }
  return make_reported(std::string(name));
}

core::FieldValue extract_color(const DEVMODEW& dm)
{
  if (!has_dm_field(dm.dmFields, DM_COLOR))
  {
    return make_not_reported();
  }
  const std::string_view name = color_name(dm.dmColor);
  if (name.empty())
  {
    return make_unknown(dm.dmColor);
  }
  return make_reported(std::string(name));
}

core::FieldValue extract_paper_size(const DEVMODEW& dm)
{
  if (!has_dm_field(dm.dmFields, DM_PAPERSIZE))
  {
    return make_not_reported();
  }

  // dmPaperSize == 0 means the driver did not pick a standard form; check
  // custom dimensions if available.
  if (dm.dmPaperSize == 0)
  {
    if (has_dm_field(dm.dmFields, DM_PAPERLENGTH) && has_dm_field(dm.dmFields, DM_PAPERWIDTH) &&
        (dm.dmPaperLength > 0 || dm.dmPaperWidth > 0))
    {
      // dmPaperLength and dmPaperWidth are in tenths of a millimetre.
      const double width_mm = static_cast<double>(dm.dmPaperWidth) / 10.0;
      const double length_mm = static_cast<double>(dm.dmPaperLength) / 10.0;
      std::string custom;
      custom += std::to_string(static_cast<int>(width_mm));
      custom += "x";
      custom += std::to_string(static_cast<int>(length_mm));
      custom += "mm";
      return make_reported(std::move(custom));
    }
    return make_reported("Custom");
  }

  const std::string_view name = paper_size_name(dm.dmPaperSize);
  if (name.empty())
  {
    return make_unknown(dm.dmPaperSize);
  }
  return make_reported(std::string(name));
}

core::FieldValue extract_orientation(const DEVMODEW& dm)
{
  if (!has_dm_field(dm.dmFields, DM_ORIENTATION))
  {
    return make_not_reported();
  }
  const std::string_view name = orientation_name(dm.dmOrientation);
  if (name.empty())
  {
    return make_unknown(dm.dmOrientation);
  }
  return make_reported(std::string(name));
}

core::FieldValue extract_n_up(const DEVMODEW& dm)
{
  if (!has_dm_field(dm.dmFields, DM_NUP))
  {
    return make_not_reported();
  }
  const std::string_view name = n_up_name(dm.dmNup);
  if (name.empty())
  {
    return make_unknown(static_cast<std::int16_t>(dm.dmNup));
  }
  return make_reported(std::string(name));
}

core::FieldValue extract_collate(const DEVMODEW& dm)
{
  if (!has_dm_field(dm.dmFields, DM_COLLATE))
  {
    return make_not_reported();
  }
  const std::string_view name = collate_name(dm.dmCollate);
  if (name.empty())
  {
    return make_unknown(dm.dmCollate);
  }
  return make_reported(std::string(name));
}

core::FieldValue extract_print_quality(const DEVMODEW& dm)
{
  if (!has_dm_field(dm.dmFields, DM_PRINTQUALITY))
  {
    return make_not_reported();
  }
  const std::string_view name = print_quality_name(dm.dmPrintQuality);
  if (name.empty())
  {
    return make_unknown(dm.dmPrintQuality);
  }
  return make_reported(std::string(name));
}

// ── Media type (DM_MEDIATYPE) — HP PCL6 / Canon UFR II common field ──

std::string_view media_type_name(const DWORD dm_media_type) noexcept
{
  // DMMEDIA_* constants from wingdi.h.  Only the most common ones
  // observed across HP, Canon, Brother, and Epson drivers.
  switch (dm_media_type)
  {
    case DMMEDIA_STANDARD:      return "plain";
    case DMMEDIA_TRANSPARENCY:  return "transparency";
    case DMMEDIA_GLOSSY:        return "glossy";
    default:
    {
      // Some drivers use custom media type IDs outside the standard enum.
      if (dm_media_type > static_cast<DWORD>(DMMEDIA_USER))
        return {};  // custom — report as Unknown with raw value
      return {};
    }
  }
}

core::FieldValue extract_media_type(const DEVMODEW& dm)
{
  if (!has_dm_field(dm.dmFields, DM_MEDIATYPE))
  {
    return make_not_reported();
  }
  const std::string_view name = media_type_name(dm.dmMediaType);
  if (name.empty())
  {
    // If the value is above DMMEDIA_USER, treat as vendor-specific.
    return make_unknown(static_cast<std::int16_t>(dm.dmMediaType));
  }
  return make_reported(std::string(name));
}

}  // namespace

// ── Public name mappers ──────────────────────────────────────────────────

std::string_view paper_size_name(const std::int16_t dm_paper_size) noexcept
{
  return common::paper_size_name(dm_paper_size);
}

std::string_view duplex_name(const std::int16_t dm_duplex) noexcept
{
  switch (dm_duplex)
  {
    case DMDUP_SIMPLEX:
      return "simplex";
    case DMDUP_VERTICAL:
      return "long-edge (book)";
    case DMDUP_HORIZONTAL:
      return "short-edge (calendar)";
    default:
      return {};
  }
}

std::string_view color_name(const std::int16_t dm_color) noexcept
{
  switch (dm_color)
  {
    case DMCOLOR_MONOCHROME:
      return "monochrome";
    case DMCOLOR_COLOR:
      return "color";
    default:
      return {};
  }
}

std::string_view orientation_name(const std::int16_t dm_orientation) noexcept
{
  switch (dm_orientation)
  {
    case DMORIENT_PORTRAIT:
      return "portrait";
    case DMORIENT_LANDSCAPE:
      return "landscape";
    default:
      return {};
  }
}

std::string_view n_up_name(const std::int32_t dm_nup) noexcept
{
  switch (dm_nup)
  {
    case 1:
      return "1-up (normal)";
    case 2:
      return "2-up";
    case 4:
      return "4-up";
    case 6:
      return "6-up";
    case 9:
      return "9-up";
    case 16:
      return "16-up";
    default:
      return {};
  }
}

std::string_view collate_name(const std::int16_t dm_collate) noexcept
{
  switch (dm_collate)
  {
    case DMCOLLATE_FALSE:
      return "uncollated";
    case DMCOLLATE_TRUE:
      return "collated";
    default:
      return {};
  }
}

std::string_view print_quality_name(const std::int16_t dm_print_quality) noexcept
{
  switch (dm_print_quality)
  {
    case DMRES_HIGH:
      return "high";
    case DMRES_MEDIUM:
      return "medium";
    case DMRES_LOW:
      return "low";
    case DMRES_DRAFT:
      return "draft";
    // Canon / Epson / Brother drivers often report positive DPI values
    // (300, 600, 1200, 2400) instead of the DMRES_* categories.
    // Common DPI values used by printer drivers — map to readable names.
    case 300:
      return "300 dpi";
    case 600:
      return "600 dpi";
    case 1200:
      return "1200 dpi";
    case 2400:
      return "2400 dpi";
    case 4800:
      return "4800 dpi";
    default:
    {
      // Any other positive value = custom DPI.
      if (dm_print_quality > 0)
      {
        return {};
      }
      return {};
    }
  }
}

// ── Main extraction ──────────────────────────────────────────────────────

#ifdef _WIN32
core::MetadataSnapshot extract_metadata(const JOB_INFO_2W& job)
{
  core::MetadataSnapshot snapshot;
  snapshot.schema_version = 1;
  snapshot.job_id = job.JobId;
  snapshot.printer_name = to_utf8(job.pPrinterName);

  // Job-level fields always available from JOB_INFO_2W.
  snapshot.total_pages = extract_total_pages(job);
  snapshot.pages_printed = extract_pages_printed(job);

  const DEVMODEW* dm = devmode_from_job(job);
  if (dm == nullptr)
  {
    // No DEVMODE available from this driver / job.
    snapshot.copies = make_not_reported();
    snapshot.duplex = make_not_reported();
    snapshot.color = make_not_reported();
    snapshot.paper_size = make_not_reported();
    snapshot.orientation = make_not_reported();
    snapshot.n_up = make_not_reported();
    snapshot.collate = make_not_reported();
    snapshot.print_quality = make_not_reported();
    snapshot.media_type = make_not_reported();
    snapshot.file_extension = make_not_reported();
    return snapshot;
  }

  // Store raw dmFields as hex for diagnostics.
  {
    std::ostringstream hex;
    hex << "0x" << std::hex << std::setfill('0') << std::setw(8) << dm->dmFields;
    snapshot.dm_fields_hex = hex.str();
  }

  snapshot.copies = extract_copies(*dm);
  snapshot.duplex = extract_duplex(*dm);
  snapshot.color = extract_color(*dm);
  snapshot.paper_size = extract_paper_size(*dm);
  snapshot.orientation = extract_orientation(*dm);
  snapshot.n_up = extract_n_up(*dm);
  snapshot.collate = extract_collate(*dm);
  snapshot.print_quality = extract_print_quality(*dm);
  snapshot.media_type = extract_media_type(*dm);

  // Extract file extension from the document name for per-filetype pricing.
  {
    std::string doc_name = to_utf8(job.pDocument);
    if (!doc_name.empty())
    {
      std::string ext = common::extract_file_extension(doc_name);
      if (!ext.empty())
        snapshot.file_extension = core::FieldValue{std::move(ext), core::FieldState::Reported};
      else
        snapshot.file_extension = make_not_reported();
    }
    else
    {
      snapshot.file_extension = make_not_reported();
    }
  }

  return snapshot;
}

#endif

}  // namespace wpce::collector
