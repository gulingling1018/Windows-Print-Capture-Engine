#pragma once

#include "core/data_contracts.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>

namespace wpce::ui
{

/// Right-side panel showing job identity and print metadata for the
/// currently selected job.
///
/// Two sections, independently populated:
///   1. Job info   — job_id, document, user, printer, time, status
///   2. Metadata   — total_pages, copies, duplex, color, paper, N-up, …
class JobDetailPanel : public QWidget
{
  Q_OBJECT

 public:
  explicit JobDetailPanel(QWidget* parent = nullptr);

 public slots:
  /// Update both sections from a full job+metadata pair.
  void showJob(const core::JobSnapshot& job,
               const core::MetadataSnapshot& metadata);

  /// Clear everything.
  void clear();

 private:
  void rebuildJobInfo(const core::JobSnapshot& job);
  void rebuildMetadata(const core::MetadataSnapshot& metadata);
  void clearLayout(QLayout* layout);
  void addFieldRow(const QString& label, const core::FieldValue& fv);

  QVBoxLayout* root_ = nullptr;

  // ── Job info section ─────────────────────────────────────────────────
  QLabel* jobHeader_ = nullptr;
  QVBoxLayout* jobInfoLayout_ = nullptr;

  // ── Metadata section ────────────────────────────────────────────────
  QLabel* metaHeader_ = nullptr;
  QVBoxLayout* metaLayout_ = nullptr;
};

}  // namespace wpce::ui
