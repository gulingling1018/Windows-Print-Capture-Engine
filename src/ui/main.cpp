/// @file main.cpp
/// Qt application entry point for the WPCE Print Job Monitor.

#include "ui/main_window.h"

#include <QApplication>

#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char* argv[])
{
#ifdef _WIN32
  // Configure UTF-8 console I/O (consistent with wpce CLI).
  SetConsoleCP(CP_UTF8);
  SetConsoleOutputCP(CP_UTF8);
#endif

  QApplication app(argc, argv);
  app.setApplicationName(QStringLiteral("WPCE Monitor"));
  app.setApplicationVersion(QStringLiteral("0.1.0"));
  app.setOrganizationName(QStringLiteral("WPCE"));

  // Apply a basic application-wide stylesheet.
  app.setStyleSheet(QStringLiteral(
    "QTableView { gridline-color: #ddd; }"
    "QTableView::item:selected { background-color: #0078d4; color: white; }"
    "QHeaderView::section { background-color: #f0f0f0; padding: 4px; border: 1px solid #ddd; }"
    "QSplitter::handle { background-color: #ccc; }"
    "QComboBox { padding: 2px 6px; }"
    "QToolBar { border-bottom: 1px solid #ccc; padding: 4px; }"
  ));

  wpce::ui::MainWindow window;
  window.show();

  return app.exec();
}
