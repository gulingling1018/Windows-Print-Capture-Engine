#pragma once

#include "core/data_contracts.h"

#include <QAbstractTableModel>
#include <QDate>
#include <QMap>
#include <QString>
#include <QVector>

#include <cstdint>

namespace wpce::ui
{

struct PricingConfig;

/// Per-job cost data tracked within a user task.
struct JobCostEntry
{
  int pages{0};
  double price{0.0};
};

/// A user task binds multiple print jobs to one user on one date.
/// Each task gets a daily auto-increment ID: "YYYYMMDD-NNN".
struct UserTask
{
  QString taskId;           // "20260620-001", "20260620-002", ...
  QString userId;
  QDate date;
  QVector<std::uint32_t> jobIds;
  QMap<std::uint32_t, JobCostEntry> jobCosts;
  int totalPages{0};
  double totalPrice{0.0};
  bool completed{false};

  void recomputeTotals();
};

/// Table model for user-task management.
/// Columns: User, Date, Job Count, Total Pages, Price (¥), Status
class UserTaskModel : public QAbstractTableModel
{
  Q_OBJECT

 public:
  enum Column
  {
    ColTaskId = 0,
    ColUser,
    ColDate,
    ColJobCount,
    ColTotalPages,
    ColPrice,
    ColStatus,
    ColumnCount
  };

  explicit UserTaskModel(QObject* parent = nullptr);

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  int columnCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation,
                      int role = Qt::DisplayRole) const override;

  /// Access a task by row.
  [[nodiscard]] const UserTask* taskAtRow(int row) const;
  [[nodiscard]] const UserTask* taskForUser(const QString& userId) const;

  /// Statistics query.
  [[nodiscard]] int todayJobCount() const;
  [[nodiscard]] double todayRevenue() const;
  [[nodiscard]] int activeUserCount() const;
  [[nodiscard]] int completedTaskCount() const;

 public slots:
  void onJobSnapshot(core::JobSnapshot snapshot);
  void onMetadata(core::MetadataSnapshot metadata);
  void onLifecycleEvent(core::JobLifecycleEvent event);
  void completeUserTask(const QString& userId);
  void clear();

 private:
  int rowForUser(const QString& userId) const;

  QString generateTaskId(const QDate& date);

  QVector<UserTask> tasks_;
  QMap<QString, int> activeTaskIndex_;          // userId → row index (active tasks only)
  QMap<std::uint32_t, int> jobToTaskRow_;       // job_id → task row index
  int dailyTaskCounter_{0};
  QDate lastTaskDate_;
};

}  // namespace wpce::ui
