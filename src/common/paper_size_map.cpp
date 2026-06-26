/// @file paper_size_map.cpp
/// Shared DMPAPER_* → human-readable name mapping table implementation.

#include "common/paper_size_map.h"

#ifdef _WIN32
#include <windows.h>
#include <winspool.h>
#else
// Provide stub values for non-Windows builds (e.g. test harness).
#define DMPAPER_LETTER   1
#define DMPAPER_LETTERSMALL 2
#define DMPAPER_TABLOID  3
#define DMPAPER_LEDGER   4
#define DMPAPER_LEGAL    5
#define DMPAPER_STATEMENT 6
#define DMPAPER_EXECUTIVE 7
#define DMPAPER_A3       8
#define DMPAPER_A4       9
#define DMPAPER_A4SMALL  10
#define DMPAPER_A5       11
#define DMPAPER_B4       12
#define DMPAPER_B5       13
#define DMPAPER_FOLIO    14
#define DMPAPER_QUARTO   15
#define DMPAPER_10X14    16
#define DMPAPER_11X17    17
#define DMPAPER_NOTE     18
#define DMPAPER_ENV_9    19
#define DMPAPER_ENV_10   20
#define DMPAPER_ENV_11   21
#define DMPAPER_ENV_12   22
#define DMPAPER_ENV_14   23
#define DMPAPER_CSHEET   24
#define DMPAPER_DSHEET   25
#define DMPAPER_ESHEET   26
#define DMPAPER_ENV_DL   27
#define DMPAPER_ENV_C5   28
#define DMPAPER_ENV_C3   29
#define DMPAPER_ENV_C4   30
#define DMPAPER_ENV_C6   31
#define DMPAPER_ENV_C65  32
#define DMPAPER_ENV_B4   33
#define DMPAPER_ENV_B5   34
#define DMPAPER_ENV_B6   35
#define DMPAPER_ENV_ITALY 36
#define DMPAPER_ENV_MONARCH 37
#define DMPAPER_ENV_PERSONAL 38
#define DMPAPER_FANFOLD_US 39
#define DMPAPER_FANFOLD_STD_GERMAN 40
#define DMPAPER_FANFOLD_LGL_GERMAN 41
#define DMPAPER_ISO_B4   42
#define DMPAPER_JAPANESE_POSTCARD 43
#define DMPAPER_9X11     44
#define DMPAPER_10X11    45
#define DMPAPER_15X11    46
#define DMPAPER_ENV_INVITE 47
#define DMPAPER_LETTER_EXTRA 50
#define DMPAPER_LEGAL_EXTRA 51
#define DMPAPER_TABLOID_EXTRA 52
#define DMPAPER_A4_EXTRA 53
#define DMPAPER_LETTER_TRANSVERSE 54
#define DMPAPER_A4_TRANSVERSE 55
#define DMPAPER_LETTER_EXTRA_TRANSVERSE 56
#define DMPAPER_A_PLUS   57
#define DMPAPER_B_PLUS   58
#define DMPAPER_LETTER_PLUS 59
#define DMPAPER_A4_PLUS  60
#define DMPAPER_A5_TRANSVERSE 61
#define DMPAPER_B5_TRANSVERSE 62
#define DMPAPER_A3_EXTRA 63
#define DMPAPER_A5_EXTRA 64
#define DMPAPER_B5_EXTRA 65
#define DMPAPER_A2       66
#define DMPAPER_A3_TRANSVERSE 67
#define DMPAPER_A3_EXTRA_TRANSVERSE 68
#define DMPAPER_DBL_JAPANESE_POSTCARD 69
#define DMPAPER_A6       70
#define DMPAPER_JENV_KAKU2 71
#define DMPAPER_JENV_KAKU3 72
#define DMPAPER_JENV_CHOU3 73
#define DMPAPER_JENV_CHOU4 74
#define DMPAPER_LETTER_ROTATED 75
#define DMPAPER_A3_ROTATED 76
#define DMPAPER_A4_ROTATED 77
#define DMPAPER_A5_ROTATED 78
#define DMPAPER_B4_JIS_ROTATED 79
#define DMPAPER_B5_JIS_ROTATED 80
#define DMPAPER_JAPANESE_POSTCARD_ROTATED 81
#define DMPAPER_DBL_JAPANESE_POSTCARD_ROTATED 82
#define DMPAPER_A6_ROTATED 83
#define DMPAPER_JENV_KAKU2_ROTATED 84
#define DMPAPER_JENV_KAKU3_ROTATED 85
#define DMPAPER_JENV_CHOU3_ROTATED 86
#define DMPAPER_JENV_CHOU4_ROTATED 87
#define DMPAPER_B6_JIS   88
#define DMPAPER_B6_JIS_ROTATED 89
#define DMPAPER_12X11    90
#define DMPAPER_JENV_YOU4 91
#define DMPAPER_JENV_YOU4_ROTATED 92
#define DMPAPER_P16K     93
#define DMPAPER_P32K     94
#define DMPAPER_P32KBIG  95
#define DMPAPER_PENV_1   96
#define DMPAPER_PENV_2   97
#define DMPAPER_PENV_3   98
#define DMPAPER_PENV_4   99
#define DMPAPER_PENV_5   100
#define DMPAPER_PENV_6   101
#define DMPAPER_PENV_7   102
#define DMPAPER_PENV_8   103
#define DMPAPER_PENV_9   104
#define DMPAPER_PENV_10  105
#define DMPAPER_P16K_ROTATED 106
#define DMPAPER_P32K_ROTATED 107
#define DMPAPER_P32KBIG_ROTATED 108
#define DMPAPER_PENV_1_ROTATED 109
#define DMPAPER_PENV_2_ROTATED 110
#define DMPAPER_PENV_3_ROTATED 111
#define DMPAPER_PENV_4_ROTATED 112
#define DMPAPER_PENV_5_ROTATED 113
#define DMPAPER_PENV_6_ROTATED 114
#define DMPAPER_PENV_7_ROTATED 115
#define DMPAPER_PENV_8_ROTATED 116
#define DMPAPER_PENV_9_ROTATED 117
#define DMPAPER_PENV_10_ROTATED 118
#endif

namespace wpce::common
{

const PaperSizeEntry kPaperSizes[] = {
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

const std::size_t kPaperSizesCount = sizeof(kPaperSizes) / sizeof(kPaperSizes[0]);

std::string_view paper_size_name(const std::int16_t dm_paper_size) noexcept
{
  for (std::size_t i = 0; i < kPaperSizesCount; ++i)
  {
    if (kPaperSizes[i].id == dm_paper_size)
    {
      return kPaperSizes[i].name;
    }
  }
  return {};
}

}  // namespace wpce::common
