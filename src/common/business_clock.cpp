#include "common/business_clock.h"

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace wpce::common
{

namespace
{

bool valid_calendar_date(const std::tm& value) noexcept
{
  const int year = value.tm_year + 1900;
  const int month = value.tm_mon + 1;
  return year >= 2000 && year <= 9999 &&
         month >= 1 && month <= 12 &&
         value.tm_mday >= 1 && value.tm_mday <= 31;
}

bool local_time(std::time_t timestamp, std::tm& out) noexcept
{
#ifdef _WIN32
  return localtime_s(&out, &timestamp) == 0;
#else
  return localtime_r(&timestamp, &out) != nullptr;
#endif
}

bool beijing_time(std::time_t timestamp, std::tm& out) noexcept
{
  constexpr std::time_t kChinaOffsetSeconds = 8 * 60 * 60;
  const std::time_t chinaTimestamp = timestamp + kChinaOffsetSeconds;
#ifdef _WIN32
  return gmtime_s(&out, &chinaTimestamp) == 0;
#else
  return gmtime_r(&chinaTimestamp, &out) != nullptr;
#endif
}

BusinessDate format_date(const std::tm& value, bool fallback) noexcept
{
  BusinessDate result;
  char dashed[16]{};
  char compact[16]{};
  std::snprintf(dashed, sizeof(dashed), "%04d-%02d-%02d",
                value.tm_year + 1900, value.tm_mon + 1, value.tm_mday);
  std::snprintf(compact, sizeof(compact), "%04d%02d%02d",
                value.tm_year + 1900, value.tm_mon + 1, value.tm_mday);
  result.yyyy_mm_dd = dashed;
  result.yyyymmdd = compact;
  result.used_beijing_fallback = fallback;
  return result;
}

}  // namespace

BusinessDate business_date_from_time(std::time_t timestamp,
                                     bool force_beijing_fallback) noexcept
{
  std::tm resolved{};
  if (!force_beijing_fallback && local_time(timestamp, resolved) &&
      valid_calendar_date(resolved))
  {
    return format_date(resolved, false);
  }

  resolved = {};
  if (beijing_time(timestamp, resolved) && valid_calendar_date(resolved))
  {
    return format_date(resolved, true);
  }

  // A deterministic last resort keeps paths and IDs valid even if the system
  // clock API itself is unavailable.
  resolved = {};
  resolved.tm_year = 70;
  resolved.tm_mon = 0;
  resolved.tm_mday = 1;
  return format_date(resolved, true);
}

BusinessDate current_business_date() noexcept
{
  const auto now = std::chrono::system_clock::now();
  return business_date_from_time(std::chrono::system_clock::to_time_t(now));
}

std::optional<std::chrono::sys_days> parse_business_date(const std::string& date) noexcept
{
  if (date.size() != 10 || date[4] != '-' || date[7] != '-') return std::nullopt;
  for (std::size_t i = 0; i < date.size(); ++i)
    if (i != 4 && i != 7 && (date[i] < '0' || date[i] > '9')) return std::nullopt;
  const int year = (date[0]-'0')*1000+(date[1]-'0')*100+(date[2]-'0')*10+date[3]-'0';
  const unsigned month = (date[5]-'0')*10+date[6]-'0';
  const unsigned day = (date[8]-'0')*10+date[9]-'0';
  const std::chrono::year_month_day value{std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{day}};
  if (year < 1970 || !value.ok()) return std::nullopt;
  return std::chrono::sys_days{value};
}

BusinessDayCoordinator::BusinessDayCoordinator(DateProvider provider) : provider_(std::move(provider)) {}
void BusinessDayCoordinator::setDateProvider(DateProvider provider) { provider_ = std::move(provider); }
std::string BusinessDayCoordinator::today() const
{
  auto date = provider_ ? provider_() : current_business_date().yyyy_mm_dd;
  if (!parse_business_date(date)) throw std::runtime_error("invalid business date");
  return date;
}
void BusinessDayCoordinator::bind(std::string date)
{
  if (!parse_business_date(date)) throw std::runtime_error("invalid bucket business date");
  bound_date_ = std::move(date);
}

}  // namespace wpce::common
