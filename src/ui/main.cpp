/// @file main.cpp
/// Qt application entry point for the WPCE Print Job Monitor.

#include "ui/main_window.h"

#include <QApplication>
#include <QFile>

#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char* argv[])
{
#ifdef _WIN32
  SetConsoleCP(CP_UTF8);
  SetConsoleOutputCP(CP_UTF8);
#endif

  QApplication app(argc, argv);
  QApplication::setStyle(QStringLiteral("Fusion"));
  app.setApplicationName(QStringLiteral("WPCE Monitor"));
  app.setApplicationVersion(QStringLiteral("0.1.0"));
  app.setOrganizationName(QStringLiteral("WPCE"));

  // Load unified theme from Qt resource.
  QFile themeFile(QStringLiteral(":/styles/theme.qss"));
  if (themeFile.open(QIODevice::ReadOnly | QIODevice::Text))
  {
    app.setStyleSheet(QString::fromUtf8(themeFile.readAll()));
    themeFile.close();
  }

  wpce::ui::MainWindow window;
  window.show();

  return app.exec();
}
