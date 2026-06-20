#include "storage/file_utils.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace wpce::storage
{

std::filesystem::path ensure_output_directory(const std::filesystem::path& directory)
{
  std::filesystem::create_directories(directory);
  return directory;
}

std::filesystem::path make_temp_path(const std::filesystem::path& final_path)
{
  std::ostringstream suffix;
  suffix << ".tmp." << std::hash<std::thread::id>{}(std::this_thread::get_id());
  return final_path.string() + suffix.str();
}

void replace_file(const std::filesystem::path& temp_path, const std::filesystem::path& final_path)
{
#ifdef _WIN32
  if (!MoveFileExW(
        temp_path.wstring().c_str(),
        final_path.wstring().c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
  {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "MoveFileExW");
  }
#else
  std::filesystem::rename(temp_path, final_path);
#endif
}

void write_text_file_atomic(const std::string& content, const std::filesystem::path& path)
{
  ensure_output_directory(path.parent_path());
  const std::filesystem::path temp_path = make_temp_path(path);

  try
  {
    {
      std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
      if (!output)
      {
        throw std::runtime_error("failed to open temporary file: " + path.string());
      }
      output.write(content.data(), static_cast<std::streamsize>(content.size()));
      if (!output)
      {
        throw std::runtime_error("failed to write temporary file: " + path.string());
      }
    }
    replace_file(temp_path, path);
  }
  catch (...)
  {
    std::error_code ignored;
    std::filesystem::remove(temp_path, ignored);
    throw;
  }
}

}  // namespace wpce::storage
