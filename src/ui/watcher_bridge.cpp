#include "ui/watcher_bridge.h"
#include "ui/worker_shutdown.h"

#include "bridge/metadata_merger.h"
#include "common/document_name.h"
#include "common/physical_job_identity.h"
#include "common/business_clock.h"
#include "common/logger.h"
#include "ipc/named_event.h"
#include "storage/job_snapshot_writer.h"

#include <QMetaObject>
#include <QCoreApplication>
#include <QEvent>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <mutex>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#include <winspool.h>
#endif

namespace wpce::ui
{

namespace
{

constexpr std::uint32_t kSyntheticJobIdBase = 0x80000000u;

std::wstring widen_utf8(const std::string_view value)
{
  if (value.empty()) return {};

  const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                        static_cast<int>(value.size()),
                                        nullptr, 0);
  if (size <= 0) return {};

  std::wstring result(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(),
                      static_cast<int>(value.size()),
                      result.data(), size);
  return result;
}

std::string narrow_utf8(const wchar_t* value)
{
  if (value == nullptr || value[0] == L'\0') return {};

  const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1,
                                        nullptr, 0, nullptr, nullptr);
  if (size <= 1) return {};

  std::string result(static_cast<std::size_t>(size) - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size,
                      nullptr, nullptr);
  return result;
}

/// Enumerate local printers. Returns empty vector on failure.
std::vector<std::wstring> listLocalPrinterNames()
{
  std::vector<std::wstring> result;

#ifdef _WIN32
  DWORD needed = 0;
  DWORD returned = 0;

  // PRINTER_ENUM_LOCAL covers locally-installed printers (USB, TCP/IP, WSD).
  // PRINTER_ENUM_CONNECTIONS adds network-shared printers (\\server\printer).
  // Both flags are needed for full coverage in mixed environments.
  EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 2,
                nullptr, 0, &needed, &returned);

  if (needed == 0) return result;

  // ── Second pass: fetch with the same flags as the probe call ──────
  // The probe above used LOCAL | CONNECTIONS to compute the buffer size;
  // using LOCAL only here would truncate shared-network printers whose
  // entries were already accounted for in the buffer, producing a
  // misleading EnumPrintersW error (ERROR_INSUFFICIENT_BUFFER or worse,
  // silent truncation of network printers in mixed environments).
  std::vector<BYTE> buffer(needed);
  if (!EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 2,
                     buffer.data(), static_cast<DWORD>(buffer.size()),
                     &needed, &returned))
  {
    return result;
  }

  const auto* printers = reinterpret_cast<PRINTER_INFO_2W*>(buffer.data());
  for (DWORD i = 0; i < returned; ++i)
  {
    result.emplace_back(printers[i].pPrinterName);
  }
#endif

  return result;
}

std::string make_job_key(const std::string& printerName, std::uint32_t rawJobId)
{
  return printerName + "\x1f" + std::to_string(rawJobId);
}

std::uint32_t allocate_synthetic_job_id(
    std::uint32_t& nextSyntheticJobId,
    const std::unordered_map<std::uint32_t, std::string>& owners)
{
  for (std::uint64_t attempts = 0;
       attempts <= std::numeric_limits<std::uint32_t>::max();
       ++attempts)
  {
    const std::uint32_t candidate = nextSyntheticJobId++;
    if (candidate == 0) continue;
    if (!owners.contains(candidate)) return candidate;
  }
  return 0;
}

/// @returns true when this snapshot's authoritative source is the XPS
/// PrintTicket XML (v4 drivers) rather than the spooler DEVMODE.
/// PrintTicket-sourced data MUST win over DEVMODE-sourced data because
/// DEVMODE is null for v4 drivers and contains stale printer defaults
/// for v3 drivers once the job has been rendered.
bool is_from_print_ticket(const core::MetadataSnapshot& m) noexcept
{
  return m.dm_fields_hex == "PrintTicket";
}

/// Merge a single field with source-awareness.
/// PrintTicket-reported values are authoritative and never overwritten
/// by spooler DEVMODE data.  Spooler DEVMODE values are only used to
/// fill fields that PrintTicket has not reported.
///
/// @param current      The cached field (may already be PrintTicket-sourced).
/// @param incoming     The newly-arrived field.
/// @param current_pt   Whether `current` came from a PrintTicket snapshot.
/// @param incoming_pt  Whether `incoming` came from a PrintTicket snapshot.
void merge_field(core::FieldValue& current,
                 const core::FieldValue& incoming,
                 bool current_pt,
                 bool incoming_pt)
{
  if (incoming.state != core::FieldState::Reported)
  {
    // Incoming has no usable value — only adopt it if we have nothing.
    const bool current_empty =
      current.state == core::FieldState::Unknown ||
      current.state == core::FieldState::NotReported;
    if (current_empty && incoming.state != core::FieldState::Unknown)
      current = incoming;
    return;
  }

  // Incoming is Reported.  Always accept if current is empty.
  if (current.state != core::FieldState::Reported)
  {
    current = incoming;
    return;
  }

  // Both are Reported — apply source authority.
  //   PrintTicket > spooler DEVMODE (v4 drivers: DEVMODE is null/stale).
  //   Same-source: let the newer value win (freshness).
  if (incoming_pt && !current_pt)
  {
    current = incoming;
    return;
  }
  if (!incoming_pt && current_pt)
  {
    // Spooler DEVMODE trying to overwrite PrintTicket — refuse.
    return;
  }

  // Same authority level — accept the newer value.
  current = incoming;
}

