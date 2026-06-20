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
  SetConsoleCP(CP_UTF8);
  SetConsoleOutputCP(CP_UTF8);
#endif

  QApplication app(argc, argv);
  QApplication::setStyle(QStringLiteral("Fusion"));
  app.setApplicationName(QStringLiteral("WPCE Monitor"));
  app.setApplicationVersion(QStringLiteral("0.1.0"));
  app.setOrganizationName(QStringLiteral("WPCE"));

  app.setStyleSheet(QStringLiteral(
    "QWidget#AppRoot { background: #f6f8fb; color: #172033; font-family: 'Segoe UI'; font-size: 10pt; }"
    "QFrame#HeroBar, QFrame#Panel, QFrame#MetricCard, QWidget#DetailPanel, QWidget#TimelinePanel {"
    "  background: #ffffff; border: 1px solid #d9e2ee; border-radius: 8px;"
    "}"
    "QLabel#WindowTitle { color: #101828; font-size: 22px; font-weight: 700; }"
    "QLabel#WindowSubtitle, QLabel#MutedText, QLabel#MetricLabel, QLabel#EmptyHint { color: #667085; }"
    "QLabel#PanelHeader { color: #101828; font-size: 13px; font-weight: 700; }"
    "QLabel#MetricValue { color: #101828; font-size: 22px; font-weight: 700; }"
    "QFrame#BottomPanel, QFrame#StatisticsPanel { background: transparent; border: 0; }"
    "QLabel#PathValue { color: #344054; font-size: 9pt; }"
    "QLabel#StatusPill {"
    "  border-radius: 12px; padding: 4px 10px; font-weight: 700;"
    "  background: #eef2f6; color: #475467;"
    "}"
    "QLabel#StatusPill[state='running'] { background: #dcfae6; color: #067647; }"
    "QLabel#EmptyState { color: #667085; font-size: 14px; background: #fbfcfe; border: 1px dashed #cbd5e1; border-radius: 8px; }"
    "QFrame#Divider { color: #eaecf0; background: #eaecf0; max-height: 1px; border: 0; }"
    "QFrame#FieldRow { background: #fbfcfe; border: 1px solid #edf1f6; border-radius: 6px; }"
    "QLabel#FieldName { color: #667085; font-size: 9pt; }"
    "QLabel#MonoValue { color: #101828; }"
    "QLabel#StateBadge { border-radius: 10px; padding: 3px 8px; font-size: 9pt; font-weight: 700; }"
    "QLabel#StateBadge[state='reported'] { background: #dcfae6; color: #067647; }"
    "QLabel#StateBadge[state='unsupported'] { background: #fff4e5; color: #b54708; }"
    "QLabel#StateBadge[state='not_reported'] { background: #eef2f6; color: #475467; }"
    "QLabel#StateBadge[state='unknown'] { background: #eef4ff; color: #3538cd; }"
    "QLabel#StateBadge[state='error'] { background: #fee4e2; color: #b42318; }"
    "QPushButton {"
    "  min-height: 32px; padding: 0 14px; border-radius: 6px;"
    "  border: 1px solid #cbd5e1; background: #ffffff; color: #344054; font-weight: 600;"
    "}"
    "QPushButton:hover { background: #f8fafc; border-color: #98a2b3; }"
    "QPushButton:pressed { background: #eef2f6; }"
    "QPushButton:disabled { background: #f2f4f7; color: #98a2b3; border-color: #e4e7ec; }"
    "QPushButton[buttonRole='primary'] { background: #2563eb; color: #ffffff; border-color: #2563eb; }"
    "QPushButton[buttonRole='primary']:hover { background: #1d4ed8; border-color: #1d4ed8; }"
    "QPushButton[buttonRole='danger'] { background: #ffffff; color: #b42318; border-color: #fecdca; }"
    "QPushButton[buttonRole='danger']:hover { background: #fff1f3; border-color: #fda29b; }"
    "QPushButton[buttonRole='quiet'] { background: #ffffff; color: #475467; }"
    "QLineEdit, QComboBox {"
    "  min-height: 32px; padding: 0 10px; border: 1px solid #cbd5e1;"
    "  border-radius: 6px; background: #ffffff; color: #101828;"
    "}"
    "QLineEdit:focus, QComboBox:focus { border-color: #2563eb; }"
    "QComboBox::drop-down { border: 0; width: 24px; }"
    "QTableView#JobTable, QTableView#TaskTable {"
    "  background: #ffffff; alternate-background-color: #f8fafc;"
    "  border: 1px solid #e4e7ec; border-radius: 8px; gridline-color: transparent;"
    "  selection-background-color: #dbeafe; selection-color: #101828;"
    "}"
    "QTableView#JobTable::item { padding: 6px 8px; border-bottom: 1px solid #edf1f6; }"
    "QTableView#JobTable::item:hover { background: #eef4ff; }"
    "QHeaderView::section {"
    "  background: #f8fafc; color: #475467; padding: 8px 10px;"
    "  border: 0; border-bottom: 1px solid #e4e7ec; font-weight: 700;"
    "}"
    "QListWidget#TimelineList { background: #ffffff; outline: 0; }"
    "QListWidget#TimelineList::item { padding: 8px 10px; border-bottom: 1px solid #edf1f6; color: #344054; }"
    "QSplitter::handle { background: #cbd5e1; }"
    "QSplitter::handle:horizontal { width: 3px; margin: 0 4px; }"
    "QSplitter::handle:horizontal:hover { background: #2563eb; width: 3px; }"
    "QSplitter::handle:vertical { height: 3px; margin: 4px 0; }"
    "QSplitter::handle:vertical:hover { background: #2563eb; height: 3px; }"
    "QStatusBar { background: #ffffff; color: #475467; border-top: 1px solid #e4e7ec; }"
    "QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }"
    "QScrollBar::handle:vertical { background: #cbd5e1; border-radius: 5px; min-height: 36px; }"
    "QScrollBar::handle:vertical:hover { background: #98a2b3; }"
    "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
    "QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }"
    "QScrollBar::handle:horizontal { background: #cbd5e1; border-radius: 5px; min-width: 36px; }"
    "QScrollBar::handle:horizontal:hover { background: #98a2b3; }"
    "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }"
  ));

  wpce::ui::MainWindow window;
  window.show();

  return app.exec();
}
