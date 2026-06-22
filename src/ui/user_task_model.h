#pragma once

#include "common/app_config.h"
#include "core/data_contracts.h"

#include <QAbstractTableModel>
#include <QDate>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QTimer>
#include <QVector>

#include <cstdint>

namespace wpce::ui
{

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
/// Columns: TaskId, User, Date, Job Count, Total Pages, Price (¥), Status
///
/// Persistence:
///   saveToJson(filePath) / loadFromJson(filePath) serialise the full
///   task list to/from JSON.  Auto-save fires 2 s after the last change.
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

  /// Persistence — JSON serialisation.
  [[nodiscard]] bool saveToJson(const QString& filePath) const;
  bool loadFromJson(const QString& filePath);
  [[nodiscard]] QJsonObject toJson() const;
  bool fromJson(const QJsonObject& json);

  /// Configure the pricing reference used for cost calculation.
  /// If never called, built-in defaults from common::PricingConfig are used.
  void setPricingConfig(const common::PricingConfig* pricing);

  /// Configure the auto-save path.  When non-empty, the model will
  /// schedule a deferred save after each change.
  void setAutoSavePath(const QString& filePath);
  [[nodiscard]] QString autoSavePath() const;

  /// Force an immediate save (called on shutdown).
  void flushSave();

 public slots:
  void onJobSnapshot(core::JobSnapshot snapshot);
  void onMetadata(core::MetadataSnapshot metadata);
  void onLifecycleEvent(core::JobLifecycleEvent event);
  void completeUserTask(const QString& userId);
  void clear();

 private:
  int rowForUser(const QString& userId) const;
  QString generateTaskId(const QDate& date);
  void scheduleAutoSave();

  QVector<UserTask> tasks_;
  QMap<QString, int> activeTaskIndex_;          // userId → row index (active tasks only)
  QMap<std::uint32_t, int> jobToTaskRow_;       // job_id → task row index
  int dailyTaskCounter_{0};
  QDate lastTaskDate_;

  const common::PricingConfig* pricing_{nullptr};
  common::PricingConfig defaultPricing_;

  QString autoSavePath_;
  QTimer autoSaveTimer_;
  bool dirty_{false};
};

}  // namespace wpce::ui
