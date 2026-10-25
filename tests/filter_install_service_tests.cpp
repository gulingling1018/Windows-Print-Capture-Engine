#include "management/filter_install_service.h"
#include <QCoreApplication>
#include <QFile>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <gtest/gtest.h>

using wpce::management::FilterInstallService;
namespace
{
QByteArray result(const QString& status, bool success, bool ready, bool restart)
{
  return QJsonDocument(QJsonObject{{"schema_version",1},{"status",status},{"success",success},{"ready",ready},
      {"restart_required",restart},{"message","diagnostic"},{"targets",QJsonArray{"C:/test/pipeline.xml"}},
      {"errors",QJsonArray{}}}).toJson(QJsonDocument::Compact);
}
TEST(FilterInstallService, InvalidOrLegacyOutputCannotConfirmInstallation)
{
  EXPECT_FALSE(FilterInstallService::parseResult("installed",0).ready);
  EXPECT_FALSE(FilterInstallService::parseResult("{}",0).success);
  EXPECT_FALSE(FilterInstallService::parseResult(result("configured",true,true,false),7).success);
  EXPECT_FALSE(FilterInstallService::parseResult(result("failed",true,false,false),0).success);
}
TEST(FilterInstallService, RestartPendingIsDistinctFromConfigured)
{
  const auto pending=FilterInstallService::parseResult(result("restart_required",true,false,true),0);
  EXPECT_TRUE(pending.success); EXPECT_FALSE(pending.ready); EXPECT_TRUE(pending.restart_required);
  const auto verified=FilterInstallService::parseResult(result("configured",true,true,false),0);
  EXPECT_TRUE(verified.ready); EXPECT_FALSE(verified.restart_required);
  EXPECT_TRUE(FilterInstallService::summary(verified).contains(QStringLiteral("试打")));
}
TEST(FilterInstallService, ZeroTargetsOrContradictoryStateCannotConfirmInstallation)
{
  auto object=QJsonDocument::fromJson(result("configured",true,true,false)).object();
  object["targets"]=QJsonArray{};
  EXPECT_FALSE(FilterInstallService::parseResult(QJsonDocument(object).toJson(),0).ready);
  EXPECT_FALSE(FilterInstallService::parseResult(result("configured",true,true,true),0).ready);
  EXPECT_FALSE(FilterInstallService::parseResult(result("unverified",false,false,false),0).success);
}
class FilterInstallProcess : public testing::Test
{
 protected:
  int argc{1}; char name[16]{"install-test"}; char* argv[2]{name,nullptr};
  std::unique_ptr<QCoreApplication> app;
  QTemporaryDir directory;
  void SetUp() override {app=std::make_unique<QCoreApplication>(argc,argv); ASSERT_TRUE(directory.isValid());}
  void TearDown() override {app.reset();}
  QString stub(const QByteArray& body)
  {
    const auto path=directory.filePath(QStringLiteral("安装 服务.ps1"));
    QFile file(path); if(!file.open(QIODevice::WriteOnly)) return {};
    file.write("\xEF\xBB\xBF"); file.write(body); file.close(); return path;
  }
};
TEST_F(FilterInstallProcess, StartupProbeIsReadOnlyAndAsynchronous)
{
  const auto script=stub("param([switch]$Probe,[switch]$AsJson,[string]$DllPath,[string]$OutputDir)\n"
      "[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)\n"
      "if(-not $Probe){exit 9}\n"
      "@{schema_version=1;status='not_installed';success=$false;ready=$false;restart_required=$false;"
      "message='read only';targets=@();errors=@()} | ConvertTo-Json -Compress\nexit 1\n");
  FilterInstallService service;
  QEventLoop loop; bool completed=false;
  QObject::connect(&service,&FilterInstallService::completed,[&](const auto& value){
    completed=true; EXPECT_EQ(value.status,"not_installed"); EXPECT_FALSE(value.success); loop.quit();
  });
  ASSERT_TRUE(service.start(FilterInstallService::Action::Probe,script,"test.dll",directory.path()));
  EXPECT_TRUE(service.busy()); EXPECT_FALSE(completed);
  EXPECT_FALSE(service.start(FilterInstallService::Action::Install,script,"test.dll",directory.path()));
  QTimer::singleShot(6000,&loop,&QEventLoop::quit); loop.exec(); EXPECT_TRUE(completed); EXPECT_FALSE(service.busy());
}
TEST_F(FilterInstallProcess, ProbeCannotRequestRestartAndMissingScriptFails)
{
  FilterInstallService service;
  EXPECT_FALSE(service.start(FilterInstallService::Action::Probe,"absent.ps1","test.dll",directory.path(),true));
  QEventLoop loop; bool completed=false;
  QObject::connect(&service,&FilterInstallService::completed,[&](const auto& value){completed=true;EXPECT_FALSE(value.ready);loop.quit();});
  ASSERT_TRUE(service.start(FilterInstallService::Action::Probe,directory.filePath("absent.ps1"),"test.dll",directory.path()));
  QTimer::singleShot(6000,&loop,&QEventLoop::quit);loop.exec();EXPECT_TRUE(completed);
}
TEST_F(FilterInstallProcess, ExplicitInstallForwardsRestartWithoutExecutingSystemOperations)
{
  const auto script=stub("param([switch]$Probe,[switch]$AsJson,[switch]$RestartSpooler,[string]$DllPath,[string]$OutputDir)\n"
      "[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)\n"
      "if($Probe -or -not $RestartSpooler){exit 9}\n"
      "@{schema_version=1;status='configured';success=$true;ready=$true;restart_required=$false;"
      "message='stub only';targets=@('fake-pipeline.xml');errors=@()} | ConvertTo-Json -Compress\nexit 0\n");
  FilterInstallService service;QEventLoop loop;bool completed=false;
  QObject::connect(&service,&FilterInstallService::completed,[&](const auto& value){completed=true;EXPECT_TRUE(value.ready);loop.quit();});
  ASSERT_TRUE(service.start(FilterInstallService::Action::Install,script,"test.dll",directory.path(),true));
  EXPECT_TRUE(service.installing());
  QTimer::singleShot(6000,&loop,&QEventLoop::quit);loop.exec();EXPECT_TRUE(completed);EXPECT_FALSE(service.installing());
}
}
