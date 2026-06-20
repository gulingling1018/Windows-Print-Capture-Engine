#include "ui/watcher_bridge.h"

#include "storage/job_snapshot_writer.h"

#include <QMetaObject>

#include <mutex>
#include <string_view>

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

  EnumPrintersW(PRINTER_ENUM_LOCAL, nullptr, 2,
                nullptr, 0, &needed, &returned);

  if (needed == 0) return result;

  std::vector<BYTE> buffer(needed);
  if (!EnumPrintersW(PRINTER_ENUM_LOCAL, nullptr, 2,
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

}  // namespace

WatcherBridge::WatcherBridge(QObject* parent) : QObject(parent) {}

WatcherBridge::~WatcherBridge()
{
  stopWatching();
}

void WatcherBridge::startWatching(const QString& outputDir)
{
  if (running_) return;

  running_ = true;

  auto printers = listLocalPrinterNames();
  const std::filesystem::path dir(outputDir.toStdWString());

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
        runWatcher(printerName, dir);
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

void WatcherBridge::runWatcher(const std::wstring& printerName,
                               const std::filesystem::path& outputDir)
{
  try
  {
    // Short test: can we open this printer?
    {
      HANDLE hTest = nullptr;
      if (!OpenPrinterW(const_cast<LPWSTR>(printerName.c_str()),
                        &hTest, nullptr))
      {
        emit printerSkipped(QString::fromStdWString(printerName));
        return;
      }
      ClosePrinter(hTest);
    }

    storage::AsyncJobSnapshotWriter writer(
      storage::JobSnapshotWriterConfig{.output_directory = outputDir});
    writer.start();

    core::JobSnapshot pendingJob;
    std::mutex pendingMutex;

    auto jobCallback = [&](const core::JobSnapshot& snapshot)
    {
      emit jobSnapshotReceived(snapshot);
      std::scoped_lock lock(pendingMutex);
      pendingJob = snapshot;
    };

    auto metadataCallback = [&](const core::MetadataSnapshot& metadata)
    {
      emit metadataReceived(metadata);
      core::JobSnapshot job;
      {
        std::scoped_lock lock(pendingMutex);
        job = pendingJob;
      }
      (void)writer.enqueue(std::move(job), metadata);
    };

    auto lifecycleCallback = [&](const core::JobLifecycleEvent& event)
    {
      emit lifecycleEventReceived(event);
    };

    watcher::PrintWatcher watcher(
      watcher::WatcherConfig{
        .printer_name = printerName,
        .wait_timeout = std::chrono::milliseconds{250},
        .stall_timeout = std::chrono::seconds{30},
      },
      std::move(jobCallback),
      std::move(metadataCallback),
      std::move(lifecycleCallback));

    watcher.start();

    while (running_ && watcher.running())
    {
      QThread::msleep(200);
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

}  // namespace wpce::ui
