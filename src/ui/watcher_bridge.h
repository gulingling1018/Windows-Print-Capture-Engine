#pragma once

#include "core/data_contracts.h"
#include "watcher/print_watcher.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QVector>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
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
  void printerSkipped(QString printerName);
  void sessionFinished();

 private:
  void runWatcher(const std::wstring& printerName,
                  const std::filesystem::path& outputDir);

  std::mutex threadsMutex_;
  std::vector<QThread*> watcherThreads_;
  std::atomic_bool running_{false};

  QStringList watchedPrinters_;
  mutable std::mutex printerListMutex_;
};

}  // namespace wpce::ui
