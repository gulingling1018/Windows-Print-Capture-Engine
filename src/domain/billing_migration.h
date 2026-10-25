#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace wpce::domain
{
struct BillingMigrationResult
{
  std::size_t migrated_dates{0};
  std::vector<std::string> errors;
  [[nodiscard]] bool ok() const { return errors.empty(); }
};
/// Read-only legacy inputs. Date is taken from SETTLE-YYYYMMDD-* (the
/// original business date), never from the UTC calendar day.
BillingMigrationResult migrate_legacy_billing(const std::filesystem::path& root,
    const std::string& today);
}
