#include "management/filter_install_service.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace wpce::management
{
FilterInstallService::FilterInstallService(QObject* parent) : QObject(parent)
{
  qRegisterMetaType<FilterInstallResult>();
#ifdef Q_OS_WIN
  process_.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
#endif
  probeTimeout_.setSingleShot(true);
  connect(&probeTimeout_, &QTimer::timeout, this, [this] {
    if (active_ && action_ == Action::Probe) { probeTimedOut_ = true; process_.kill(); }
  });
  connect(&process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
    finish(status == QProcess::NormalExit ? code : -1);
  });
  connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (active_ && error == QProcess::FailedToStart) finish(-1);
  });
}
FilterInstallService::~FilterInstallService()
{
  // Normal UI close is blocked while installing. Never kill an installer
  // between backup and rollback; a read-only probe can be cancelled safely.
  if (active_ && action_ == Action::Probe) process_.kill();
  if (process_.state() != QProcess::NotRunning) process_.waitForFinished(-1);
}
bool FilterInstallService::start(Action action, const QString& script, const QString& dll,
    const QString& output, bool restart_spooler)
{
  if (active_ || (action == Action::Probe && restart_spooler)) return false;
  active_ = true; action_ = action; probeTimedOut_ = false;
  QStringList args{QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
      QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"), QStringLiteral("-File"), script,
      QStringLiteral("-DllPath"), dll, QStringLiteral("-OutputDir"), output, QStringLiteral("-AsJson")};
  if (action == Action::Probe) args << QStringLiteral("-Probe");
  if (restart_spooler) args << QStringLiteral("-RestartSpooler");
  process_.setProgram(QDir(qEnvironmentVariable("SystemRoot", "C:/Windows"))
      .filePath(QStringLiteral("System32/WindowsPowerShell/v1.0/powershell.exe")));
  process_.setArguments(args);
  process_.setWorkingDirectory(QFileInfo(script).absolutePath());
  process_.setProcessChannelMode(QProcess::SeparateChannels);
  process_.start();
  if (action == Action::Probe) probeTimeout_.start(30000);
  return true;
}
FilterInstallResult FilterInstallService::parseResult(const QByteArray& bytes, int exit_code)
{
  FilterInstallResult result;
  if (bytes.size() > 2 * 1024 * 1024)
  { result.message = QStringLiteral("安装服务结果过大，未确认成功。"); return result; }
  QJsonParseError error{};
  const auto document = QJsonDocument::fromJson(bytes, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject())
  { result.message = QStringLiteral("安装服务未返回有效结果。"); return result; }
  const auto object = document.object();
  const auto status = object.value("status").toString();
  const QStringList known{QStringLiteral("configured"),QStringLiteral("restart_required"),QStringLiteral("not_installed"),
      QStringLiteral("unverified"),QStringLiteral("failed"),QStringLiteral("dry_run")};
  if (object.value("schema_version").toInt(-1) != 1 || !known.contains(status) ||
      !object.value("success").isBool() || !object.value("ready").isBool() ||
      !object.value("restart_required").isBool() || !object.value("message").isString() ||
      !object.value("targets").isArray() || !object.value("errors").isArray())
  { result.message = QStringLiteral("安装服务结果不完整，未确认成功。"); return result; }
  result.status = status; result.message = object.value("message").toString();
  result.success = object.value("success").toBool(); result.ready = object.value("ready").toBool();
  result.restart_required = object.value("restart_required").toBool();
  if ((result.success && exit_code != 0) || (!result.success && exit_code == 0) ||
      result.success != (status == "configured" || status == "restart_required" || status == "dry_run") ||
      result.ready != (status == "configured") || result.restart_required != (status == "restart_required") ||
      (result.ready && !result.success) || (result.restart_required && !result.success))
  { result = {}; result.message = QStringLiteral("安装退出码与结果不一致，未确认成功。"); return result; }
  for (const auto& target : object.value("targets").toArray())
    result.targets << (target.isString() ? target.toString() : target.toObject().value("path").toString());
  for (const auto& entry : object.value("errors").toArray()) result.errors << entry.toString();
  if (result.ready && (result.targets.isEmpty() || result.targets.contains(QString{}) || !result.errors.isEmpty()))
  { result = {}; result.message = QStringLiteral("安装结果没有有效目标，未确认成功。"); }
  return result;
}
void FilterInstallService::finish(int exit_code)
{
  if (!active_) return;
  probeTimeout_.stop();
  auto result = parseResult(process_.readAllStandardOutput(), exit_code);
  const auto diagnostic = QString::fromUtf8(process_.readAllStandardError()).left(4096);
  if (!result.success && !diagnostic.isEmpty()) result.errors << diagnostic;
  if (probeTimedOut_) { result = {}; result.message = QStringLiteral("只读检查超时，未确认安装状态。"); }
  active_ = false;
  emit completed(std::move(result));
}
QString FilterInstallService::summary(const FilterInstallResult& result)
{
  if (result.ready) return QStringLiteral("XPS 配置检查通过（%1 个管道）；请试打验证采集。").arg(result.targets.size());
  if (result.restart_required) return QStringLiteral("XPS 配置已保存，尚需在合适时段明确重启打印服务。");
  if (result.status == "not_installed") return QStringLiteral("XPS 过滤器尚未完成安装验证；可打开“XPS 设置”。");
  return QStringLiteral("XPS 状态未确认：%1").arg(result.message);
}
}
