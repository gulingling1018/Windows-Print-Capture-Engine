/// @file user_group_model.h
/// User-centric grouped tree model for print-job monitoring.
///
/// Replaces JobTableModel's flat job list with a two-level tree:
///   Level 0 — User groups (collapsible, auto-aggregated summary)
///   Level 1 — Individual print jobs
///
/// Each group row aggregates simplex/duplex page counts and flags
/// special items (colour pages, non-A4/B5 paper) for print-shop
/// operator workflow.

#pragma once

#include "core/data_contracts.h"
#include "ui/job_table_model.h"       // for JobRowData

#include <QAbstractItemModel>
#include <QDateTime>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace wpce::ui
{

// ── Group-level aggregation ──────────────────────────────────────────

struct UserGroupSummary
{
  int simplexPages = 0;       // A4/B5 simplex pages
  int duplexPages = 0;        // A4/B5 duplex pages (physical pages)
  int duplexSheets = 0;       // duplex sheets = ceil(duplexPages/2)
  int colorPages = 0;         // colour-flagged pages on default paper
  int specialPaperPages = 0;  // non-A4/B5 pages total
  int jobCount = 0;           // number of child jobs
  QStringList specialItems;   // human-readable list, e.g. "彩色 3页", "A3 2页"
  QDateTime earliestJobTime;  // for group sort ordering
};

// ── Tree node (internal) ─────────────────────────────────────────────

struct GroupNode
{
  enum Type { Group, Job };

  Type type;
  GroupNode* parent = nullptr;
  QVector<GroupNode*> children;

  // ── Group fields (type == Group) ────────────────────────────────────
  QString userId;
  UserGroupSummary summary;
  bool completed = false;

  // ── Job fields (type == Job) ────────────────────────────────────────
  std::uint32_t jobId = 0;
  JobRowData jobData;
};

// ── Tree model ───────────────────────────────────────────────────────

class UserGroupModel : public QAbstractItemModel
{
  Q_OBJECT

 public:
  /// Columns — simplified: no JobId, Price, or Copies columns.
  enum Column
  {
    ColName = 0,       // User name (group) or Document name (job)
    ColPhase,          // Phase text
    ColProgress,       // Progress percentage + bar
    ColPages,          // Page count
    ColColor,          // Colour mode (dot + label)
    ColDuplex,         // Duplex mode
    ColPaper,          // Paper size
    ColSubmitTime,     // Submission time
    ColumnCount
  };

  /// Custom roles — supplementing Qt::DisplayRole etc.
  enum Role
  {
    GroupTypeRole       = Qt::UserRole + 1,  // "group" | "job"
    PhaseKeyRole        = Qt::UserRole + 2,  // string phase key
    ProgressPercentRole = Qt::UserRole + 3,  // int 0-100 or -1
    SortRole            = Qt::UserRole + 4,  // sortable value
    UserIdRole          = Qt::UserRole + 5,  // QString user identity
    RowColorRole        = Qt::UserRole + 6,  // QColor background tint
    RowTextColorRole    = Qt::UserRole + 7,  // QColor foreground
    GroupCompletedRole  = Qt::UserRole + 8,  // bool
    FreshnessSecsRole   = Qt::UserRole + 9,  // int seconds since update
  };

  explicit UserGroupModel(QObject* parent = nullptr);

  // ── QAbstractItemModel overrides ────────────────────────────────────
  QModelIndex index(int row, int column,
                    const QModelIndex& parent = QModelIndex()) const override;
  QModelIndex parent(const QModelIndex& index) const override;
  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  int columnCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation,
                      int role = Qt::DisplayRole) const override;
  Qt::ItemFlags flags(const QModelIndex& index) const override;

  // ── Accessors ───────────────────────────────────────────────────────
  [[nodiscard]] int groupCount() const;
  [[nodiscard]] int totalJobCount() const;
  [[nodiscard]] bool isGroupRow(const QModelIndex& index) const;
  [[nodiscard]] bool isJobRow(const QModelIndex& index) const;
  [[nodiscard]] QString userIdAt(const QModelIndex& index) const;
  [[nodiscard]] bool isGroupCompleted(const QModelIndex& index) const;
  [[nodiscard]] UserGroupSummary groupSummaryAt(const QModelIndex& index) const;
  [[nodiscard]] std::optional<JobRowData> jobDataAt(const QModelIndex& index) const;
  [[nodiscard]] QModelIndex groupIndexForUser(const QString& userId) const;

 public slots:
  // ── Data ingestion (same signals as old JobTableModel) ──────────────
  void onJobSnapshot(core::JobSnapshot snapshot);
  void onMetadata(core::MetadataSnapshot metadata);
  void onLifecycleEvent(core::JobLifecycleEvent event);

  // ── Operator actions ────────────────────────────────────────────────
  void completeUser(const QString& userId);
  void clear();

 signals:
  void userCompleted(const QString& userId);
  void groupCountChanged(int total, int active);

 private slots:
  void flushPendingUpdates();

 private:
  // ── Internal helpers ────────────────────────────────────────────────
  GroupNode* findGroup(const QString& userId);
  GroupNode* findJob(std::uint32_t jobId);
  void recomputeGroupSummary(GroupNode* group);
  GroupNode* insertGroupNode(const QString& userId, int row);
  void insertJobNode(GroupNode* group, int row, core::JobSnapshot snapshot);
  void markDirty(GroupNode* node);

  // ── Data ────────────────────────────────────────────────────────────
  QVector<GroupNode*> groups_;              // top-level group nodes (ordered)
  QMap<QString, GroupNode*> groupMap_;      // userId -> group node
  QMap<std::uint32_t, GroupNode*> jobMap_;  // jobId -> job node
  QSet<GroupNode*> dirtyNodes_;             // pending flush
  QTimer* flushTimer_ = nullptr;
  mutable std::mutex dataMutex_;
};

}  // namespace wpce::ui
