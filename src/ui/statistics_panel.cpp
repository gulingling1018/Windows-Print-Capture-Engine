#include "ui/statistics_panel.h"
#include "ui/user_task_model.h"

#include <QHBoxLayout>
#include <QVBoxLayout>

namespace wpce::ui
{

namespace
{

QFrame* makeStatCard(const QString& caption, const QString& icon, QLabel* value)
{
  auto* card = new QFrame();
  card->setObjectName(QStringLiteral("MetricCard"));

  auto* layout = new QVBoxLayout(card);
  layout->setContentsMargins(14, 10, 14, 10);
  layout->setSpacing(4);

  auto* header = new QHBoxLayout();
  header->setSpacing(6);
  auto* iconLabel = new QLabel(icon);
  iconLabel->setObjectName(QStringLiteral("StatIcon"));
  auto* captionLabel = new QLabel(caption);
  captionLabel->setObjectName(QStringLiteral("MetricLabel"));
  header->addWidget(iconLabel);
  header->addWidget(captionLabel);
  header->addStretch();

  if (value->objectName().isEmpty())
  {
    value->setObjectName(QStringLiteral("MetricValue"));
  }
  value->setTextInteractionFlags(Qt::TextSelectableByMouse);

  layout->addLayout(header);
  layout->addWidget(value);
  return card;
}

}  // namespace

StatisticsPanel::StatisticsPanel(QWidget* parent) : QFrame(parent)
{
  setObjectName(QStringLiteral("StatisticsPanel"));

  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(12);

  jobCountValue_  = new QLabel(QStringLiteral("0"));
  revenueValue_   = new QLabel(QStringLiteral("¥ 0.00"));
  userCountValue_ = new QLabel(QStringLiteral("0"));
  completionValue_ = new QLabel(QStringLiteral("0 / 0"));

  layout->addWidget(makeStatCard(QStringLiteral("今日作业"),
                                  QStringLiteral("\xF0\x9F\x93\x84"), jobCountValue_));
  layout->addWidget(makeStatCard(QStringLiteral("今日营收"),
                                  QStringLiteral("\xF0\x9F\x92\xB0"), revenueValue_));
  layout->addWidget(makeStatCard(QStringLiteral("活跃用户"),
                                  QStringLiteral("\xF0\x9F\x91\xA5"), userCountValue_));
  layout->addWidget(makeStatCard(QStringLiteral("完成/进行中"),
                                  QStringLiteral("\xE2\x9C\x85"), completionValue_));
}

void StatisticsPanel::refresh(const UserTaskModel* taskModel)
{
  if (taskModel == nullptr)
  {
    jobCountValue_->setText(QStringLiteral("0"));
    revenueValue_->setText(QStringLiteral("¥ 0.00"));
    userCountValue_->setText(QStringLiteral("0"));
    completionValue_->setText(QStringLiteral("0 / 0"));
    return;
  }

  jobCountValue_->setText(QString::number(taskModel->todayJobCount()));
  revenueValue_->setText(QStringLiteral("¥ %1").arg(taskModel->todayRevenue(), 0, 'f', 2));

  const int active  = taskModel->activeUserCount();
  const int completed = taskModel->completedTaskCount();
  const int total = active + completed;
  userCountValue_->setText(QString::number(total));
  completionValue_->setText(QStringLiteral("%1 / %2").arg(completed).arg(total));
}

}  // namespace wpce::ui
