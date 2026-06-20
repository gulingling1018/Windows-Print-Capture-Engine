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
#include <optional>
#include <vector>

namespace wpce::ui
{

struct JobRowData
{
  core::JobSnapshot job;
  core::MetadataSnapshot metadata;
  core::JobLifecyclePhase phase{core::JobLifecyclePhase::Unknown};
  int totalPages{0};
  int pagesPrinted{0};
  double price{0.0};
  std::vector<core::JobLifecycleEvent> lifecycleEvents;
};

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
    ColPrice,
    ColSubmitTime,
    ColumnCount
  };

  enum Role
  {
    PhaseKeyRole = Qt::UserRole + 1,
    ProgressPercentRole,
    SortRole
  };

  explicit JobTableModel(QObject* parent = nullptr);

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  int columnCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation,
                      int role = Qt::DisplayRole) const override;

  [[nodiscard]] std::optional<JobRowData> jobAtRow(int row) const;

 public slots:
  void clear();
  void onJobSnapshot(core::JobSnapshot snapshot);
  void onMetadata(core::MetadataSnapshot metadata);
  void onLifecycleEvent(core::JobLifecycleEvent event);

 private slots:
  void flushPendingUpdates();

 private:
  QVector<std::uint32_t> jobOrder_;
  QMap<std::uint32_t, JobRowData> jobs_;
  QSet<int> dirtyRows_;
  mutable std::mutex dataMutex_;
  QTimer* flushTimer_ = nullptr;
};

}  // namespace wpce::ui