int positive_int_value(const core::FieldValue& field)
{
  if (field.state != core::FieldState::Reported || field.value.empty())
    return 0;
  try
  {
    int n = std::stoi(field.value);
    return n > 0 ? n : 0;
  }
  catch (...) { return 0; }
}

void merge_copies_field(core::FieldValue& current,
                        const core::FieldValue& incoming,
                        bool current_pt,
                        bool incoming_pt)
{
  const int current_n = positive_int_value(current);
  const int incoming_n = positive_int_value(incoming);

  // A later spool/driver source with copies > 1 fixes the common Word
  // path where the first PrintTicket/DEVMODE value is the default "1".
  if (incoming_n > 1 && incoming_n > current_n)
  {
    current = incoming;
    return;
  }

  // Do not let a PrintTicket default "1" erase a non-PrintTicket value
  // that already proved this is a multi-copy job.
  if (current_n > 1 && incoming_n <= 1 && incoming_pt && !current_pt)
    return;

  merge_field(current, incoming, current_pt, incoming_pt);
}

/// Field overlay that preserves the page count progress fields
/// (total_pages / pages_printed) regardless of source, because the
/// spooler tracks page progress more frequently than the PrintTicket
/// (which is captured once at job start).
void merge_progress_field(core::FieldValue& current,
                           const core::FieldValue& incoming)
{
  if (incoming.state == core::FieldState::Reported)
  {
    current = incoming;
    return;
  }
  const bool current_empty =
    current.state == core::FieldState::Unknown ||
    current.state == core::FieldState::NotReported;
  if (current_empty && incoming.state != core::FieldState::Unknown)
    current = incoming;
}

void merge_total_pages_field(core::FieldValue& current,
                             const core::FieldValue& incoming)
{
  const int current_n = positive_int_value(current);
  const int incoming_n = positive_int_value(incoming);

  // Refresh and notification-only snapshots can briefly report 0 pages
  // after a full job snapshot or EMF enrichment already found the count.
  // Treat that as "not ready yet" instead of a newer authoritative value.
  if (current_n > 0 && incoming.state == core::FieldState::Reported && incoming_n <= 0)
  {
    return;
  }

  merge_progress_field(current, incoming);
}

core::MetadataSnapshot merge_metadata_snapshots(core::MetadataSnapshot current,
                                                 const core::MetadataSnapshot& incoming)
{
  if (!incoming.printer_name.empty())
  {
    current.printer_name = incoming.printer_name;
  }

  const bool current_pt = is_from_print_ticket(current);
  const bool incoming_pt = is_from_print_ticket(incoming);

  // Page progress fields: keep a known positive total page count when a
  // later refresh reports 0 while the spooler is still settling.
  merge_total_pages_field(current.total_pages, incoming.total_pages);
  merge_progress_field(current.pages_printed, incoming.pages_printed);

  // Print-setting fields — apply source authority.
  merge_copies_field(current.copies, incoming.copies,        current_pt, incoming_pt);
  merge_field(current.duplex,        incoming.duplex,        current_pt, incoming_pt);
  merge_field(current.color,         incoming.color,         current_pt, incoming_pt);
  merge_field(current.paper_size,    incoming.paper_size,    current_pt, incoming_pt);
  merge_field(current.orientation,   incoming.orientation,   current_pt, incoming_pt);
  merge_field(current.n_up,          incoming.n_up,          current_pt, incoming_pt);
  merge_field(current.collate,       incoming.collate,       current_pt, incoming_pt);
  merge_field(current.print_quality, incoming.print_quality, current_pt, incoming_pt);
  merge_field(current.media_type,    incoming.media_type,    current_pt, incoming_pt);
  merge_field(current.page_range,    incoming.page_range,    current_pt, incoming_pt);

  // File type evidence is merged as one atomic value/source/confidence tuple.
  // A low-confidence application-title hint must never overwrite an exact
  // DWG/DWT suffix recovered later (or earlier) from the SHD spool data.
  if (incoming.file_extension.state == core::FieldState::Reported &&
      !incoming.file_extension.value.empty() &&
      (current.file_extension.state != core::FieldState::Reported ||
       current.file_extension.value.empty() ||
       incoming.file_extension_confidence >= current.file_extension_confidence))
  {
    current.file_extension = incoming.file_extension;
    current.file_extension_source = incoming.file_extension_source;
    current.file_extension_confidence = incoming.file_extension_confidence;
  }

  // Source marker — PrintTicket authority is sticky.
  if (incoming_pt || current_pt)
  {
    current.dm_fields_hex = "PrintTicket";
  }
  else if (!incoming.dm_fields_hex.empty())
  {
    current.dm_fields_hex = incoming.dm_fields_hex;
  }

  return current;
}

}  // namespace

