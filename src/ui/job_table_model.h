#pragma once

#include "core/data_contracts.h"

#include <QAbstractTableModel>
#include <QMap>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVector>

#include <cstdint>
#include <mutex>

namespace wpce::ui
{

/// Per-job data aggregated from job, metadata, and lifecycle callbacks.
struct JobRowData
{
  core::JobSnapshot job;
  core::MetadataSnapshot metadata;
  core::JobLifecyclePhase phase{core::JobLifecyclePhase::Unknown};
  int totalPages{0};
  int pagesPrinted{0};
};

/// QAbstractTableModel that provides a real-time, sortable view of
/// all print jobs observed by the PrintWatcher.
///
/// Updates are batched on a 200 ms timer to keep the UI responsive
/// under high-frequency spooler notifications.
class JobTableModel : public QAbstractTableModel
{
  Q_OBJECT

 public:
  enum Column
  {
    ColJobId = 0,
    ColDocument,
    ColUser,
    ColPhase,
    ColProgress,
    ColTotalPages,
    ColCopies,
    ColSubmitTime,
    ColumnCount  // Must be last
  };

  explicit JobTableModel(QObject* parent = nullptr);

  // ── QAbstractTableModel interface ───────────────────────────────────

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  int columnCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation,
                      int role = Qt::DisplayRole) const override;

  // ── Data access ─────────────────────────────────────────────────────

  /// Return the full row data for the job at the given model row, or nullptr.
  const JobRowData* jobAtRow(int row) const;

 public slots:
  /// Called from WatcherBridge::jobSnapshotReceived (cross-thread queued).
  void onJobSnapshot(core::JobSnapshot snapshot);

  /// Called from WatcherBridge::metadataReceived (cross-thread queued).
  void onMetadata(core::MetadataSnapshot metadata);

  /// Called from WatcherBridge::lifecycleEventReceived (cross-thread queued).
  void onLifecycleEvent(core::JobLifecycleEvent event);

 private slots:
  void flushPendingUpdates();

 private:
  /// Ordered list of job IDs in display order.
  QVector<std::uint32_t> jobOrder_;

  /// Job data keyed by job_id.
  QMap<std::uint32_t, JobRowData> jobs_;

  /// Pending row updates that need to be flushed (by timer).
  QSet<int> dirtyRows_;

  /// Guards jobs_ / jobOrder_ / dirtyRows_ across threads.
  mutable std::mutex dataMutex_;

  /// Periodic flush timer (200 ms).
  QTimer* flushTimer_ = nullptr;
};

}  // namespace wpce::ui
