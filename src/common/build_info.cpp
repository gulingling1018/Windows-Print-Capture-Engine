#include "common/build_info.h"

#include <wpce/common/build_config.h>

namespace wpce::common
{

std::string_view product_name() noexcept
{
  return WPCE_PRODUCT_NAME;
}

std::string_view product_version() noexcept
{
  return WPCE_PRODUCT_VERSION;
}

std::string_view product_description() noexcept
{
  return WPCE_PRODUCT_DESCRIPTION;
}

}  // namespace wpce::common