WatcherBridge::WatcherBridge(QObject* parent, bool includeExternalInboxRoots)
    : QObject(parent), includeExternalInboxRoots_(includeExternalInboxRoots)
{
  qRegisterMetaType<core::JobSnapshot>("core::JobSnapshot");
  qRegisterMetaType<core::MetadataSnapshot>("core::MetadataSnapshot");
  qRegisterMetaType<core::JobLifecycleEvent>("core::JobLifecycleEvent");
}

WatcherBridge::~WatcherBridge()
{
  (void)stopWatching();
  // Destruction cannot leave callbacks referencing this object. Ordinary close
  // uses the bounded check and keeps the object alive on timeout.
  std::vector<QThread*> remaining;
  { std::scoped_lock lock(threadsMutex_); remaining.swap(watcherThreads_); }
  for (auto* thread : remaining) { thread->wait(); delete thread; }
}

void WatcherBridge::setStallTimeout(std::chrono::seconds timeout) noexcept
{
  stallTimeout_ = timeout;
}

void WatcherBridge::setRescanInterval(std::chrono::seconds interval) noexcept
{
  rescanInterval_ = interval;
}

void WatcherBridge::setPollTimeout(std::chrono::milliseconds timeout) noexcept
{
  pollTimeout_ = timeout;
}

void WatcherBridge::seedMinSyntheticJobId(std::uint32_t minId) noexcept
{
  std::scoped_lock lock(jobIdMutex_);
  syntheticJobIdFloor_ = std::max(kSyntheticJobIdBase, minId);
  if (nextSyntheticJobId_ < syntheticJobIdFloor_)
    nextSyntheticJobId_ = syntheticJobIdFloor_;
  forceSyntheticJobIds_ = minId > 1;
}

void WatcherBridge::resetCapturedJobMappings()
{
  std::scoped_lock lock(jobIdMutex_);
  normalizedJobIds_.clear();
  normalizedJobOwners_.clear();
  normalizedJobIdentities_.clear();
  capturedSnapshots_.clear();
  rawJobPrinters_.clear();
  nextSyntheticJobId_ = syntheticJobIdFloor_;
  for (const auto& snapshot : storedJobSeeds_)
  {
    const auto id = snapshot.job_id;
    const auto raw = snapshot.external_job_id ? snapshot.external_job_id : id;
    const auto identity = common::physical_job_identity(snapshot);
    if (!id || identity.empty()) continue;
    const auto key = make_job_key(snapshot.printer_name, raw);
    const auto previous = normalizedJobIds_.find(key);
    if (previous == normalizedJobIds_.end() || capturedSnapshots_.at(previous->second).submit_time_utc <= snapshot.submit_time_utc)
      normalizedJobIds_[key] = id;
    normalizedJobOwners_[id] = key; normalizedJobIdentities_[id] = identity;
    capturedSnapshots_[id] = snapshot;
  }
}

void WatcherBridge::startWatching(const QString& outputDir)
{
  if (running_) return;
  { std::scoped_lock lock(threadsMutex_); if (!watcherThreads_.empty()) return; }

  running_ = true;

  auto printers = listLocalPrinterNames();
  const std::filesystem::path dir(outputDir.toStdWString());

  resetCapturedJobMappings();
  {
    std::scoped_lock lock(metadataCacheMutex_);
    metadataCache_.clear();
  }

  {
    std::scoped_lock lock(printerListMutex_);
    watchedPrinters_.clear();
  }

  if (printers.empty())
  {
    running_ = false;
    emit watcherError(QStringLiteral("未发现本地打印机"));
    emit sessionFinished();
    return;
  }

  startPrintTicketInbox(dir);

  for (const auto& printerName : printers)
  {
    // Skip printers whose names suggest we can't open them.
    // We'll handle failure inside runWatcher().

    auto* thread = QThread::create(
      [this, printerName, dir]()
      {
        runWatcherWithRestart(printerName, dir);
      });

    {
      std::scoped_lock lock(printerListMutex_);
      watchedPrinters_.append(QString::fromStdWString(printerName));
    }

    {
      std::scoped_lock lock(threadsMutex_);
      watcherThreads_.push_back(thread);
    }

    thread->start();
  }
}

