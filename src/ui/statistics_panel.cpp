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
    value->setObjectName(QStringLiteral("MetricValue"));
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

  jobCountValue_         = new QLabel(QStringLiteral("0"));
  currentUserPriceValue_ = new QLabel(QStringLiteral("-"));
  userCountValue_        = new QLabel(QStringLiteral("0"));
  completionValue_       = new QLabel(QStringLiteral("0 / 0"));

  layout->addWidget(makeStatCard(QStringLiteral("今日作业"),
                                  QStringLiteral("📄"), jobCountValue_));
  layout->addWidget(makeStatCard(QStringLiteral("当前用户金额"),
                                  QStringLiteral("💰"), currentUserPriceValue_));
  layout->addWidget(makeStatCard(QStringLiteral("活跃用户"),
                                  QStringLiteral("👥"), userCountValue_));
  layout->addWidget(makeStatCard(QStringLiteral("完成/进行中"),
                                  QStringLiteral("✅"), completionValue_));
}

void StatisticsPanel::refresh(const UserTaskModel* taskModel)
{
  if (taskModel == nullptr)
  {
    jobCountValue_->setText(QStringLiteral("0"));
    userCountValue_->setText(QStringLiteral("0"));
    completionValue_->setText(QStringLiteral("0 / 0"));
    return;
  }

  jobCountValue_->setText(QString::number(taskModel->todayJobCount()));

  const int active  = taskModel->activeUserCount();
  const int completed = taskModel->completedTaskCount();
  const int total = active + completed;
  userCountValue_->setText(QString::number(total));
  completionValue_->setText(QStringLiteral("%1 / %2").arg(completed).arg(total));
}

void StatisticsPanel::setCurrentUserPrice(const QString& userName, double price)
{
  if (userName.isEmpty() || price <= 0.0)
  {
    currentUserPriceValue_->setText(QStringLiteral("-"));
    return;
  }
  currentUserPriceValue_->setText(
    QStringLiteral("%1  ¥%2").arg(userName).arg(price, 0, 'f', 2));
}

}  // namespace wpce::ui
