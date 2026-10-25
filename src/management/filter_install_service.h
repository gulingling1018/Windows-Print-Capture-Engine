#pragma once
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>

namespace wpce::management
{
struct FilterInstallResult
{
  bool success{false}, ready{false}, restart_required{false};
  QString status{QStringLiteral("failed")}, message;
  QStringList targets, errors;
};
class FilterInstallService : public QObject
{
  Q_OBJECT
 public:
  enum class Action { Probe, Install };
  explicit FilterInstallService(QObject* parent = nullptr);
  ~FilterInstallService() override;
  bool start(Action action, const QString& script, const QString& dll,
      const QString& output, bool restart_spooler = false);
  bool busy() const noexcept { return active_; }
  bool installing() const noexcept { return active_ && action_ == Action::Install; }
  static FilterInstallResult parseResult(const QByteArray& json, int exit_code);
  static QString summary(const FilterInstallResult& result);
 signals:
  void completed(wpce::management::FilterInstallResult result);
 private:
  void finish(int exit_code);
  QProcess process_;
  QTimer probeTimeout_;
  bool active_{false}, probeTimedOut_{false};
  Action action_{Action::Probe};
};
}
Q_DECLARE_METATYPE(wpce::management::FilterInstallResult)
