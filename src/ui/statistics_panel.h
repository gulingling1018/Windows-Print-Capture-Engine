#pragma once

#include <QFrame>
#include <QLabel>

namespace wpce::ui
{

class UserTaskModel;

/// Dashboard panel showing print statistics (today's jobs, revenue, users, completion).
class StatisticsPanel : public QFrame
{
  Q_OBJECT

 public:
  explicit StatisticsPanel(QWidget* parent = nullptr);

 public slots:
  void refresh(const UserTaskModel* taskModel);

 private:
  QLabel* jobCountValue_ = nullptr;
  QLabel* revenueValue_ = nullptr;
  QLabel* userCountValue_ = nullptr;
  QLabel* completionValue_ = nullptr;
};

}  // namespace wpce::ui
