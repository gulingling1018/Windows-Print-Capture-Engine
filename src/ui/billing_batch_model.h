/// @file billing_batch_model.h
/// QAbstractItemModel that exposes BillingService state to Qt views.
///
/// Two-level tree:
///   Level 0 — BillingBatch (group row)
///   Level 1 — PrintJobRecord (job row)
///
/// Batches are ordered for operator workflow:
///   1. Active batches (current first, then others by creation time)
///   2. Suspended batches (most recent suspend first)
///   3. Settled batches (most recent settle first)
///
/// The model is read-only from the view's perspective; all mutations
/// go through BillingService which emits signals that the model
/// listens to and refreshes accordingly.

#pragma once

#include "billing/billing_service.h"
#include "domain/billing_batch.h"

#include <QAbstractItemModel>
#include <QColor>
#include <QDateTime>
#include <QHash>
#include <QString>

#include <cstdint>
#include <vector>

namespace wpce::ui
{

class BillingBatchModel : public QAbstractItemModel
{
  Q_OBJECT

 public:
  enum Column
  {
    ColName = 0,        // display_name (batch) or document_name (job)
    ColState,           // batch: state badge; job: lifecycle phase
    ColProgress,        // batch: jobs/¥; job: pages progress
    ColPages,           // batch: total pages; job: total_pages
    ColDuplex,          // batch: duplex sheets count; job: simplex/duplex label
    ColPaper,           // batch: special items summary; job: paper size
    ColSubmitTime,      // submit time
    ColPrice,           // total price / per-job price
    ColumnCount
  };

  enum Role
  {
    BatchIdRole       = Qt::UserRole + 1,
    JobIdRole         = Qt::UserRole + 2,
    NodeTypeRole      = Qt::UserRole + 3,  // "batch" | "job"
    StateRole         = Qt::UserRole + 4,  // BatchState as int
    PhaseKeyRole      = Qt::UserRole + 5,
    ProgressPercentRole = Qt::UserRole + 6,
    SortRole          = Qt::UserRole + 7,
    RowColorRole      = Qt::UserRole + 8,
    RowTextColorRole  = Qt::UserRole + 9,
    PricePendingRole  = Qt::UserRole + 10,
    PendingReasonRole = Qt::UserRole + 11,
    DisplayNameRole   = Qt::UserRole + 12,
    AnchorFirstRole   = Qt::UserRole + 13,  // first doc name
    AnchorLastRole    = Qt::UserRole + 14,  // last doc name
    DocumentCountRole = Qt::UserRole + 15,
    SimplexPagesRole  = Qt::UserRole + 16,
    DuplexPagesRole   = Qt::UserRole + 17,
    ColorDowngradedRole = Qt::UserRole + 18,
  };

  explicit BillingBatchModel(BillingService* service, QObject* parent = nullptr);

  // QAbstractItemModel overrides
  QModelIndex index(int row, int column,
                    const QModelIndex& parent = QModelIndex()) const override;
  QModelIndex parent(const QModelIndex& index) const override;
  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  int columnCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation,
                      int role = Qt::DisplayRole) const override;
  Qt::ItemFlags flags(const QModelIndex& index) const override;

  // Accessors
  [[nodiscard]] int batchCount() const;
  [[nodiscard]] QModelIndex batchIndex(const QString& batchId) const;
  [[nodiscard]] QString batchIdAt(const QModelIndex& index) const;
  [[nodiscard]] std::uint32_t jobIdAt(const QModelIndex& index) const;
  [[nodiscard]] bool isBatchRow(const QModelIndex& index) const;
  [[nodiscard]] bool isJobRow(const QModelIndex& index) const;
  [[nodiscard]] domain::BatchState batchStateAt(const QModelIndex& index) const;
  [[nodiscard]] const domain::BillingBatch* batchAt(const QModelIndex& index) const;
  [[nodiscard]] const domain::PrintJobRecord* jobAt(const QModelIndex& index) const;

  /// Index of the current active batch in the model's row order.
  [[nodiscard]] int currentActiveRow() const;

  /// Index of the next batch to switch to (Suspended first, then Active).
  [[nodiscard]] int nextRelevantRow(int fromRow) const;
  [[nodiscard]] int prevRelevantRow(int fromRow) const;

 public slots:
  void refreshAll();
  void onBatchUpdated(const QString& batchId);
  void onBatchesChanged();

 private:
  /// Rebuild the internal ordered batch list from the service.
  void rebuildOrdering();

  /// Ordering comparator: Active (current first) < Suspended (recent first) < Settled (recent first).
  bool orderLess(const domain::BillingBatch& a, const domain::BillingBatch& b) const;

  BillingService* service_;
  /// Ordered batch ids (model row → batch_id).
  std::vector<std::string> orderedBatchIds_;
};

}  // namespace wpce::ui
