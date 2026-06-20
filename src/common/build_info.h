#pragma once

#include <string_view>

namespace wpce::common
{

std::string_view product_name() noexcept;
std::string_view product_version() noexcept;
std::string_view product_description() noexcept;

}  // namespace wpce::common

