#include "ui/watcher_bridge.h"
#include "bridge/print_ticket_inbox.h"
#include <algorithm>
#include <QMetaObject>
#include "common/business_clock.h"
#include "ipc/named_event.h"

namespace wpce::ui
{
void WatcherBridge::seedStoredJobs(std::vector<core::JobSnapshot> jobs)
{
  storedJobSeeds_ = std::move(jobs);
}
void WatcherBridge::setPrintTicketAcknowledgement(std::function<bool(std::uint32_t)> callback)
{ inboxAcknowledgement_ = std::move(callback); }
void WatcherBridge::startPrintTicketInbox(const std::filesystem::path& output)
{
  inboxOutput_ = output;
  inboxWriter_ = std::make_unique<storage::AsyncJobSnapshotWriter>(storage::JobSnapshotWriterConfig{
      .output_directory=output, .rotate_daily=true});
  inboxWriter_->start();
  { std::scoped_lock lock(inboxDeliveryMutex_); inboxDeliveries_.clear(); }
  auto* worker = QThread::create([this] {
    std::unique_ptr<ipc::NamedEvent> ready;
    if (includeExternalInboxRoots_)
      ready = std::make_unique<ipc::NamedEvent>(ipc::kPTMetadataReadyEventName, ipc::EventRole::Creator);
    while (running_)
    {
      if (ready && ready->valid()) ready->reset();
      processPrintTicketInbox();
      if (ready && ready->valid()) (void)ready->wait(500);
      else QThread::msleep(100);
    }
  });
  { std::scoped_lock lock(threadsMutex_); watcherThreads_.push_back(worker); }
  worker->start();
}
void WatcherBridge::processPrintTicketInbox()
{
  if (inboxOutput_.empty() || !inboxWriter_) return;
  try
  {
  const auto roots = includeExternalInboxRoots_ ? bridge::pt_inbox_directories(inboxOutput_) :
      std::vector<std::filesystem::path>{inboxOutput_, inboxOutput_.parent_path()};
  for (const auto& root : roots)
  {
    std::error_code error;
    if (!std::filesystem::is_directory(root / "_writer-spool/pages", error)) continue;
    const auto canonical = std::filesystem::weakly_canonical(root, error);
    if (error || std::find(pageRecoveryRoots_.begin(), pageRecoveryRoots_.end(), canonical) != pageRecoveryRoots_.end()) continue;
    if (pageRecoveryRoots_.size() >= 8) { recordStorageFailure(QStringLiteral("页面恢复目录数量超出本次会话限制，请重新启动监听后重试。")); break; }
    auto writer = std::make_unique<storage::ReliableFileWriter>(storage::ReliableWriterConfig{.root=canonical,.queue_name="pages"});
    writer->start(); pageRecoveryRoots_.push_back(canonical); pageRecoveryWriters_.push_back(std::move(writer));
  }
  QString health;
  if (!inboxWriter_->last_error().empty()) health = QStringLiteral("采集数据正在等待补写，请检查数据目录。 ");
  for (const auto& writer : pageRecoveryWriters_)
    if (!writer->last_error().empty()) health = QStringLiteral("页面数据正在等待补写，请检查数据目录。");
  if (health != inboxHealth_)
  { inboxHealth_ = health; emit storageWarning(health.isEmpty() ? QStringLiteral("采集存储已恢复。") : health); }
  const auto result = bridge::consume_pt_inbox(roots,
      [this](const bridge::PrintTicketMessage& message) {
        const auto digest = storage::stable_digest(bridge::serialize_pt_message(message));
        std::optional<InboxDelivery> archive;
        {
          std::scoped_lock lock(inboxDeliveryMutex_);
          const auto existing = inboxDeliveries_.find(message.message_id);
          if (existing != inboxDeliveries_.end())
          {
            if (existing->second.digest != digest) return false;
            if (existing->second.archived) return true;
            if (existing->second.accepted) archive = existing->second;
            else
            {
              if (existing->second.waiting || !running_) return false;
              inboxDeliveries_.erase(existing);
            }
          }
          if (!archive)
          {
            if (!running_ || inboxDeliveries_.size() >= 32) return false;
            inboxDeliveries_.emplace(message.message_id, InboxDelivery{digest});
          }
        }
        if (archive)
        {
          if (!inboxWriter_->enqueue(std::move(archive->snapshot), std::move(archive->metadata), archive->capture_date)) return false;
          std::scoped_lock lock(inboxDeliveryMutex_);
          const auto state = inboxDeliveries_.find(message.message_id);
          if (state != inboxDeliveries_.end()) state->second.archived = true;
          return true;
        }
        // No filesystem callback waits for the GUI. The claim stays durable
        // until a later poll observes the application's persistence result.
        QMetaObject::invokeMethod(this, [this, message] {
          bool accepted = false;
          core::JobSnapshot snapshot;
          core::MetadataSnapshot metadata;
          const auto capture_date = common::current_business_date().yyyy_mm_dd;
          try
          {
            {
              std::scoped_lock lock(jobIdMutex_);
              const auto id = bridge::match_pt_job(message, capturedSnapshots_);
              if (id) snapshot = capturedSnapshots_.at(id);
            }
            if (snapshot.job_id && inboxAcknowledgement_ && inboxAcknowledgement_(snapshot.job_id))
            {
              metadata = message.metadata; metadata.job_id = snapshot.job_id; metadata.dm_fields_hex = "PrintTicket";
              metadata = mergeCapturedMetadata(std::move(metadata));
              emit metadataReceived(metadata);
              accepted = inboxAcknowledgement_(snapshot.job_id);
            }
          }
          catch (const std::exception& error) { recordStorageFailure(QString::fromUtf8(error.what())); }
          catch (...) { recordStorageFailure(QStringLiteral("PrintTicket 交付异常，原消息已保留。")); }
          std::scoped_lock lock(inboxDeliveryMutex_);
          const auto state = inboxDeliveries_.find(message.message_id);
          if (state != inboxDeliveries_.end())
          {
            state->second.waiting = false; state->second.accepted = accepted;
            if (accepted)
            {
              state->second.snapshot = std::move(snapshot); state->second.metadata = std::move(metadata);
              state->second.capture_date = capture_date;
            }
          }
          if (includeExternalInboxRoots_)
          {
            ipc::NamedEvent ready(ipc::kPTMetadataReadyEventName, ipc::EventRole::Opener);
            if (ready.valid()) ready.signal();
          }
        }, Qt::QueuedConnection);
        return false;
      }, 8);
  {
    std::scoped_lock lock(inboxDeliveryMutex_);
    for (const auto& id : result.acknowledged_ids) inboxDeliveries_.erase(id);
  }
  for (const auto& error : result.errors) recordStorageFailure(QString::fromStdString(error));
  if (result.quarantined) emit storageWarning(QStringLiteral("部分 PrintTicket 身份不完整或内容冲突，已保留到隔离目录，请核对。"));
  }
  catch (const std::exception& error) { recordStorageFailure(QString::fromUtf8(error.what())); }
}
}
