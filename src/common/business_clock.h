#pragma once

#include <ctime>
#include <chrono>
#include <functional>
#include <optional>
#include <string>

namespace wpce::common
{

struct BusinessDate
{
  std::string yyyy_mm_dd;
  std::string yyyymmdd;
  bool used_beijing_fallback{false};
};

/// Resolve a business date from the supplied Unix timestamp. The PC's
/// configured local timezone is authoritative. If local conversion fails or
/// produces an invalid calendar date, UTC+8 (China Standard Time) is used.
/// force_beijing_fallback exists for deterministic tests and diagnostics.
[[nodiscard]] BusinessDate business_date_from_time(
    std::time_t timestamp,
    bool force_beijing_fallback = false) noexcept;

/// Current business date using the same policy as business_date_from_time().
[[nodiscard]] BusinessDate current_business_date() noexcept;

[[nodiscard]] std::optional<std::chrono::sys_days> parse_business_date(const std::string& date) noexcept;

/// Holds the date bound to a bucket, independently of wall-clock changes.
class BusinessDayCoordinator
{
 public:
  using DateProvider = std::function<std::string()>;
  explicit BusinessDayCoordinator(DateProvider provider = {});
  void setDateProvider(DateProvider provider);
  [[nodiscard]] std::string today() const;
  void bind(std::string date);
  [[nodiscard]] const std::string& boundDate() const noexcept { return bound_date_; }
 private:
  DateProvider provider_;
  std::string bound_date_;
};

}  // namespace wpce::common
