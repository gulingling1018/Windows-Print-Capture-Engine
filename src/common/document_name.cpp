#include "common/document_name.h"

#include <algorithm>
#include <string>

namespace wpce::common
{

namespace
{

std::string trim_ascii(std::string_view value)
{
  auto is_space = [](unsigned char ch) {
    return ch == '\0' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
  };

  while (!value.empty() && is_space(static_cast<unsigned char>(value.front())))
    value.remove_prefix(1);
  while (!value.empty() && is_space(static_cast<unsigned char>(value.back())))
    value.remove_suffix(1);

  return std::string(value);
}

std::string ascii_lower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z')
      return static_cast<char>(ch - 'A' + 'a');
    return static_cast<char>(ch);
  });
  return value;
}

}  // namespace

bool is_spooler_placeholder_document(std::string_view documentName)
{
  const std::string trimmed = trim_ascii(documentName);
  if (trimmed.empty()) return false;

  constexpr std::string_view kLocalDownlevelDocumentZh =
      "\xE6\x9C\xAC\xE5\x9C\xB0\xE4\xB8\x8B\xE5\xB1\x82\xE6\x96\x87\xE6\xA1\xA3";
  if (trimmed == kLocalDownlevelDocumentZh)
    return true;

  return ascii_lower(trimmed) == "local downlevel document";
}

}  // namespace wpce::common
