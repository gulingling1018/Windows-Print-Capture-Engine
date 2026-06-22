#pragma once

#include "core/data_contracts.h"
#include "watcher/print_watcher.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
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
  explicit WatcherBridge(QObject* parent = nullptr);
  ~WatcherBridge() override;

  /// Set the stall-detection timeout used by each per-printer watcher.
  /// 0 disables stall detection.  Must be called before startWatching().
  void setStallTimeout(std::chrono::seconds timeout) noexcept;

  /// Discover local printers and start a watcher for each one.
  void startWatching(const QString& outputDir);

  /// Stop all watchers and join their threads.
  void stopWatching();

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
  void runWatcher(const std::wstring& printerName,
                  const std::filesystem::path& outputDir);

  /// Wraps runWatcher() with exponential-backoff restart logic.
  /// Called from the per-printer QThread; loops until stopWatching()
  /// or permanent failure.
  void runWatcherWithRestart(const std::wstring& printerName,
                             const std::filesystem::path& outputDir);

  [[nodiscard]] std::uint32_t normalizedJobId(const std::string& printerName,
                                              std::uint32_t rawJobId);
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
  std::vector<QThread*> watcherThreads_;
  std::atomic_bool running_{false};
  std::chrono::seconds stallTimeout_{30};

  std::mutex restartStateMutex_;
  std::unordered_map<std::wstring, WatcherRestartState> restartStates_;

  mutable std::mutex jobIdMutex_;
  std::unordered_map<std::string, std::uint32_t> normalizedJobIds_;
  std::unordered_map<std::uint32_t, std::string> normalizedJobOwners_;
  std::unordered_map<std::uint32_t, std::string> rawJobPrinters_;
  std::uint32_t nextSyntheticJobId_{0x80000000u};

  mutable std::mutex metadataCacheMutex_;
  std::unordered_map<std::uint32_t, core::MetadataSnapshot> metadataCache_;

  QStringList watchedPrinters_;
  mutable std::mutex printerListMutex_;
};

}  // namespace wpce::ui
