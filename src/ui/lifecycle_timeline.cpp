#include "ui/lifecycle_timeline.h"

#include <QFont>
#include <QFrame>
#include <QVBoxLayout>

namespace wpce::ui
{

QString LifecycleTimeline::phaseIcon(core::JobLifecyclePhase phase) noexcept
{
  switch (phase)
  {
    case core::JobLifecyclePhase::Accepted:  return QStringLiteral("\xe2\x9e\xa4");   // ➤
    case core::JobLifecyclePhase::Spooling:  return QStringLiteral("\xe2\x8f\xb3");   // ⏳
    case core::JobLifecyclePhase::Printing:  return QStringLiteral("\xf0\x9f\x96\xa8"); // 🖨
    case core::JobLifecyclePhase::Completed: return QStringLiteral("\xe2\x9c\x93");   // ✓
    case core::JobLifecyclePhase::Failed:    return QStringLiteral("\xe2\x9c\x97");   // ✗
    case core::JobLifecyclePhase::Cancelled: return QStringLiteral("\xe2\x9c\x82");   // ✂
    case core::JobLifecyclePhase::Stalled:   return QStringLiteral("\xe2\x9a\xa0");   // ⚠
    default:                                 return QStringLiteral("?");
  }
}

LifecycleTimeline::LifecycleTimeline(QWidget* parent) : QWidget(parent)
{
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(8, 8, 8, 8);

  header_ = new QLabel(QStringLiteral("\xe7\x94\x9f\xe5\x91\xbd\xe5\x91\xa8\xe6\x9c\x9f")); // 生命周期
  QFont headerFont = header_->font();
  headerFont.setBold(true);
  headerFont.setPointSize(headerFont.pointSize() + 2);
  header_->setFont(headerFont);
  root->addWidget(header_);

  auto* rule = new QFrame();
  rule->setFrameShape(QFrame::HLine);
  rule->setFrameShadow(QFrame::Sunken);
  root->addWidget(rule);

  list_ = new QListWidget();
  list_->setAlternatingRowColors(true);
  QFont monoFont("Consolas");
  monoFont.setPointSize(10);
  list_->setFont(monoFont);
  root->addWidget(list_, 1);
}

void LifecycleTimeline::addEvent(const core::JobLifecycleEvent& event)
{
  // If switching to a new job, clear first.
  if (event.job_id != currentJobId_ && currentJobId_ != 0)
  {
    clear();
  }
  currentJobId_ = event.job_id;

  const QString icon = phaseIcon(event.phase);
  const QString phaseStr = QString::fromUtf8(core::to_string(event.phase).data());

  // Extract time portion from timestamp.
  QString timeStr = QString::fromStdString(event.timestamp_utc);
  if (timeStr.length() >= 19)
  {
    timeStr = timeStr.mid(11, 8);  // "HH:MM:SS"
  }

  QString text = QStringLiteral("%1  %2  %3")
    .arg(timeStr, icon, phaseStr);

  // Append progress info.
  if (event.total_pages > 0)
  {
    const int pct = (event.pages_printed * 100) / event.total_pages;
    text += QStringLiteral("  (%1%)").arg(pct);
  }

  auto* item = new QListWidgetItem(text);
  list_->addItem(item);
  list_->scrollToBottom();
}

void LifecycleTimeline::loadEvents(const std::vector<core::JobLifecycleEvent>& events)
{
  clear();
  for (const auto& event : events)
  {
    addEvent(event);
  }
}

void LifecycleTimeline::clear()
{
  currentJobId_ = 0;
  list_->clear();
}

}  // namespace wpce::ui