bool WatcherBridge::stopWatching(unsigned long timeoutMs)
{
  running_ = false;
  std::vector<QThread*> threads;
  { std::scoped_lock lock(threadsMutex_); threads.swap(watcherThreads_); }
  const bool stopped = join_worker_threads(threads, timeoutMs);
  {
    std::scoped_lock lock(threadsMutex_);
    watcherThreads_.insert(watcherThreads_.end(), threads.begin(), threads.end());
  }
  if (stopped)
  {
    // Producers are joined: all of their queued callbacks are now known.
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
    if (inboxWriter_ && inboxWriter_->running()) processPrintTicketInbox();
    if (inboxWriter_) inboxWriter_->request_stop();
    for (auto& writer : pageRecoveryWriters_) writer->request_stop();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    auto pending = [&] {
      if (inboxWriter_ && inboxWriter_->running()) return true;
      for (const auto& writer : pageRecoveryWriters_) if (writer->running()) return true;
      return false;
    };
    while (pending() && std::chrono::steady_clock::now() < deadline) QThread::msleep(10);
    if (pending()) return false;
    if (inboxWriter_)
    {
      inboxWriter_->stop();
      if (!inboxWriter_->drained()) recordStorageFailure(QString::fromStdString(inboxWriter_->last_error()));
    }
    for (auto& writer : pageRecoveryWriters_)
    {
      writer->stop();
      if (!writer->drained()) recordStorageFailure(QString::fromStdString(writer->last_error()));
    }
    pageRecoveryWriters_.clear(); pageRecoveryRoots_.clear();
    emit sessionFinished();
  }
  return stopped;
}

QStringList WatcherBridge::storageErrors() const
{
  std::scoped_lock lock(storageErrorsMutex_); return storageErrors_;
}

void WatcherBridge::recordStorageFailure(const QString& error)
{
  std::scoped_lock lock(storageErrorsMutex_);
  if (!storageErrors_.contains(error)) storageErrors_.append(error);
}

void WatcherBridge::acknowledgeRecoveryExport()
{
  std::scoped_lock lock(storageErrorsMutex_); storageErrors_.clear();
}

bool WatcherBridge::isRunning() const noexcept
{
  return running_;
}

QStringList WatcherBridge::watchedPrinters() const
{
  std::scoped_lock lock(printerListMutex_);
  return watchedPrinters_;
}

std::uint32_t WatcherBridge::normalizedJobId(const std::string& printerName,
                                             std::uint32_t rawJobId)
{
  if (rawJobId == 0)
  {
    return 0;
  }

  std::scoped_lock lock(jobIdMutex_);

  const std::string key = make_job_key(printerName, rawJobId);
  if (auto existing = normalizedJobIds_.find(key); existing != normalizedJobIds_.end())
  {
    return existing->second;
  }

  std::uint32_t normalized = forceSyntheticJobIds_ ? 0 : rawJobId;
  const auto owner = normalizedJobOwners_.find(normalized);
  if (normalized == 0 ||
      (owner != normalizedJobOwners_.end() && owner->second != key))
  {
    normalized = allocate_synthetic_job_id(nextSyntheticJobId_, normalizedJobOwners_);
    if (normalized == 0) return 0;
  }

  normalizedJobIds_[key] = normalized;
  normalizedJobOwners_[normalized] = key;

  const auto rawOwner = rawJobPrinters_.find(rawJobId);
  if (rawOwner == rawJobPrinters_.end())
  {
    rawJobPrinters_[rawJobId] = printerName;
  }
  else if (rawOwner->second != printerName)
  {
    rawJobPrinters_[rawJobId].clear();
  }

  return normalized;
}

std::uint32_t WatcherBridge::normalizedJobIdForSnapshot(
    const std::string& printerName,
    std::uint32_t rawJobId,
    const core::JobSnapshot& snapshot)
{
  if (rawJobId == 0)
  {
    return 0;
  }

  const std::string key = make_job_key(printerName, rawJobId);
  const std::string incomingIdentity = common::physical_job_identity(snapshot);

  std::scoped_lock lock(jobIdMutex_);

  if (auto existing = normalizedJobIds_.find(key); existing != normalizedJobIds_.end())
  {
    const std::uint32_t normalized = existing->second;
    auto oldIdentity = normalizedJobIdentities_.find(normalized);
    if (!incomingIdentity.empty() &&
        oldIdentity != normalizedJobIdentities_.end() &&
        !oldIdentity->second.empty() &&
        oldIdentity->second != incomingIdentity)
    {
      // The spooler reused the same raw JobId for a different physical job.
      // Keep the old normalized id reserved for historical callbacks and give
      // the new job a fresh id instead of overwriting the previous billable job.
      const std::uint32_t recycled =
          allocate_synthetic_job_id(nextSyntheticJobId_, normalizedJobOwners_);
      if (recycled == 0) return 0;

      normalizedJobIds_[key] = recycled;
      normalizedJobOwners_[recycled] = key;
      normalizedJobIdentities_[recycled] = incomingIdentity;
      return recycled;
    }

    if (!incomingIdentity.empty())
    {
      normalizedJobIdentities_[normalized] = incomingIdentity;
    }
    return normalized;
  }

  std::uint32_t normalized = forceSyntheticJobIds_ ? 0 : rawJobId;
  const auto owner = normalizedJobOwners_.find(normalized);
  if (normalized == 0 ||
      (owner != normalizedJobOwners_.end() && owner->second != key))
  {
    normalized = allocate_synthetic_job_id(nextSyntheticJobId_, normalizedJobOwners_);
    if (normalized == 0) return 0;
  }

  normalizedJobIds_[key] = normalized;
  normalizedJobOwners_[normalized] = key;
  if (!incomingIdentity.empty())
  {
    normalizedJobIdentities_[normalized] = incomingIdentity;
  }

  const auto rawOwner = rawJobPrinters_.find(rawJobId);
  if (rawOwner == rawJobPrinters_.end())
  {
    rawJobPrinters_[rawJobId] = printerName;
  }
  else if (rawOwner->second != printerName)
  {
    rawJobPrinters_[rawJobId].clear();
  }

  return normalized;
}

