#pragma once

#include "core/data_contracts.h"

#include <QLabel>
#include <QListWidget>
#include <QWidget>

#include <cstdint>
#include <vector>

namespace wpce::ui
{

/// Timeline widget showing lifecycle events for a selected job.
class LifecycleTimeline : public QWidget
{
  Q_OBJECT

 public:
  explicit LifecycleTimeline(QWidget* parent = nullptr);

 public slots:
  /// Append a new lifecycle event.  Events are displayed in arrival order.
  void addEvent(const core::JobLifecycleEvent& event);

  /// Load the full event history for a job (from lifecycle summaries).
  void loadEvents(const std::vector<core::JobLifecycleEvent>& events);

  /// Clear the timeline.
  void clear();

 private:
  static QString phaseIcon(core::JobLifecyclePhase phase) noexcept;

  QLabel* header_ = nullptr;
  QListWidget* list_ = nullptr;
  std::uint32_t currentJobId_{0};
};

}  // namespace wpce::ui
