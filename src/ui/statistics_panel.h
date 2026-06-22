#pragma once

#include <QFrame>
#include <QLabel>
#include <QString>

namespace wpce::ui
{

class UserTaskModel;

/// Dashboard panel showing print statistics (today's jobs, current-user amount,
/// active users, completion ratio).
class StatisticsPanel : public QFrame
{
  Q_OBJECT

 public:
  explicit StatisticsPanel(QWidget* parent = nullptr);

  /// Refresh statistics from the UserTaskModel.
  void refresh(const UserTaskModel* taskModel);

  /// Display the total price for the currently-selected user.
  /// Pass 0 to clear.
  void setCurrentUserPrice(const QString& userName, double price);

 private:
  QLabel* jobCountValue_ = nullptr;
  QLabel* currentUserPriceValue_ = nullptr;
  QLabel* userCountValue_ = nullptr;
  QLabel* completionValue_ = nullptr;
};

}  // namespace wpce::ui
