#include "ui/filter_install_dialog.h"
#include "management/filter_install_service.h"
#include <QCoreApplication>
#include <QDir>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>

namespace wpce::ui
{
FilterInstallDialog::FilterInstallDialog(const QString& output, QWidget* parent)
    : QDialog(parent), service_(new management::FilterInstallService(this))
{
  setWindowTitle(QStringLiteral("XPS 过滤器设置")); resize(650,410);
  auto* layout=new QVBoxLayout(this);
  auto* info=new QLabel(QStringLiteral("检查不会修改系统。安装需要管理员权限，仅配置已发现的兼容管道。\n"
      "重启打印服务会中断正在进行的打印，请在合适时段执行；配置检查通过后仍需试打验证。"),this);
  info->setWordWrap(true);layout->addWidget(info);
  auto* details=new QTextEdit(this);details->setReadOnly(true);layout->addWidget(details);
  auto* row=new QHBoxLayout;layout->addLayout(row);
  auto* check=new QPushButton(QStringLiteral("检查"),this);
  auto* install=new QPushButton(QStringLiteral("安装（不重启）"),this);
  auto* restart=new QPushButton(QStringLiteral("安装并重启打印服务"),this);
  auto* close=new QPushButton(QStringLiteral("关闭"),this);
  for(auto* button:{check,install,restart,close})row->addWidget(button);
  const auto script=QDir(QCoreApplication::applicationDirPath()).filePath("scripts/install-xps-filter.ps1");
  const auto dll=QDir(QCoreApplication::applicationDirPath()).filePath("wpce_xps_filter.dll");
  const auto run=[this,details,check,install,restart,script,dll,output](management::FilterInstallService::Action action,bool reboot){
    if(service_->busy())return;
    if(action==management::FilterInstallService::Action::Install){
      const auto message=reboot ? QStringLiteral("将安装过滤器并重启打印服务，正在进行的打印可能中断。是否继续？") :
          QStringLiteral("将备份并修改兼容打印管道、注册过滤器。本次不重启打印服务，完成后仍需安排重启。是否继续？");
      if(QMessageBox::warning(this,QStringLiteral("确认安装操作"),message,QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel)!=QMessageBox::Yes)return;
    }
    if(service_->start(action,script,dll,output,reboot)){
      details->setPlainText(QStringLiteral("正在处理，请等待结果…"));check->setEnabled(false);install->setEnabled(false);restart->setEnabled(false);
    }
  };
  connect(service_,&management::FilterInstallService::completed,this,[details,check,install,restart](const auto& result){
    details->setPlainText(management::FilterInstallService::summary(result)+"\n\n"+result.message+
        "\n\n"+result.targets.join('\n')+"\n"+result.errors.join('\n'));
    check->setEnabled(true);install->setEnabled(true);restart->setEnabled(true);
  });
  connect(check,&QPushButton::clicked,this,[run]{run(management::FilterInstallService::Action::Probe,false);});
  connect(install,&QPushButton::clicked,this,[run]{run(management::FilterInstallService::Action::Install,false);});
  connect(restart,&QPushButton::clicked,this,[run]{run(management::FilterInstallService::Action::Install,true);});
  connect(close,&QPushButton::clicked,this,&FilterInstallDialog::reject);
  run(management::FilterInstallService::Action::Probe,false);
}
void FilterInstallDialog::reject()
{
  if(service_->installing()){
    QMessageBox::information(this,QStringLiteral("安装尚未完成"),QStringLiteral("正在安装或回滚，请等待明确结果后关闭。"));return;
  }
  QDialog::reject();
}
}
