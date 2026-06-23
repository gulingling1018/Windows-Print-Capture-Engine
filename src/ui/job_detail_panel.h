/// @file job_detail_panel.h
/// Right-side detail panel showing either user-group summary or per-job detail.

#pragma once

#include "core/data_contracts.h"
#include "ui/user_group_model.h"

#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

// Forward declare
class QResizeEvent;

namespace wpce::ui
{

class JobDetailPanel : public QWidget
{
  Q_OBJECT

 public:
  explicit JobDetailPanel(QWidget* parent = nullptr);

  /// Show per-job detail (individual print job).
  void showJob(const core::JobSnapshot& job, const core::MetadataSnapshot& metadata,
               const core::JobPricingEntry& pricing = {});

  /// Show user-group summary (aggregated stats).
  void showGroupSummary(const QString& userId, const UserGroupSummary& summary,
                        int activeJobCount, bool completed);

  /// Clear all content.
  void clear();

 protected:
  void resizeEvent(QResizeEvent* event) override;

 private:
  void clearLayout(QLayout* layout);
  void resetScroll();
  void rebuildJobLayout();

  QScrollArea* scroll_ = nullptr;
  QWidget* content_ = nullptr;
  QVBoxLayout* root_ = nullptr;

  // Cached state for responsive rebuilds on resize.
  bool showingJob_ = false;
  core::JobSnapshot cachedJob_;
  core::MetadataSnapshot cachedMetadata_;
  core::JobPricingEntry cachedPricing_;
  int cachedKeyParamCols_ = 2;
};

}  // namespace wpce::ui
