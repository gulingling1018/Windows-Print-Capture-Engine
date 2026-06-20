#pragma once

#include "core/data_contracts.h"

#include <QLabel>
#include <QListWidget>
#include <QWidget>

#include <cstdint>
#include <vector>

namespace wpce::ui
{

class LifecycleTimeline : public QWidget
{
  Q_OBJECT

 public:
  explicit LifecycleTimeline(QWidget* parent = nullptr);

 public slots:
  void addEvent(const core::JobLifecycleEvent& event);
  void loadEvents(const std::vector<core::JobLifecycleEvent>& events);
  void clear();

 private:
  static QString phaseDisplayName(core::JobLifecyclePhase phase) noexcept;
  void ensureEventMode();

  QLabel* header_ = nullptr;
  QListWidget* list_ = nullptr;
  std::uint32_t currentJobId_{0};
};

}  // namespace wpce::ui
