#pragma once

#include "core/data_contracts.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>

namespace wpce::ui
{

class JobDetailPanel : public QWidget
{
  Q_OBJECT

 public:
  explicit JobDetailPanel(QWidget* parent = nullptr);

 public slots:
  void showJob(const core::JobSnapshot& job,
               const core::MetadataSnapshot& metadata);
  void clear();

 private:
  void rebuildJobInfo(const core::JobSnapshot& job);
  void rebuildMetadata(const core::MetadataSnapshot& metadata);
  void clearLayout(QLayout* layout);
  void addFieldRow(const QString& label, const core::FieldValue& value);

  QVBoxLayout* root_ = nullptr;
  QLabel* jobHeader_ = nullptr;
  QVBoxLayout* jobInfoLayout_ = nullptr;
  QLabel* metaHeader_ = nullptr;
  QVBoxLayout* metaLayout_ = nullptr;
};

}  // namespace wpce::ui
