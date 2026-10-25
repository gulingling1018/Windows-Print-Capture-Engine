#include "domain/billing_migration.h"
#include "domain/billing_batch_repository.h"
#include "bridge/settlement_bridge.h"
#include "common/business_clock.h"
#include "storage/file_utils.h"
#include <map>
#include <set>
#include <sstream>

namespace wpce::domain
{
namespace
{
struct Day
{
  std::vector<BillingBatch> tasks;
  std::unordered_map<std::uint32_t, PrintJobRecord> jobs;
  std::vector<core::SettlementOrder> orders;
  bool conflict{false};
};
std::string order_date(const core::SettlementOrder& order)
{
  if (!order.order_id.starts_with("SETTLE-") || order.order_id.size() < 17 || order.order_id[15] != '-') return {};
  const auto date = order.order_id.substr(7, 4) + "-" + order.order_id.substr(11, 2) + "-" + order.order_id.substr(13, 2);
  return common::parse_business_date(date) ? date : std::string{};
}
bool add_order(Day& day, const core::SettlementOrder& order)
{
  const auto task = order.task_id.empty() ? order.batch_id : order.task_id;
  for (const auto& existing : day.orders)
  {
    if (existing.order_id == order.order_id)
      return bridge::SettlementBridge::toJson(existing) == bridge::SettlementBridge::toJson(order);
    const auto previousTask = existing.task_id.empty() ? existing.batch_id : existing.task_id;
    if (!task.empty() && task == previousTask) return false;
  }
  day.orders.push_back(order);
  return true;
}
}

BillingMigrationResult migrate_legacy_billing(const std::filesystem::path& root,
    const std::string& today)
{
  BillingMigrationResult result;
  if (!common::parse_business_date(today)) { result.errors.push_back("invalid migration date"); return result; }
  std::map<std::string, Day> days;
  auto collect = [&](const std::filesystem::path& file, const std::string& directoryDate = {})
  {
    auto order = bridge::SettlementBridge::loadFromFile(file);
    if (!order) { result.errors.push_back("Unreadable legacy order; original preserved."); return; }
    const auto date = order_date(*order);
    if (date.empty() || (!directoryDate.empty() && date != directoryDate))
    {
      result.errors.push_back("Ambiguous legacy order date; original preserved.");
      return;
    }
    if (!add_order(days[date], *order))
    {
      days[date].conflict = true;
      result.errors.push_back("Conflicting legacy order in business date " + date);
    }
  };
  try
  {
    const auto history = root / "history";
    if (std::filesystem::exists(history))
      for (const auto& entry : std::filesystem::directory_iterator(history))
      {
        if (!entry.is_directory() || entry.is_symlink()) continue;
        const auto date = entry.path().filename().string();
        if (!common::parse_business_date(date)) continue;
        for (const auto& file : std::filesystem::directory_iterator(entry.path()))
          if (file.is_regular_file() && !file.is_symlink() && file.path().extension() == ".json") collect(file.path(), date);
      }
    const auto copies = root / "_state" / "settlement_orders";
    if (std::filesystem::exists(copies))
      for (const auto& file : std::filesystem::directory_iterator(copies))
        if (file.is_regular_file() && !file.is_symlink() && file.path().extension() == ".json") collect(file.path());

    const auto active = root / "_state" / "today_active.json";
    if (std::filesystem::exists(active))
    {
      BillingBatchRepository legacy; legacy.setFilePath(active);
      std::vector<BillingBatch> tasks;
      std::unordered_map<std::uint32_t, PrintJobRecord> jobs;
      std::string date;
      if (!legacy.load(tasks, jobs, date))
        result.errors.push_back("Unreadable legacy active state; original preserved.");
      else if (date == today)
      {
        days[today].tasks = std::move(tasks);
        days[today].jobs = std::move(jobs);
      }
    }

    for (auto& [date, legacy] : days)
    {
      if (legacy.conflict) continue;
      BillingBatchRepository target; target.setFilePath(root / date / "orders.json");
      Day combined; std::string saved;
      auto loaded = target.loadDaily(combined.tasks, combined.jobs, combined.orders, saved);
      if (!loaded.ok())
      { result.errors.push_back("Protected target bucket " + date + ": " + loaded.message); continue; }
      bool conflict = false, changed = loaded.status == BucketLoadStatus::Missing;
      for (const auto& order : legacy.orders)
      {
        const auto size = combined.orders.size();
        if (!add_order(combined, order)) { conflict = true; break; }
        changed = changed || size != combined.orders.size();
      }
      if (conflict) { result.errors.push_back("Migration conflicts with target bucket " + date); continue; }
      for (const auto& task : legacy.tasks)
      {
        bool exists = false;
        for (const auto& current : combined.tasks) if (current.batch_id == task.batch_id) exists = true;
        if (exists) continue; // v4 runtime state always wins over legacy state.
        if (task.is_settled())
        {
          bool hasOrder = false;
          for (const auto& order : combined.orders) if (order.order_id == task.settle_order_id) hasOrder = true;
          if (!hasOrder) { conflict = true; break; }
        }
        for (auto jid : task.job_ids)
        {
          const auto job = legacy.jobs.find(jid);
          if (job == legacy.jobs.end() || combined.jobs.contains(jid)) { conflict = true; break; }
          combined.jobs.emplace(jid, job->second);
        }
        if (conflict) break;
        combined.tasks.push_back(task); changed = true;
      }
      if (conflict) { result.errors.push_back("Incomplete or colliding legacy task in " + date); continue; }
      if (changed && !target.save(combined.tasks, combined.jobs, combined.orders, date))
      { result.errors.push_back("Migration write failed for " + date + ": " + target.lastError()); continue; }
      if (changed) ++result.migrated_dates;
      // Marker is advisory: a retry always revalidates sources and target.
      storage::write_text_file_atomic("schema=4\nbusiness_date=" + date + "\n",
                                     root / date / "billing-migration-v4.complete");
    }
  }
  catch (const std::exception& error) { result.errors.push_back(error.what()); }
  return result;
}
}