std::optional<std::string> WatcherBridge::uniquePrinterForRawJobId(std::uint32_t rawJobId) const
{
  std::scoped_lock lock(jobIdMutex_);
  auto it = rawJobPrinters_.find(rawJobId);
  if (it == rawJobPrinters_.end() || it->second.empty())
  {
    return std::nullopt;
  }
  return it->second;
}

core::JobSnapshot WatcherBridge::normalizeJobSnapshot(core::JobSnapshot snapshot,
                                                      const std::string& fallbackPrinter)
{
  if (snapshot.printer_name.empty())
  {
    snapshot.printer_name = fallbackPrinter;
  }
  const std::uint32_t rawJobId = snapshot.job_id;
  snapshot.external_job_id = rawJobId;
  const std::string identity = common::physical_job_identity(snapshot);
  snapshot.idempotency_key = make_job_key(snapshot.printer_name, rawJobId);
  if (!identity.empty()) snapshot.idempotency_key += "\x1f" + identity;
  snapshot.job_id = normalizedJobIdForSnapshot(snapshot.printer_name, rawJobId, snapshot);
  { std::scoped_lock lock(jobIdMutex_); capturedSnapshots_[snapshot.job_id] = snapshot; }
  return snapshot;
}

core::MetadataSnapshot WatcherBridge::normalizeMetadata(core::MetadataSnapshot metadata,
                                                        const std::string& fallbackPrinter)
{
  if (metadata.printer_name.empty())
  {
    if (auto uniquePrinter = uniquePrinterForRawJobId(metadata.job_id); uniquePrinter.has_value())
    {
      metadata.printer_name = *uniquePrinter;
    }
    else
    {
      metadata.printer_name = fallbackPrinter;
    }
  }

  metadata.job_id = normalizedJobId(metadata.printer_name, metadata.job_id);

  std::scoped_lock lock(metadataCacheMutex_);
  auto it = metadataCache_.find(metadata.job_id);
  if (it == metadataCache_.end())
  {
    it = metadataCache_.insert({metadata.job_id, std::move(metadata)}).first;
  }
  else
  {
    it->second = merge_metadata_snapshots(std::move(it->second), metadata);
  }
  return it->second;
}

core::MetadataSnapshot WatcherBridge::mergeCapturedMetadata(core::MetadataSnapshot metadata)
{
  std::scoped_lock lock(metadataCacheMutex_);
  const auto id = metadata.job_id;
  auto it = metadataCache_.find(id);
  if (it == metadataCache_.end()) it = metadataCache_.emplace(id, std::move(metadata)).first;
  else it->second = merge_metadata_snapshots(std::move(it->second), metadata);
  return it->second;
}

core::JobLifecycleEvent WatcherBridge::normalizeLifecycleEvent(core::JobLifecycleEvent event,
                                                               const std::string& fallbackPrinter)
{
  event.job_id = normalizedJobId(fallbackPrinter, event.job_id);
  event.event_id = std::to_string(event.job_id) + "-" + std::to_string(event.sequence);
  return event;
}

