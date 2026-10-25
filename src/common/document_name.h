#pragma once

#include <string_view>

namespace wpce::common
{

[[nodiscard]] bool is_spooler_placeholder_document(std::string_view documentName);

}  // namespace wpce::common
