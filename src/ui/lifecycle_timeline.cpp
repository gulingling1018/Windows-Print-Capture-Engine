#include "ui/lifecycle_timeline.h"

#include <QAbstractItemView>
#include <QFont>
#include <QFrame>
#include <QSizePolicy>
#include <QVBoxLayout>

namespace wpce::ui
{

namespace
{

constexpr int kEmptyMarkerRole = Qt::UserRole + 101;

QString formatDuration(const std::int64_t milliseconds)
{
  if (milliseconds <= 0)
  {
    return QStringLiteral("0 ms");
  }

  if (milliseconds < 1000)
  {
    return QStringLiteral("%1 ms").arg(milliseconds);
  }

  const double seconds = static_cast<double>(milliseconds) / 1000.0;
  return QStringLiteral("%1 s").arg(seconds, 0, 'f', 1);
}

}  // namespace

QString LifecycleTimeline::phaseDisplayName(const core::JobLifecyclePhase phase) noexcept
{
  switch (phase)
  {
    case core::JobLifecyclePhase::Accepted:
      return QStringLiteral("已接收");
    case core::JobLifecyclePhase::Spooling:
      return QStringLiteral("假脱机");
    case core::JobLifecyclePhase::Printing:
      return QStringLiteral("打印中");
    case core::JobLifecyclePhase::Completed:
      return QStringLiteral("已完成");
    case core::JobLifecyclePhase::Failed:
      return QStringLiteral("失败");
    case core::JobLifecyclePhase::Cancelled:
      return QStringLiteral("已取消");
    case core::JobLifecyclePhase::Stalled:
      return QStringLiteral("停滞");
    case core::JobLifecyclePhase::Unknown:
    default:
      return QStringLiteral("未知");
  }
}

LifecycleTimeline::LifecycleTimeline(QWidget* parent) : QWidget(parent)
{
  setObjectName(QStringLiteral("TimelinePanel"));
  setMinimumWidth(0);
  setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(16, 16, 16, 16);
  root->setSpacing(12);

  header_ = new QLabel(QStringLiteral("生命周期"));
  header_->setObjectName(QStringLiteral("PanelHeader"));
  header_->setMinimumWidth(0);
  header_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  root->addWidget(header_);

  auto* divider = new QFrame();
  divider->setObjectName(QStringLiteral("Divider"));
  divider->setFrameShape(QFrame::HLine);
  root->addWidget(divider);

  list_ = new QListWidget();
  list_->setObjectName(QStringLiteral("TimelineList"));
  list_->setMinimumWidth(0);
  list_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
  list_->setFrameShape(QFrame::NoFrame);
  list_->setSelectionMode(QAbstractItemView::NoSelection);
  list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  QFont font(QStringLiteral("Cascadia Mono"));
  font.setStyleHint(QFont::Monospace);
  font.setPointSize(10);
  list_->setFont(font);
  root->addWidget(list_, 1);

  clear();
}

void LifecycleTimeline::ensureEventMode()
{
  if (list_->count() == 1 &&
      list_->item(0)->data(kEmptyMarkerRole).toBool())
  {
    list_->clear();
  }
}

void LifecycleTimeline::addEvent(const core::JobLifecycleEvent& event)
{
  if (event.job_id != currentJobId_ && currentJobId_ != 0)
  {
    clear();
  }
  currentJobId_ = event.job_id;
  ensureEventMode();

  QString time = QString::fromStdString(event.timestamp_utc);
  if (time.length() >= 19)
  {
    time = time.mid(11, 8);
  }

  QString progress = QStringLiteral("-");
  if (event.total_pages > 0)
  {
    const int pct = (event.pages_printed * 100) / event.total_pages;
    progress = QStringLiteral("%1 / %2  %3%")
                 .arg(event.pages_printed)
                 .arg(event.total_pages)
                 .arg(pct);
  }

  const QString text = QStringLiteral("%1  %2  %3  +%4")
                         .arg(time,
                              phaseDisplayName(event.phase),
                              progress,
                              formatDuration(event.delta_ms_since_prev));

  auto* item = new QListWidgetItem(text);
  item->setToolTip(QString::fromStdString(event.detail));
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

  auto* item = new QListWidgetItem(QStringLiteral("暂无生命周期事件"));
  item->setData(kEmptyMarkerRole, true);
  item->setFlags(Qt::NoItemFlags);
  list_->addItem(item);
}

}  // namespace wpce::ui