void WatcherBridge::runWatcher(const std::wstring& printerName,
                               const std::filesystem::path& outputDir)
{
  try
  {
    const std::string printer_name_str = narrow_utf8(printerName.c_str());

    // Build the log callback early so we can log bridge-level events too.
    auto log_callback = [printer_name_str](
                          watcher::LogLevel level,
                          std::string_view category,
                          const std::string& message) {
      // ── 1. Ring-buffer logger (structured, queryable from UI) ──────
      {
        common::LogSeverity sev = common::LogSeverity::Debug;
        switch (level)
        {
          case watcher::LogLevel::Debug: sev = common::LogSeverity::Debug; break;
          case watcher::LogLevel::Info:  sev = common::LogSeverity::Info;  break;
          case watcher::LogLevel::Warn:  sev = common::LogSeverity::Warn;  break;
          case watcher::LogLevel::Error: sev = common::LogSeverity::Error; break;
        }
        common::RingBufferLogger::instance().log(
          sev, category,
          printer_name_str + ": " + message);
      }

      // ── 2. OutputDebugStringA (legacy, picked up by DebugView) ─────
      {
        const char* level_str = "?";
        switch (level)
        {
          case watcher::LogLevel::Debug: level_str = "D"; break;
          case watcher::LogLevel::Info:  level_str = "I"; break;
          case watcher::LogLevel::Warn:  level_str = "W"; break;
          case watcher::LogLevel::Error: level_str = "E"; break;
        }
        char buf[1024];
        std::snprintf(buf, sizeof(buf), "[%s|%s|%.*s] %s\n",
                      printer_name_str.c_str(), level_str,
                      static_cast<int>(category.size()), category.data(),
                      message.c_str());
        OutputDebugStringA(buf);
      }
    };

    // Short test: can we open this printer?
    // RAII guard so the handle is closed even if log_callback throws.
    {
      HANDLE hTest = nullptr;
      if (!OpenPrinterW(const_cast<LPWSTR>(printerName.c_str()),
                        &hTest, nullptr))
      {
        log_callback(watcher::LogLevel::Warn, "bridge",
                     "Printer \"" + narrow_utf8(printerName.c_str()) + "\" skipped — OpenPrinterW failed");
        QMetaObject::invokeMethod(this, [this, pName = QString::fromStdWString(printerName)]() {
          emit printerSkipped(pName);
        }, Qt::QueuedConnection);
        return;
      }
      struct Guard { HANDLE h; ~Guard() { if (h) ClosePrinter(h); } } guard{hTest};
      (void)guard; // extend lifetime to end of scope
    }

    log_callback(watcher::LogLevel::Info, "bridge",
                 "Printer \"" + narrow_utf8(printerName.c_str()) + "\" opened, starting watcher");

    storage::AsyncJobSnapshotWriter writer(
      storage::JobSnapshotWriterConfig{.output_directory = outputDir, .rotate_daily = true});
    writer.start();

    // │─── Signal wiring ──────────────────────────────────────────────│
    // Store the last-known job snapshot per job_id so the metadata
    // callback always has the right pairing — even when dedup skips
    // the job callback (signature unchanged).
    std::unordered_map<std::uint32_t, core::JobSnapshot> jobCache;
    std::unordered_map<std::uint32_t, std::string> jobCaptureDates;
    std::unordered_set<std::uint32_t> ignoredPlaceholderJobs;
    std::mutex pendingMutex;

    // Keep a copy for bridge-level logging - log_callback will be moved
    // into PrintWatcher below and the moved-from local must not be called.
    auto bridge_log = log_callback;

    auto jobCallback = [&](const core::JobSnapshot& rawSnapshot)
    {
      core::JobSnapshot snapshot = normalizeJobSnapshot(rawSnapshot, printer_name_str);
      if (common::is_spooler_placeholder_document(snapshot.document_name))
      {
        {
          std::scoped_lock lock(pendingMutex);
          ignoredPlaceholderJobs.insert(snapshot.job_id);
          jobCache.erase(snapshot.job_id);
        }
        bridge_log(watcher::LogLevel::Info, "filter",
                   "Job " + std::to_string(snapshot.job_id) +
                   ": ignored spooler placeholder document");
        return;
      }
      QMetaObject::invokeMethod(this, [this, snapshot]() {
        emit jobSnapshotReceived(snapshot);
      }, Qt::QueuedConnection);
      std::scoped_lock lock(pendingMutex);
      jobCache[snapshot.job_id] = snapshot;
      jobCaptureDates.try_emplace(snapshot.job_id, common::current_business_date().yyyy_mm_dd);
    };

    // ── Queue-full sentinel ────────────────────────────────────────
    // Tracks consecutive enqueue failures so we emit exactly one
    // warning at onset and one recovery message when the queue drains,
    // avoiding log spam from every dropped job.
    bool storage_was_unavailable = false;
    int consecutive_rejections = 0;
    constexpr int kStorageWarnThreshold = 3;   // emit after N consecutive drops

    auto metadataCallback = [&, log_callback](const core::MetadataSnapshot& rawMetadata)
    {
      core::MetadataSnapshot metadata = normalizeMetadata(rawMetadata, printer_name_str);
      {
        std::scoped_lock lock(pendingMutex);
        if (ignoredPlaceholderJobs.find(metadata.job_id) != ignoredPlaceholderJobs.end())
          return;
      }
      QMetaObject::invokeMethod(this, [this, metadata]() {
        emit metadataReceived(metadata);
      }, Qt::QueuedConnection);
      core::JobSnapshot job;
      {
        std::scoped_lock lock(pendingMutex);
        auto it = jobCache.find(metadata.job_id);
        if (it != jobCache.end())
        {
          job = it->second;
        }
      }
      // Stub fallback — should rarely be needed now.
      if (job.job_id == 0)
      {
        job.job_id = metadata.job_id;
        job.printer_name = narrow_utf8(printerName.c_str());
      }

      // ── Metadata trace logging ──────────────────────────────────
      {
        std::ostringstream meta_log;
        meta_log << "Metadata for Job " << metadata.job_id << ": ";
        auto field = [&](const char* name, const core::FieldValue& fv) {
          meta_log << name << "=";
          if (fv.state == core::FieldState::Reported) {
            meta_log << fv.value;
          } else {
            meta_log << "[" << core::to_string(fv.state) << "]";
          }
          meta_log << " ";
        };
        field("pages", metadata.total_pages);
        field("printed", metadata.pages_printed);
        field("copies", metadata.copies);
        field("duplex", metadata.duplex);
        field("color", metadata.color);
        field("paper", metadata.paper_size);
        field("orient", metadata.orientation);
        field("nup", metadata.n_up);
        field("collate", metadata.collate);
        field("quality", metadata.print_quality);
        field("media", metadata.media_type);
        if (!metadata.dm_fields_hex.empty()) {
          meta_log << "dm_fields=" << metadata.dm_fields_hex;
        }
        log_callback(watcher::LogLevel::Debug, "meta", meta_log.str());
      }

      const bool ok = writer.enqueue(std::move(job), metadata);
      if (!ok)
      {
        recordStorageFailure(QString::fromStdString(printer_name_str + ": " + writer.last_error()));
        ++consecutive_rejections;
        if (!storage_was_unavailable && consecutive_rejections >= kStorageWarnThreshold)
        {
          storage_was_unavailable = true;
          auto printer_utf8 = narrow_utf8(printerName.c_str());
          QMetaObject::invokeMethod(this, [this, p = std::move(printer_utf8)]() {
            emit storageWarning(QStringLiteral("采集暂存失败 — 请检查 %1 的数据目录")
                                  .arg(QString::fromStdString(p)));
          }, Qt::QueuedConnection);
        }
      }
      else
      {
        if (storage_was_unavailable && consecutive_rejections > 0)
        {
          storage_was_unavailable = false;
          QMetaObject::invokeMethod(this, [this]() {
            emit storageWarning(QStringLiteral("采集暂存已恢复"));
          }, Qt::QueuedConnection);
        }
        consecutive_rejections = 0;
      }
    };

    auto lifecycleCallback = [&](const core::JobLifecycleEvent& rawEvent)
    {
      core::JobLifecycleEvent event = normalizeLifecycleEvent(rawEvent, printer_name_str);
      {
        std::scoped_lock lock(pendingMutex);
        if (ignoredPlaceholderJobs.find(event.job_id) != ignoredPlaceholderJobs.end())
          return;
      }

      // Do not drop normalized JobId mappings on the first terminal status.
      // Windows often sends retained/deleting/printed updates after a job first
      // appears completed; clearing here can split one physical print job into
      // multiple synthetic IDs and therefore multiple billable rows.

      QMetaObject::invokeMethod(this, [this, event]() {
        emit lifecycleEventReceived(event);
      }, Qt::QueuedConnection);
    };

    watcher::PrintWatcher watcher(
      watcher::WatcherConfig{
        .printer_name = printerName,
        .wait_timeout = pollTimeout_,
        .stall_timeout = stallTimeout_,
        .rescan_interval = rescanInterval_,
        .log_callback = std::move(log_callback),
      },
      std::move(jobCallback),
      std::move(metadataCallback),
      std::move(lifecycleCallback));

    watcher.start();

    // ── Heartbeat: snapshot printer name once for the loop ────────────
    const QString heartbeatPrinter = QString::fromStdWString(printerName);

    std::string storage_health;
    int tick_count = 0;
    while (running_ && watcher.running())
    {
      QThread::msleep(50);

      ++tick_count;

      // Heartbeat — fires every ~1 s (20 ticks).
      if (tick_count % 20 == 0)
      {
        const auto error = writer.last_error();
        if (error != storage_health)
        {
          storage_health = error;
          QMetaObject::invokeMethod(this, [this, failed = !error.empty()]() {
            emit storageWarning(failed ? QStringLiteral("采集文件暂未写入，数据已保留待重试，请检查目录。") : QStringLiteral("采集存储已恢复。"));
          }, Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(this, [this, heartbeatPrinter]() {
          emit watcherHeartbeat(heartbeatPrinter);
        }, Qt::QueuedConnection);
      }


    }

    watcher.stop();
    writer.stop();
    if (!writer.last_error().empty())
      recordStorageFailure(QString::fromStdString(printer_name_str + ": " + writer.last_error()));

    auto summaries = watcher.lifecycle_summaries();
    if (!summaries.empty())
    {
      std::map<std::string, std::vector<core::JobLifecycleSummary>> dailySummaries;
      for (auto& summary : summaries)
      {
        summary.job_id = normalizedJobId(summary.printer_name.empty() ? printer_name_str : summary.printer_name,
                                        summary.job_id);
        const auto date = jobCaptureDates.find(summary.job_id);
        dailySummaries[date == jobCaptureDates.end() ? common::current_business_date().yyyy_mm_dd : date->second]
            .push_back(std::move(summary));
      }
      for (const auto& [date, daily] : dailySummaries)
        storage::write_lifecycle_summaries_atomic(daily,
            storage::JobSnapshotWriterConfig{.output_directory = outputDir.parent_path() / date});
    }
  }
  catch (const std::exception& error)
  {
    recordStorageFailure(QString::fromUtf8(error.what()));
    QMetaObject::invokeMethod(this, [this, msg = QString::fromStdString(error.what())]() {
      emit watcherError(msg);
    }, Qt::QueuedConnection);
  }
  catch (...)
  {
    QMetaObject::invokeMethod(this, [this, msg = QStringLiteral("Unknown error watching %1")
                      .arg(QString::fromStdWString(printerName))]() {
      emit watcherError(msg);
    }, Qt::QueuedConnection);
  }
}

// ── Watcher Auto-Restart ───────────────────────────────────────────────

void WatcherBridge::runWatcherWithRestart(const std::wstring& printerName,
                                          const std::filesystem::path& outputDir)
{
  constexpr int kMaxRestarts = 5;
  constexpr std::chrono::minutes kRestartWindow{5};
  constexpr std::chrono::seconds kResetAfterStable{60};

  // Exponential backoff: 1s, 2s, 4s, 8s, 16s, 32s, 60s (capped).
  constexpr std::chrono::seconds kBackoffSequence[] = {
    std::chrono::seconds{1},
    std::chrono::seconds{2},
    std::chrono::seconds{4},
    std::chrono::seconds{8},
    std::chrono::seconds{16},
    std::chrono::seconds{32},
    std::chrono::seconds{60},
  };

  const auto printer_utf8 = narrow_utf8(printerName.c_str());

  for (;;)
  {
    // ── Check permanent-failure state ─────────────────────────────────
    {
      std::scoped_lock lock(restartStateMutex_);
      auto it = restartStates_.find(printerName);
      if (it != restartStates_.end() && it->second.permanent_failure)
      {
        return;  // Permanently given up on this printer.
      }
    }

    // ── Run the watcher ───────────────────────────────────────────────
    runWatcher(printerName, outputDir);

    // ── Exited. Was it intentional? ───────────────────────────────────
    if (!running_)
    {
      // stopWatching() was called — clean exit, no restart.
      return;
    }

    // ── Unexpected exit — schedule restart with backoff ───────────────
    const auto now = std::chrono::steady_clock::now();
    std::chrono::seconds delay{1};

    {
      std::scoped_lock lock(restartStateMutex_);
      auto& state = restartStates_[printerName];
      state.printer_name = printerName;

      if (state.restart_count == 0)
      {
        state.first_restart_time = now;
      }

      ++state.restart_count;

      // ── Budget check ──────────────────────────────────────────────────
      if (state.restart_count > kMaxRestarts &&
          now - state.first_restart_time < kRestartWindow)
      {
        state.permanent_failure = true;
        auto name = QString::fromStdWString(printerName);
        auto error_msg = QStringLiteral("在 %1 分钟内崩溃 %2 次，已停止重试")
                           .arg(kRestartWindow.count())
                           .arg(kMaxRestarts);
        common::log_error("bridge",
          printer_utf8 + ": permanent failure — " +
          std::to_string(kMaxRestarts) + " crashes in " +
          std::to_string(kRestartWindow.count()) + " min");

        QMetaObject::invokeMethod(this, [this, name = std::move(name),
                                          error = std::move(error_msg)]() {
          emit watcherPermanentFailure(name, error);
        }, Qt::QueuedConnection);
        return;
      }

      // ── Reset counter if previous session was stable ──────────────────
      if (state.restart_count > 1)
      {
        auto elapsed_since_first =
          std::chrono::duration_cast<std::chrono::seconds>(
            now - state.first_restart_time);
        if (elapsed_since_first > kResetAfterStable)
        {
          // Ran stably for > 60 s before this crash — reset the counter.
          state.restart_count = 1;
          state.first_restart_time = now;
        }
      }

      // ── Compute backoff ───────────────────────────────────────────────
      const int backoff_index = std::min(
        state.restart_count - 1,
        static_cast<int>(std::size(kBackoffSequence)) - 1);
      delay = kBackoffSequence[backoff_index];
      state.current_backoff = delay;

      common::log_warn("bridge",
        printer_utf8 + ": watcher exited unexpectedly, restart " +
        std::to_string(state.restart_count) + "/" +
        std::to_string(kMaxRestarts) + " after " +
        std::to_string(delay.count()) + " s backoff");
    }  // lock released here

    // ── Sleep with backoff ────────────────────────────────────────────
    const auto deadline = std::chrono::steady_clock::now() + delay;
    while (std::chrono::steady_clock::now() < deadline)
    {
      if (!running_)
      {
        return;  // stopWatching() called during backoff.
      }
      QThread::msleep(100);
    }
  }
}

}  // namespace wpce::ui
