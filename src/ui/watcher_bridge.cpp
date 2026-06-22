#include "ui/watcher_bridge.h"

#include "bridge/metadata_merger.h"
#include "common/logger.h"
#include "ipc/named_event.h"
#include "storage/job_snapshot_writer.h"

#include <QMetaObject>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
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

core::MetadataSnapshot merge_metadata_snapshots(core::MetadataSnapshot current,
                                                 const core::MetadataSnapshot& incoming)
{
  if (!incoming.printer_name.empty())
  {
    current.printer_name = incoming.printer_name;
  }

  const bool current_pt = is_from_print_ticket(current);
  const bool incoming_pt = is_from_print_ticket(incoming);

  // Page progress fields — always take the freshest reported value
  // regardless of source (spooler updates these during printing).
  merge_progress_field(current.total_pages,   incoming.total_pages);
  merge_progress_field(current.pages_printed, incoming.pages_printed);

  // Print-setting fields — apply source authority.
  merge_field(current.copies,        incoming.copies,        current_pt, incoming_pt);
  merge_field(current.duplex,        incoming.duplex,        current_pt, incoming_pt);
  merge_field(current.color,         incoming.color,         current_pt, incoming_pt);
  merge_field(current.paper_size,    incoming.paper_size,    current_pt, incoming_pt);
  merge_field(current.orientation,   incoming.orientation,   current_pt, incoming_pt);
  merge_field(current.n_up,          incoming.n_up,          current_pt, incoming_pt);
  merge_field(current.collate,       incoming.collate,       current_pt, incoming_pt);
  merge_field(current.print_quality, incoming.print_quality, current_pt, incoming_pt);
  merge_field(current.media_type,    incoming.media_type,    current_pt, incoming_pt);
  merge_field(current.page_range,    incoming.page_range,    current_pt, incoming_pt);

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

WatcherBridge::WatcherBridge(QObject* parent) : QObject(parent) {}

WatcherBridge::~WatcherBridge()
{
  stopWatching();
}

void WatcherBridge::setStallTimeout(std::chrono::seconds timeout) noexcept
{
  stallTimeout_ = timeout;
}

void WatcherBridge::startWatching(const QString& outputDir)
{
  if (running_) return;

  running_ = true;

  auto printers = listLocalPrinterNames();
  const std::filesystem::path dir(outputDir.toStdWString());

  {
    std::scoped_lock lock(jobIdMutex_);
    normalizedJobIds_.clear();
    normalizedJobOwners_.clear();
    rawJobPrinters_.clear();
    nextSyntheticJobId_ = 0x80000000u;
  }
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

void WatcherBridge::stopWatching()
{
  if (!running_) return;

  running_ = false;

  std::vector<QThread*> threads;
  {
    std::scoped_lock lock(threadsMutex_);
    threads.swap(watcherThreads_);
  }

  for (QThread* thread : threads)
  {
    if (thread != nullptr)
    {
      thread->quit();
      thread->wait(5000);
      thread->deleteLater();
    }
  }

  emit sessionFinished();
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

  std::uint32_t normalized = rawJobId;
  if (auto owner = normalizedJobOwners_.find(normalized);
      owner != normalizedJobOwners_.end() && owner->second != key)
  {
    do
    {
      normalized = nextSyntheticJobId_++;
    }
    while (normalized == 0 || normalizedJobOwners_.contains(normalized));
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
  snapshot.job_id = normalizedJobId(snapshot.printer_name, snapshot.job_id);
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
                          std::string message) {
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
    {
      HANDLE hTest = nullptr;
      if (!OpenPrinterW(const_cast<LPWSTR>(printerName.c_str()),
                        &hTest, nullptr))
      {
        log_callback(watcher::LogLevel::Warn, "bridge",
                     "Printer \"" + narrow_utf8(printerName.c_str()) + "\" skipped — OpenPrinterW failed");
        emit printerSkipped(QString::fromStdWString(printerName));
        return;
      }
      ClosePrinter(hTest);
    }

    log_callback(watcher::LogLevel::Info, "bridge",
                 "Printer \"" + narrow_utf8(printerName.c_str()) + "\" opened, starting watcher");

    storage::AsyncJobSnapshotWriter writer(
      storage::JobSnapshotWriterConfig{.output_directory = outputDir});
    writer.start();

    // │─── Signal wiring ──────────────────────────────────────────────│
    // Store the last-known job snapshot per job_id so the metadata
    // callback always has the right pairing — even when dedup skips
    // the job callback (signature unchanged).
    std::unordered_map<std::uint32_t, core::JobSnapshot> jobCache;
    std::mutex pendingMutex;

    auto jobCallback = [&](const core::JobSnapshot& rawSnapshot)
    {
      core::JobSnapshot snapshot = normalizeJobSnapshot(rawSnapshot, printer_name_str);
      emit jobSnapshotReceived(snapshot);
      std::scoped_lock lock(pendingMutex);
      jobCache[snapshot.job_id] = snapshot;
    };

    // ── Queue-full sentinel ────────────────────────────────────────
    // Tracks consecutive enqueue failures so we emit exactly one
    // warning at onset and one recovery message when the queue drains,
    // avoiding log spam from every dropped job.
    bool queue_was_full = false;
    int consecutive_full = 0;
    constexpr int kFullWarnThreshold = 3;   // emit after N consecutive drops

    auto metadataCallback = [&](const core::MetadataSnapshot& rawMetadata)
    {
      core::MetadataSnapshot metadata = normalizeMetadata(rawMetadata, printer_name_str);
      emit metadataReceived(metadata);
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
        ++consecutive_full;
        if (!queue_was_full && consecutive_full >= kFullWarnThreshold)
        {
          queue_was_full = true;
          auto printer_utf8 = narrow_utf8(printerName.c_str());
          QMetaObject::invokeMethod(this, [this, p = std::move(printer_utf8)]() {
            emit storageWarning(QStringLiteral("存储队列已满 — %1 的数据可能丢失")
                                  .arg(QString::fromStdString(p)));
          }, Qt::QueuedConnection);
        }
      }
      else
      {
        if (queue_was_full && consecutive_full > 0)
        {
          queue_was_full = false;
          QMetaObject::invokeMethod(this, [this]() {
            emit storageWarning(QStringLiteral("存储队列已恢复"));
          }, Qt::QueuedConnection);
        }
        consecutive_full = 0;
      }
    };

    auto lifecycleCallback = [&](const core::JobLifecycleEvent& rawEvent)
    {
      core::JobLifecycleEvent event = normalizeLifecycleEvent(rawEvent, printer_name_str);
      emit lifecycleEventReceived(event);
    };

    watcher::PrintWatcher watcher(
      watcher::WatcherConfig{
        .printer_name = printerName,
        .wait_timeout = std::chrono::milliseconds{250},
        .stall_timeout = stallTimeout_,
        .log_callback = std::move(log_callback),
      },
      std::move(jobCallback),
      std::move(metadataCallback),
      std::move(lifecycleCallback));

    watcher.start();

    // ── Periodic PrintTicket metadata scan ──────────────────────────────
    // The XPS filter DLL (wpce_xps_filter) writes job-N-pt-metadata.json
    // from inside the spooler process.  Scan the output directory every
    // 3 s and emit any new PrintTicket metadata into the UI pipeline.
    // Track seen files to avoid re-emitting stale data.
    std::unordered_set<std::string> seen_pt_files;
    // Seed with files already present (don't replay old sessions).
    {
      for (const auto& entry : std::filesystem::directory_iterator(outputDir))
      {
        auto n = entry.path().filename().string();
        if (n.starts_with("job-") && n.ends_with("-pt-metadata.json"))
          seen_pt_files.insert(std::move(n));
      }
    }

    // ── Named event IPC ──────────────────────────────────────────────
    // Creates (or opens) the global named event used by the XPS filter
    // DLL to signal that new PrintTicket metadata is ready on disk.
    // When the event is valid, WaitForSingleObject replaces the 50 ms
    // sleep — the loop wakes up instantly when the filter signals.
    ipc::NamedEvent pt_event(ipc::kPTMetadataReadyEventName,
                             ipc::EventRole::Opener);
    if (pt_event.valid())
    {
      log_callback(watcher::LogLevel::Info, "bridge",
                   "Named event IPC active — real-time PrintTicket delivery");
    }
    else
    {
      log_callback(watcher::LogLevel::Warn, "bridge",
                   "Named event IPC unavailable — falling back to 3 s polling");
    }

    // ── Heartbeat: snapshot printer name once for the loop ────────────
    const QString heartbeatPrinter = QString::fromStdWString(printerName);

    // Lambda for scanning & emitting PT metadata — called both on the
    // fast IPC path and the periodic fallback path.
    const auto scan_pt_metadata_now = [&]() {
      // cleanup_after_load = true: delete PT metadata files after
      // loading so they don't accumulate across long sessions and
      // don't get re-processed after a watcher thread restart.
      bridge::scan_and_merge_pt_metadata(
        outputDir,
        [&](core::MetadataSnapshot pt_meta) {
          std::string key = pt_meta.printer_name + "\x1f" + std::to_string(pt_meta.job_id);
          if (!seen_pt_files.insert(std::move(key)).second)
            return;  // Already consumed this file.

          pt_meta.dm_fields_hex = "PrintTicket";
          pt_meta = normalizeMetadata(std::move(pt_meta), printer_name_str);

          core::JobSnapshot job;
          {
            std::scoped_lock lock(pendingMutex);
            auto it = jobCache.find(pt_meta.job_id);
            if (it != jobCache.end())
            {
              job = it->second;
            }
          }
          if (job.job_id != 0)
          {
            (void)writer.enqueue(std::move(job), pt_meta);
          }

          QMetaObject::invokeMethod(this, [this, meta = std::move(pt_meta)]() mutable {
            emit metadataReceived(std::move(meta));
          }, Qt::QueuedConnection);
        },
        /* cleanup_after_load = */ true);
    };

    int tick_count = 0;
    while (running_ && watcher.running())
    {
      // ── Wait: event-driven (IPC) or legacy sleep ────────────────────
      if (pt_event.valid())
      {
        const DWORD wait_result = pt_event.wait(50);  // 50 ms tick
        if (wait_result == WAIT_OBJECT_0)
        {
          // Reset the manual-reset event BEFORE scanning so a
          // concurrent SetEvent from another filter invocation
          // during the scan is not lost.
          pt_event.reset();
          scan_pt_metadata_now();
          tick_count = 0;  // skip redundant 3 s scan next tick
        }
      }
      else
      {
        QThread::msleep(50);
      }

      ++tick_count;

      // Heartbeat — fires every ~1 s (20 ticks).
      if (tick_count % 20 == 0)
      {
        QMetaObject::invokeMethod(this, [this, heartbeatPrinter]() {
          emit watcherHeartbeat(heartbeatPrinter);
        }, Qt::QueuedConnection);
      }

      // ── Periodic fallback scan (~3 s) ─────────────────────────────
      // Even when IPC is active, scan periodically as a safety net:
      // - GDI-only drivers skip the XPS pipeline entirely
      // - Event may be missed across spooler restarts
      if (tick_count % 60 == 0)
      {
        scan_pt_metadata_now();
      }
    }

    watcher.stop();
    writer.stop();

    const auto summaries = watcher.lifecycle_summaries();
    if (!summaries.empty())
    {
      storage::write_lifecycle_summaries_atomic(
        summaries,
        storage::JobSnapshotWriterConfig{.output_directory = outputDir});
    }
  }
  catch (const std::exception& error)
  {
    emit watcherError(QString::fromStdString(error.what()));
  }
  catch (...)
  {
    emit watcherError(QStringLiteral("Unknown error watching %1")
                      .arg(QString::fromStdWString(printerName)));
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
    const auto delay = kBackoffSequence[backoff_index];
    state.current_backoff = delay;

    common::log_warn("bridge",
      printer_utf8 + ": watcher exited unexpectedly, restart " +
      std::to_string(state.restart_count) + "/" +
      std::to_string(kMaxRestarts) + " after " +
      std::to_string(delay.count()) + " s backoff");

    // ── Sleep with backoff ────────────────────────────────────────────
    // Unlock before sleeping so stopWatching() can inspect state.
    lock.~scoped_lock();

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
