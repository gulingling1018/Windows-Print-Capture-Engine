#include "common/file_type_detector.h"
#include "common/pricing_engine.h"

#include <algorithm>
#include <cctype>

namespace wpce::common
{

namespace
{

bool is_extension_char(char ch) noexcept
{
  return std::isalnum(static_cast<unsigned char>(ch)) != 0;
}

bool has_exact_suffix(std::string_view text, const std::string& extension)
{
  if (extension.empty()) return false;
  const auto slash = text.find_last_of("/\\");
  const auto dot = text.rfind('.');
  if (dot == std::string_view::npos ||
      (slash != std::string_view::npos && dot < slash) ||
      dot + 1 >= text.size())
  {
    return false;
  }

  std::string suffix;
  for (std::size_t i = dot + 1; i < text.size() && is_extension_char(text[i]); ++i)
    suffix += static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
  return suffix == extension;
}

bool contains_case_insensitive(std::string_view text, std::string_view needle)
{
  return std::search(text.begin(), text.end(), needle.begin(), needle.end(),
                     [](char left, char right) {
                       return std::tolower(static_cast<unsigned char>(left)) ==
                              std::tolower(static_cast<unsigned char>(right));
                     }) != text.end();
}

bool is_application_hint(std::string_view text)
{
  constexpr std::string_view names[] = {
    "autocad", "bricscad", "draftsight", "solidworks", "inventor",
    "revit", "catia", "creo", "sketchup", "rhino", "coreldraw",
    "illustrator", "photoshop", "indesign"
  };
  for (const auto name : names)
  {
    if (contains_case_insensitive(text, name)) return true;
  }
  return false;
}

}  // namespace

FileTypeDetectionResult detect_file_type(std::string_view text,
                                         std::string_view source)
{
  FileTypeDetectionResult result;
  result.extension = extract_file_extension(text);
  if (result.extension.empty()) return result;
  result.source = std::string(source);
  if (has_exact_suffix(text, result.extension)) result.confidence = 3;
  else if (is_application_hint(text)) result.confidence = 1;
  else result.confidence = 2;
  return result;
}

FileTypeDetectionResult resolve_file_type(
    const std::vector<FileTypeCandidate>& candidates)
{
  FileTypeDetectionResult best;
  int bestPriority = -1;
  for (const auto& candidate : candidates)
  {
    auto result = detect_file_type(candidate.text, candidate.source);
    if (result.extension.empty()) continue;
    if (result.confidence > best.confidence ||
        (result.confidence == best.confidence && candidate.source_priority > bestPriority))
    {
      best = std::move(result);
      bestPriority = candidate.source_priority;
    }
  }
  return best;
}

}  // namespace wpce::common
