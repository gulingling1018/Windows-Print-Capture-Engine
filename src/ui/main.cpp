/// @file main.cpp
/// Qt application entry point for the WPCE Print Job Monitor.

#include "ui/main_window.h"

#include <QApplication>
#include <QFile>
#include <QIcon>
#include <QMessageBox>

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

#ifdef _WIN32
  // Single-instance guard: only one WPCE Monitor may run at a time.
  HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Global\\WPCE_Monitor_SingleInstance");
  if (hMutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS)
  {
    // Another instance is already running — bring its window to the front.
    HWND hWnd = FindWindowW(nullptr, L"WPCE 打印捕获控制台");
    if (hWnd != nullptr)
    {
      if (IsIconic(hWnd)) ShowWindow(hWnd, SW_RESTORE);
      SetForegroundWindow(hWnd);
    }
    else
    {
      QMessageBox::information(nullptr, QStringLiteral("WPCE"),
        QStringLiteral("程序已在运行中，请勿重复启动。"));
    }
    if (hMutex != nullptr) CloseHandle(hMutex);
    return 0;
  }
#endif
  QApplication::setStyle(QStringLiteral("Fusion"));
  app.setApplicationName(QStringLiteral("WPCE Monitor"));
  app.setApplicationVersion(QStringLiteral("0.1.0"));
  app.setOrganizationName(QStringLiteral("WPCE"));

  // Application icon (embedded via .rc for Windows Explorer; also set for window chrome).
  app.setWindowIcon(QIcon(QStringLiteral(":/icons/app_icon")));

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
