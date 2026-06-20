#include "ui/user_task_panel.h"
#include "ui/user_task_model.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QVBoxLayout>

namespace wpce::ui
{

UserTaskPanel::UserTaskPanel(UserTaskModel* model, QWidget* parent)
    : QFrame(parent), model_(model)
{
  setObjectName(QStringLiteral("Panel"));

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(16, 14, 16, 14);
  layout->setSpacing(10);

  // Header row.
  auto* headerRow = new QHBoxLayout();
  headerRow->setContentsMargins(0, 0, 0, 0);
  headerRow->setSpacing(10);

  auto* title = new QLabel(QStringLiteral("用户任务管理"));
  title->setObjectName(QStringLiteral("PanelHeader"));
  headerRow->addWidget(title);
  headerRow->addStretch(1);

  completeBtn_ = new QPushButton(QStringLiteral("完成结算"));
  completeBtn_->setProperty("buttonRole", "primary");
  completeBtn_->setCursor(Qt::PointingHandCursor);
  completeBtn_->setEnabled(false);
  headerRow->addWidget(completeBtn_);

  layout->addLayout(headerRow);

  // Separator.
  auto* sep = new QFrame();
  sep->setFrameShape(QFrame::HLine);
  sep->setObjectName(QStringLiteral("Divider"));
  layout->addWidget(sep);

  // Table.
  tableView_ = new QTableView();
  tableView_->setObjectName(QStringLiteral("TaskTable"));
  tableView_->setModel(model_);
  tableView_->setSelectionBehavior(QAbstractItemView::SelectRows);
  tableView_->setSelectionMode(QAbstractItemView::SingleSelection);
  tableView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  tableView_->setAlternatingRowColors(true);
  tableView_->setSortingEnabled(false);
  tableView_->setShowGrid(false);
  tableView_->verticalHeader()->hide();
  tableView_->verticalHeader()->setDefaultSectionSize(36);
  tableView_->horizontalHeader()->setHighlightSections(false);
  tableView_->horizontalHeader()->setStretchLastSection(true);
  tableView_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  layout->addWidget(tableView_, 1);

  // Wire selection → button enable.
  connect(tableView_->selectionModel(), &QItemSelectionModel::currentRowChanged,
          this, [this](const QModelIndex& current, const QModelIndex&)
          {
            bool canComplete = false;
            if (current.isValid())
            {
              const auto* task = model_->taskAtRow(current.row());
              canComplete = (task != nullptr && !task->completed);
            }
            completeBtn_->setEnabled(canComplete);
          });

  connect(completeBtn_, &QPushButton::clicked, this, &UserTaskPanel::onCompleteClicked);
}

void UserTaskPanel::onCompleteClicked()
{
  const QModelIndex current = tableView_->currentIndex();
  if (!current.isValid()) return;

  const auto* task = model_->taskAtRow(current.row());
  if (task == nullptr || task->completed) return;

  const QString userId = task->userId;
  model_->completeUserTask(userId);
  emit taskCompleted();
}

}  // namespace wpce::ui
