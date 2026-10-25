#pragma once
#include <QDialog>
namespace wpce::management { class FilterInstallService; }
namespace wpce::ui
{
class FilterInstallDialog : public QDialog
{
 public:
  explicit FilterInstallDialog(const QString& output, QWidget* parent = nullptr);
 protected:
  void reject() override;
 private:
  management::FilterInstallService* service_;
};
}
