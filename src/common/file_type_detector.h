#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace wpce::common
{

struct FileTypeCandidate
{
  std::string text;
  std::string source;
  int source_priority{0};
};

struct FileTypeDetectionResult
{
  std::string extension;
  std::string source;
  int confidence{0}; // 0 unknown, 1 application hint, 2 marker, 3 exact suffix
};

[[nodiscard]] FileTypeDetectionResult detect_file_type(
    std::string_view text,
    std::string_view source = "document_name");

[[nodiscard]] FileTypeDetectionResult resolve_file_type(
    const std::vector<FileTypeCandidate>& candidates);

}  // namespace wpce::common
