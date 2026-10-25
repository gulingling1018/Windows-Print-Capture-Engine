#include "billing/billing_service.h"
#include "bridge/settlement_bridge.h"
#include "storage/file_utils.h"
#include "storage/json_serializer.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace wpce::ui
{
bool BillingService::replayDeferredEvents()
{
  const auto count = deferred_events_.size();
  for (std::size_t i = 0; i < count; ++i)
  {
    // Copy first: failure must leave the original event available for retry/export.
    auto event = deferred_events_.front();
    processing_date_ = event.date;
    struct Reset { std::string& value; ~Reset() { value.clear(); } } reset{processing_date_};
    if (!synchronizeBusinessDay()) return false;
    deferred_events_.pop_front();
    try
    {
      std::visit([this](auto value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, core::JobSnapshot>) onJobSnapshot(std::move(value));
        else if constexpr (std::is_same_v<T, core::MetadataSnapshot>) onMetadata(std::move(value));
        else onLifecycleEvent(std::move(value));
      }, event.payload);
    }
    catch (...)
    {
      deferred_events_.push_front(std::move(event));
      throw;
    }
    // A counter/storage failure may have deferred it again. Do not spin.
    if (deferred_events_.size() >= count - i)
    {
      auto retry = std::move(deferred_events_.back()); deferred_events_.pop_back();
      deferred_events_.push_front(std::move(retry));
      return false;
    }
  }
  return deferred_events_.empty();
}

BillingService::ShutdownResult BillingService::prepareShutdown()
{
  saveTimer_.stop();
  try
  {
    // Reload a protected bucket only when it cannot discard dirty memory.
    if (!storage_ready_ && !dirty_) load();
    const bool drained = replayDeferredEvents();
    const bool synchronized = drained && synchronizeBusinessDay();
    flushSave();
    if (drained && synchronized && storage_ready_ && !dirty_)
    {
      bool orphan = false;
      for (const auto& [id, record] : jobRegistry_)
      {
        (void)record;
        if (!jobToBatchId_.contains(id)) { orphan = true; break; }
      }
      if (!orphan) return {true, 0, {}};
      return {false, 0, QStringLiteral("仍有未关联的采集记录，请等待作业快照或导出恢复数据。")};
    }
  }
  catch (const std::exception& error) { reportStorageError(error.what()); }
  catch (...) { reportStorageError("Unexpected shutdown persistence failure"); }
  return {false, deferred_events_.size(),
      QStringLiteral("数据尚未安全保存（暂存事件 %1 条）。请检查数据目录后重试，或导出恢复数据。")
          .arg(static_cast<qulonglong>(deferred_events_.size()))};
}

bool BillingService::exportRecovery(const QString& path, QString* error, const QStringList& captureErrors) const
{
  try
  {
    const auto target = std::filesystem::path(path.toStdWString());
    if (target.empty() || std::filesystem::exists(target) || target.filename() == "orders.json" ||
        target.filename() == "counters.json")
      throw std::runtime_error("Recovery destination must be a new, separate file");
    auto json = [](const std::string& text) {
      QJsonParseError parse;
      const auto result = QJsonDocument::fromJson(QByteArray::fromStdString(text), &parse);
      if (parse.error != QJsonParseError::NoError || !result.isObject())
        throw std::runtime_error("Recovery serialization failed");
      return result.object();
    };
    QJsonArray jobs, tasks, orders, events, previousIds, ignoredIds;
    QJsonObject aliases, previousIdentities;
    for (const auto& [alias, canonical] : duplicateJobAliases_)
      aliases[QString::number(alias)] = static_cast<qint64>(canonical);
    for (auto id : previous_day_job_ids_) previousIds.append(static_cast<qint64>(id));
    for (auto id : ignoredPlaceholderJobIds_) ignoredIds.append(static_cast<qint64>(id));
    for (const auto& [id, identity] : previous_day_fingerprints_)
      previousIdentities[QString::number(id)] = QString::fromStdString(identity);
    auto anchor = [](const domain::BatchDocAnchor& value) {
      return QJsonObject{{"job_id", static_cast<qint64>(value.job_id)},
          {"document_name", QString::fromStdString(value.document_name)},
          {"submit_time_utc", QString::fromStdString(value.submit_time_utc)}, {"page_count", value.page_count}};
    };
    for (const auto& [id, record] : jobRegistry_)
      jobs.append(QJsonObject{{"job_id", static_cast<qint64>(id)}, {"batch_job_seq", static_cast<qint64>(record.batch_job_seq)},
          {"snapshot", json(storage::to_json(record.snapshot))}, {"metadata", json(storage::to_json(record.metadata))},
          {"pricing", json(storage::to_json(record.pricing))}, {"phase", QString::fromUtf8(core::to_string(record.phase).data())},
          {"last_updated_utc", QString::fromStdString(record.last_updated_utc)}});
    for (const auto& task : batches_)
    {
      QJsonArray ids; for (auto id : task.job_ids) ids.append(static_cast<qint64>(id));
      tasks.append(QJsonObject{{"batch_id", QString::fromStdString(task.batch_id)},
          {"display_name", QString::fromStdString(task.display_name)}, {"source_user", QString::fromStdString(task.source_user)},
          {"state", QString::fromUtf8(domain::to_string(task.state).data())}, {"job_ids", ids},
          {"period_start_utc", QString::fromStdString(task.period_start_utc)},
          {"period_end_utc", QString::fromStdString(task.period_end_utc)}, {"suspend_time_utc", QString::fromStdString(task.suspend_time_utc)},
          {"settle_order_id", QString::fromStdString(task.settle_order_id)}, {"total_pages", task.total_pages},
          {"simplex_pages", task.simplex_pages}, {"duplex_pages", task.duplex_pages}, {"document_count", task.document_count},
          {"total_price", task.total_price}, {"pending_count", task.pending_count},
          {"first_doc_anchor", anchor(task.first_doc_anchor)}, {"last_doc_anchor", anchor(task.last_doc_anchor)}});
    }
    for (const auto& order : settlementOrders_) orders.append(json(bridge::SettlementBridge::toJson(order)));
    for (const auto& event : deferred_events_)
      std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        const char* type = "lifecycle";
        if constexpr (std::is_same_v<T, core::JobSnapshot>) type = "job";
        else if constexpr (std::is_same_v<T, core::MetadataSnapshot>) type = "metadata";
        events.append(QJsonObject{{"business_date", QString::fromStdString(event.date)},
            {"type", type}, {"payload", json(storage::to_json(value))}});
      }, event.payload);
    const QJsonObject root{{"format", "wpce-shutdown-recovery"}, {"schema_version", 1}, {"manual_recovery_required", true},
        {"capture_errors", QJsonArray::fromStringList(captureErrors)},
        {"created_at_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {"business_date", boundBusinessDate()}, {"source_bucket", orderBucketPath_},
        {"current_active_batch_id", QString::fromStdString(currentActiveBatchId_)},
        {"duplicate_job_aliases", aliases}, {"previous_day_job_ids", previousIds},
        {"previous_day_fingerprints", previousIdentities}, {"ignored_placeholder_job_ids", ignoredIds},
        {"dirty", dirty_}, {"tasks", tasks}, {"jobs", jobs}, {"orders", orders}, {"deferred_events", events}};
    storage::write_text_file_atomic(QJsonDocument(root).toJson(QJsonDocument::Indented).toStdString(), target);
    return true;
  }
  catch (const std::exception& exception)
  {
    if (error) *error = QString::fromUtf8(exception.what());
    return false;
  }
}
}
