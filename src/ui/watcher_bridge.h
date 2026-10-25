#pragma once

#include "core/data_contracts.h"
#include "watcher/print_watcher.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include "storage/job_snapshot_writer.h"
#include <functional>
#include <QVector>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace wpce::ui
{

/// Thread-safe bridge that monitors **all** local printers simultaneously.
///
/// Spawns one QThread per local printer.  Each thread runs a PrintWatcher;
/// callbacks are forwarded as cross-thread Qt signals to the UI.
class WatcherBridge : public QObject
{
  Q_OBJECT

 public:
  explicit WatcherBridge(QObject* parent = nullptr, bool includeExternalInboxRoots = true);
  ~WatcherBridge() override;

  /// Set the stall-detection timeout used by each per-printer watcher.
  /// 0 disables stall detection.  Must be called before startWatching().
  void setStallTimeout(std::chrono::seconds timeout) noexcept;

  /// Set the periodic rescan interval (safety net for dropped notifications).
  /// 0 disables.  Must be called before startWatching().
  void setRescanInterval(std::chrono::seconds interval) noexcept;

  /// Set the WaitForSingleObject polling timeout (ms).
  void setPollTimeout(std::chrono::milliseconds timeout) noexcept;

  /// Ensure synthetic job IDs start above @p minId to avoid collisions
  /// with IDs persisted from a previous session.  Must be called before
  /// startWatching().
  void seedMinSyntheticJobId(std::uint32_t minId) noexcept;

  /// Discover local printers and start a watcher for each one.
  void startWatching(const QString& outputDir);

  /// Stop all watchers and join their threads.
  [[nodiscard]] bool stopWatching(unsigned long timeoutMs = 10000);
  [[nodiscard]] QStringList storageErrors() const;
  // Only acknowledge diagnostics after a separate recovery export succeeds.
  void acknowledgeRecoveryExport();
  void setPrintTicketAcknowledgement(std::function<bool(std::uint32_t)> callback);
  void processPrintTicketInbox();
  void seedStoredJobs(std::vector<core::JobSnapshot> jobs);

  /// Whether any watcher is currently running.
  [[nodiscard]] bool isRunning() const noexcept;

  /// List of printer names currently being watched.
  [[nodiscard]] QStringList watchedPrinters() const;

 signals:
  void jobSnapshotReceived(core::JobSnapshot snapshot);
  void metadataReceived(core::MetadataSnapshot metadata);
  void lifecycleEventReceived(core::JobLifecycleEvent event);
  void watcherError(QString error);
  void storageWarning(QString warning);
  void watcherHeartbeat(QString printerName);
  void watcherPermanentFailure(QString printerName, QString error);
  void printerSkipped(QString printerName);
  void sessionFinished();

 private:
  friend class WatcherBridgeInboxTestPeer;
  void runWatcher(const std::wstring& printerName,
                  const std::filesystem::path& outputDir);
  void recordStorageFailure(const QString& error);
  void startPrintTicketInbox(const std::filesystem::path& output);
  core::MetadataSnapshot mergeCapturedMetadata(core::MetadataSnapshot metadata);

  /// Wraps runWatcher() with exponential-backoff restart logic.
  /// Called from the per-printer QThread; loops until stopWatching()
  /// or permanent failure.
  void runWatcherWithRestart(const std::wstring& printerName,
                             const std::filesystem::path& outputDir);

  [[nodiscard]] std::uint32_t normalizedJobId(const std::string& printerName,
                                              std::uint32_t rawJobId);
  [[nodiscard]] std::uint32_t normalizedJobIdForSnapshot(
      const std::string& printerName,
      std::uint32_t rawJobId,
      const core::JobSnapshot& snapshot);
  [[nodiscard]] std::optional<std::string> uniquePrinterForRawJobId(std::uint32_t rawJobId) const;
  [[nodiscard]] core::JobSnapshot normalizeJobSnapshot(core::JobSnapshot snapshot,
                                                       const std::string& fallbackPrinter);
  [[nodiscard]] core::MetadataSnapshot normalizeMetadata(core::MetadataSnapshot metadata,
                                                         const std::string& fallbackPrinter);
  [[nodiscard]] core::JobLifecycleEvent normalizeLifecycleEvent(core::JobLifecycleEvent event,
                                                                const std::string& fallbackPrinter);

  struct WatcherRestartState
  {
    std::wstring printer_name;
    int restart_count = 0;
    std::chrono::steady_clock::time_point first_restart_time;
    std::chrono::seconds current_backoff{1};
    bool permanent_failure = false;
  };

  std::mutex threadsMutex_;
  void resetCapturedJobMappings();
  std::vector<QThread*> watcherThreads_;
  std::atomic_bool running_{false};
  std::chrono::seconds stallTimeout_{30};
  std::chrono::seconds rescanInterval_{5};
  std::chrono::milliseconds pollTimeout_{250};

  std::mutex restartStateMutex_;
  std::unordered_map<std::wstring, WatcherRestartState> restartStates_;

  mutable std::mutex jobIdMutex_;
  std::unordered_map<std::string, std::uint32_t> normalizedJobIds_;
  std::unordered_map<std::uint32_t, std::string> normalizedJobOwners_;
  std::unordered_map<std::uint32_t, std::string> normalizedJobIdentities_;
  std::unordered_map<std::uint32_t, core::JobSnapshot> capturedSnapshots_;
  std::vector<core::JobSnapshot> storedJobSeeds_;
  std::unordered_map<std::uint32_t, std::string> rawJobPrinters_;
  std::uint32_t syntheticJobIdFloor_{0x80000000u};
  std::uint32_t nextSyntheticJobId_{0x80000000u};
  bool forceSyntheticJobIds_{false};

  mutable std::mutex metadataCacheMutex_;
  std::unordered_map<std::uint32_t, core::MetadataSnapshot> metadataCache_;

  QStringList watchedPrinters_;
  mutable std::mutex printerListMutex_;
  mutable std::mutex storageErrorsMutex_;
  QStringList storageErrors_;
  std::filesystem::path inboxOutput_;
  std::unique_ptr<storage::AsyncJobSnapshotWriter> inboxWriter_;
  std::vector<std::unique_ptr<storage::ReliableFileWriter>> pageRecoveryWriters_;
  std::vector<std::filesystem::path> pageRecoveryRoots_;
  QString inboxHealth_;
  std::function<bool(std::uint32_t)> inboxAcknowledgement_;
  struct InboxDelivery
  {
    std::string digest;
    bool waiting{true}, accepted{false}, archived{false};
    core::JobSnapshot snapshot;
    core::MetadataSnapshot metadata;
    std::string capture_date;
  };
  std::mutex inboxDeliveryMutex_;
  std::unordered_map<std::string, InboxDelivery> inboxDeliveries_;
  bool includeExternalInboxRoots_{true};
};

}  // namespace wpce::ui
