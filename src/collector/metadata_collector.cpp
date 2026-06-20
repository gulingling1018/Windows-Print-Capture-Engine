#include "collector/metadata_collector.h"

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

// ── Paper size lookup table (DMPAPER_* → name) ─────────────────────────

struct PaperSizeEntry
{
  std::int16_t id;
  std::string_view name;
};

constexpr PaperSizeEntry kPaperSizes[] = {
  {DMPAPER_LETTER, "Letter"},
  {DMPAPER_LETTERSMALL, "Letter Small"},
  {DMPAPER_TABLOID, "Tabloid"},
  {DMPAPER_LEDGER, "Ledger"},
  {DMPAPER_LEGAL, "Legal"},
  {DMPAPER_STATEMENT, "Statement"},
  {DMPAPER_EXECUTIVE, "Executive"},
  {DMPAPER_A3, "A3"},
  {DMPAPER_A4, "A4"},
  {DMPAPER_A4SMALL, "A4 Small"},
  {DMPAPER_A5, "A5"},
  {DMPAPER_B4, "B4 (JIS)"},
  {DMPAPER_B5, "B5 (JIS)"},
  {DMPAPER_FOLIO, "Folio"},
  {DMPAPER_QUARTO, "Quarto"},
  {DMPAPER_10X14, "10x14"},
  {DMPAPER_11X17, "11x17"},
  {DMPAPER_NOTE, "Note"},
  {DMPAPER_ENV_9, "Envelope #9"},
  {DMPAPER_ENV_10, "Envelope #10"},
  {DMPAPER_ENV_11, "Envelope #11"},
  {DMPAPER_ENV_12, "Envelope #12"},
  {DMPAPER_ENV_14, "Envelope #14"},
  {DMPAPER_CSHEET, "C Sheet"},
  {DMPAPER_DSHEET, "D Sheet"},
  {DMPAPER_ESHEET, "E Sheet"},
  {DMPAPER_ENV_DL, "Envelope DL"},
  {DMPAPER_ENV_C5, "Envelope C5"},
  {DMPAPER_ENV_C3, "Envelope C3"},
  {DMPAPER_ENV_C4, "Envelope C4"},
  {DMPAPER_ENV_C6, "Envelope C6"},
  {DMPAPER_ENV_C65, "Envelope C65"},
  {DMPAPER_ENV_B4, "Envelope B4"},
  {DMPAPER_ENV_B5, "Envelope B5"},
  {DMPAPER_ENV_B6, "Envelope B6"},
  {DMPAPER_ENV_ITALY, "Envelope Italy"},
  {DMPAPER_ENV_MONARCH, "Envelope Monarch"},
  {DMPAPER_ENV_PERSONAL, "Envelope Personal"},
  {DMPAPER_FANFOLD_US, "US Fanfold"},
  {DMPAPER_FANFOLD_STD_GERMAN, "German Std Fanfold"},
  {DMPAPER_FANFOLD_LGL_GERMAN, "German Legal Fanfold"},
  {DMPAPER_ISO_B4, "B4 (ISO)"},
  {DMPAPER_JAPANESE_POSTCARD, "Japanese Postcard"},
  {DMPAPER_9X11, "9x11"},
  {DMPAPER_10X11, "10x11"},
  {DMPAPER_15X11, "15x11"},
  {DMPAPER_ENV_INVITE, "Envelope Invite"},
  {DMPAPER_LETTER_EXTRA, "Letter Extra"},
  {DMPAPER_LEGAL_EXTRA, "Legal Extra"},
  {DMPAPER_TABLOID_EXTRA, "Tabloid Extra"},
  {DMPAPER_A4_EXTRA, "A4 Extra"},
  {DMPAPER_LETTER_TRANSVERSE, "Letter Transverse"},
  {DMPAPER_A4_TRANSVERSE, "A4 Transverse"},
  {DMPAPER_LETTER_EXTRA_TRANSVERSE, "Letter Extra Transverse"},
  {DMPAPER_A_PLUS, "Super A"},
  {DMPAPER_B_PLUS, "Super B"},
  {DMPAPER_LETTER_PLUS, "Letter Plus"},
  {DMPAPER_A4_PLUS, "A4 Plus"},
  {DMPAPER_A5_TRANSVERSE, "A5 Transverse"},
  {DMPAPER_B5_TRANSVERSE, "B5 (JIS) Transverse"},
  {DMPAPER_A3_EXTRA, "A3 Extra"},
  {DMPAPER_A5_EXTRA, "A5 Extra"},
  {DMPAPER_B5_EXTRA, "B5 (ISO) Extra"},
  {DMPAPER_A2, "A2"},
  {DMPAPER_A3_TRANSVERSE, "A3 Transverse"},
  {DMPAPER_A3_EXTRA_TRANSVERSE, "A3 Extra Transverse"},
  {DMPAPER_DBL_JAPANESE_POSTCARD, "Japanese Double Postcard"},
  {DMPAPER_A6, "A6"},
  {DMPAPER_JENV_KAKU2, "Japanese Envelope Kaku #2"},
  {DMPAPER_JENV_KAKU3, "Japanese Envelope Kaku #3"},
  {DMPAPER_JENV_CHOU3, "Japanese Envelope Chou #3"},
  {DMPAPER_JENV_CHOU4, "Japanese Envelope Chou #4"},
  {DMPAPER_LETTER_ROTATED, "Letter Rotated"},
  {DMPAPER_A3_ROTATED, "A3 Rotated"},
  {DMPAPER_A4_ROTATED, "A4 Rotated"},
  {DMPAPER_A5_ROTATED, "A5 Rotated"},
  {DMPAPER_B4_JIS_ROTATED, "B4 (JIS) Rotated"},
  {DMPAPER_B5_JIS_ROTATED, "B5 (JIS) Rotated"},
  {DMPAPER_JAPANESE_POSTCARD_ROTATED, "Japanese Postcard Rotated"},
  {DMPAPER_DBL_JAPANESE_POSTCARD_ROTATED, "Double Japanese Postcard Rotated"},
  {DMPAPER_A6_ROTATED, "A6 Rotated"},
  {DMPAPER_JENV_KAKU2_ROTATED, "Japanese Envelope Kaku #2 Rotated"},
  {DMPAPER_JENV_KAKU3_ROTATED, "Japanese Envelope Kaku #3 Rotated"},
  {DMPAPER_JENV_CHOU3_ROTATED, "Japanese Envelope Chou #3 Rotated"},
  {DMPAPER_JENV_CHOU4_ROTATED, "Japanese Envelope Chou #4 Rotated"},
  {DMPAPER_B6_JIS, "B6 (JIS)"},
  {DMPAPER_B6_JIS_ROTATED, "B6 (JIS) Rotated"},
  {DMPAPER_12X11, "12x11"},
  {DMPAPER_JENV_YOU4, "Japanese Envelope You #4"},
  {DMPAPER_JENV_YOU4_ROTATED, "Japanese Envelope You #4 Rotated"},
  {DMPAPER_P16K, "PRC 16K"},
  {DMPAPER_P32K, "PRC 32K"},
  {DMPAPER_P32KBIG, "PRC 32K Big"},
  {DMPAPER_PENV_1, "PRC Envelope #1"},
  {DMPAPER_PENV_2, "PRC Envelope #2"},
  {DMPAPER_PENV_3, "PRC Envelope #3"},
  {DMPAPER_PENV_4, "PRC Envelope #4"},
  {DMPAPER_PENV_5, "PRC Envelope #5"},
  {DMPAPER_PENV_6, "PRC Envelope #6"},
  {DMPAPER_PENV_7, "PRC Envelope #7"},
  {DMPAPER_PENV_8, "PRC Envelope #8"},
  {DMPAPER_PENV_9, "PRC Envelope #9"},
  {DMPAPER_PENV_10, "PRC Envelope #10"},
  {DMPAPER_P16K_ROTATED, "PRC 16K Rotated"},
  {DMPAPER_P32K_ROTATED, "PRC 32K Rotated"},
  {DMPAPER_P32KBIG_ROTATED, "PRC 32K Big Rotated"},
  {DMPAPER_PENV_1_ROTATED, "PRC Envelope #1 Rotated"},
  {DMPAPER_PENV_2_ROTATED, "PRC Envelope #2 Rotated"},
  {DMPAPER_PENV_3_ROTATED, "PRC Envelope #3 Rotated"},
  {DMPAPER_PENV_4_ROTATED, "PRC Envelope #4 Rotated"},
  {DMPAPER_PENV_5_ROTATED, "PRC Envelope #5 Rotated"},
  {DMPAPER_PENV_6_ROTATED, "PRC Envelope #6 Rotated"},
  {DMPAPER_PENV_7_ROTATED, "PRC Envelope #7 Rotated"},
  {DMPAPER_PENV_8_ROTATED, "PRC Envelope #8 Rotated"},
  {DMPAPER_PENV_9_ROTATED, "PRC Envelope #9 Rotated"},
  {DMPAPER_PENV_10_ROTATED, "PRC Envelope #10 Rotated"},
};

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

}  // namespace

// ── Public name mappers ──────────────────────────────────────────────────

std::string_view paper_size_name(const std::int16_t dm_paper_size) noexcept
{
  for (const PaperSizeEntry& entry : kPaperSizes)
  {
    if (entry.id == dm_paper_size)
    {
      return entry.name;
    }
  }
  return {};
}

std::string_view duplex_name(const std::int16_t dm_duplex) noexcept
{
  switch (dm_duplex)
  {
    case DMDUP_SIMPLEX:
      return "simplex";
    case DMDUP_VERTICAL:
      return "vertical";
    case DMDUP_HORIZONTAL:
      return "horizontal";
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
    default:
    {
      // dmPrintQuality can also be a positive DPI value.
      if (dm_print_quality > 0)
      {
        // Return empty to signal Unknown with raw value.
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

  return snapshot;
}
#endif

}  // namespace wpce::collector
