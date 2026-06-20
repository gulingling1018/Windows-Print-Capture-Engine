#pragma once

#include <QFrame>
#include <QPushButton>
#include <QTableView>

namespace wpce::ui
{

class UserTaskModel;

/// Panel with user task list table and a "完成结算" complete-button.
class UserTaskPanel : public QFrame
{
  Q_OBJECT

 public:
  explicit UserTaskPanel(UserTaskModel* model, QWidget* parent = nullptr);

  QTableView* tableView() const { return tableView_; }

 signals:
  void taskCompleted();

 private slots:
  void onCompleteClicked();

 private:
  UserTaskModel* model_ = nullptr;
  QTableView* tableView_ = nullptr;
  QPushButton* completeBtn_ = nullptr;
};

}  // namespace wpce::ui
