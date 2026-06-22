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
    // ═══════════════════════════════════════════════════════════════
    //  GLOBALS
    // ═══════════════════════════════════════════════════════════════
    "* { font-family: 'Segoe UI', 'Microsoft YaHei UI'; }"

    // ── Root ───────────────────────────────────────────────────────
    "QWidget#AppRoot { background: #eef1f6; color: #1e293b; font-size: 10pt; }"

    // ═══════════════════════════════════════════════════════════════
    //  HERO BAR — dark gradient, white text, clearly separated
    // ═══════════════════════════════════════════════════════════════
    "QFrame#HeroBar {"
    "  background: qlineargradient(x1:0,y1:0,x2:1,y2:0,"
    "    stop:0 #0f2b4a, stop:0.5 #194a7a, stop:1 #2563eb);"
    "  border: none; border-radius: 12px;"
    "  margin-bottom: 2px;"
    "}"
    "QLabel#WindowTitle { color: #ffffff; font-size: 22px; font-weight: 800; }"
    "QLabel#WindowSubtitle { color: rgba(255,255,255,0.70); font-size: 10pt; }"

    // ═══════════════════════════════════════════════════════════════
    //  METRIC CARDS — white with left-coloured accent
    // ═══════════════════════════════════════════════════════════════
    "QFrame#MetricCard {"
    "  background: #ffffff;"
    "  border: 1px solid #d4dae3; border-left: 4px solid #2563eb;"
    "  border-radius: 8px;"
    "}"
    "QFrame#MetricCard:hover { border-color: #2563eb; }"
    "QLabel#MetricValue { color: #0f172a; font-size: 22px; font-weight: 800; }"
    "QLabel#MetricLabel { color: #64748b; font-size: 9pt; font-weight: 600; }"

    // ═══════════════════════════════════════════════════════════════
    //  PANELS — white cards with distinct coloured borders
    // ═══════════════════════════════════════════════════════════════
    "QFrame#TablePanel {"
    "  background: #ffffff;"
    "  border: 1px solid #cdd4df; border-top: 3px solid #2563eb;"
    "  border-radius: 10px;"
    "}"
    "QWidget#DetailPanel, QWidget#TimelinePanel {"
    "  background: #ffffff;"
    "  border: 1px solid #cdd4df; border-radius: 10px;"
    "}"
    "QWidget#DetailPanel { border-left: 3px solid #7c3aed; }"
    "QWidget#TimelinePanel { border-left: 3px solid #0891b2; }"

    // Bottom panel
    "QFrame#BottomPanel { background: transparent; border: 0; }"
    "QFrame#StatisticsPanel {"
    "  background: #f0f4ff;"
    "  border: 1px solid #c7d2e8; border-left: 4px solid #6366f1;"
    "  border-radius: 10px;"
    "}"

    // Task panel — the container frame in UserTaskPanel
    "QFrame#Panel {"
    "  background: #ffffff;"
    "  border: 1px solid #cdd4df; border-top: 3px solid #f59e0b;"
    "  border-radius: 10px;"
    "}"

    // ═══════════════════════════════════════════════════════════════
    //  TYPOGRAPHY
    // ═══════════════════════════════════════════════════════════════
    "QLabel#PanelHeader { color: #0f172a; font-size: 13px; font-weight: 700; }"
    "QLabel#MutedText, QLabel#EmptyHint { color: #94a3b8; }"
    "QLabel#PathValue { color: #475569; font-size: 9pt; }"

    // ── Status pill ─────────────────────────────────────────────────
    "QLabel#StatusPill {"
    "  border-radius: 12px; padding: 4px 12px; font-weight: 700; font-size: 9pt;"
    "  background: #e2e8f0; color: #475569;"
    "}"
    "QLabel#StatusPill[state='running'] {"
    "  background: #dcfce7; color: #166534;"
    "}"

    // ── Empty state ─────────────────────────────────────────────────
    "QLabel#EmptyState {"
    "  color: #94a3b8; font-size: 14px; background: #f8fafc;"
    "  border: 1px dashed #cbd5e1; border-radius: 8px;"
    "}"

    "QLabel#StatIcon { font-size: 18px; }"

    // ═══════════════════════════════════════════════════════════════
    //  DIVIDERS
    // ═══════════════════════════════════════════════════════════════
    "QFrame#Divider {"
    "  color: #e2e8f0; background: #e2e8f0; max-height: 1px; border: 0;"
    "}"

    // ═══════════════════════════════════════════════════════════════
    //  FIELD ROW & BADGES
    // ═══════════════════════════════════════════════════════════════
    "QFrame#FieldRow {"
    "  background: #f8fafc; border: 1px solid #e8ecf1; border-radius: 6px;"
    "}"
    "QLabel#FieldName { color: #64748b; font-size: 9pt; }"
    "QLabel#MonoValue { color: #0f172a; }"
    "QScrollArea#DetailScroll { background: transparent; border: 0; }"
    "QWidget#DetailContent { background: transparent; }"
    "QLabel#DetailTitle { color: #0f172a; font-size: 15px; font-weight: 800; }"
    "QLabel#StateBadge { border-radius: 10px; padding: 3px 8px; font-size: 9pt; font-weight: 700; }"
    "QLabel#StateBadge[state='reported']    { background: #dcfce7; color: #166534; }"
    "QLabel#StateBadge[state='unsupported'] { background: #fef3c7; color: #92400e; }"
    "QLabel#StateBadge[state='not_reported']{ background: #e2e8f0; color: #475569; }"
    "QLabel#StateBadge[state='unknown']     { background: #dbeafe; color: #1e40af; }"
    "QLabel#StateBadge[state='error']       { background: #fee2e2; color: #991b1b; }"
    "QLabel#FieldGroupLabel { color: #0f172a; font-size: 11px; font-weight: 700; "
    "  text-transform: uppercase; letter-spacing: 0px; margin-top: 2px; }"
    "QLabel#SourceLabel { font-size: 9pt; font-weight: 600; border-radius: 4px; "
    "  padding: 2px 8px; }"
    "QLabel#SourceLabel[pt='false'] { background: #fef3c7; color: #92400e; }"
    "QLabel#SourceLabel[pt='true']  { background: #dcfce7; color: #166534; }"
    "QWidget#ParamGrid { background: transparent; }"
    "QFrame#ParamTile {"
    "  background: #f8fafc; border: 1px solid #e8ecf1;"
    "  border-left: 4px solid #94a3b8; border-radius: 8px;"
    "}"
    "QFrame#ParamTile[state='reported'] { border-left-color: #10b981; }"
    "QFrame#ParamTile[state='unsupported'] { border-left-color: #f59e0b; }"
    "QFrame#ParamTile[state='not_reported'] { border-left-color: #94a3b8; }"
    "QFrame#ParamTile[state='unknown'] { border-left-color: #3b82f6; }"
    "QFrame#ParamTile[state='error'] { border-left-color: #ef4444; }"
    "QLabel#ParamName { color: #64748b; font-size: 9pt; font-weight: 700; }"
    "QLabel#ParamValue { color: #0f172a; font-size: 10pt; }"

    // ── Group summary card (detail panel) ──────────────────────────────
    "QFrame#SummaryCard {"
    "  background: #f8fafc; border: 1px solid #e2e8f0; border-radius: 8px;"
    "}"

    // ═══════════════════════════════════════════════════════════════
    //  BUTTONS
    // ═══════════════════════════════════════════════════════════════
    "QPushButton {"
    "  min-height: 32px; padding: 4px 16px; border-radius: 6px;"
    "  border: 1px solid #cbd5e1; background: #ffffff; color: #334155; font-weight: 600;"
    "}"
    "QPushButton:hover { background: #f1f5f9; border-color: #94a3b8; }"
    "QPushButton:pressed { background: #e2e8f0; }"
    "QPushButton:disabled { background: #f1f5f9; color: #94a3b8; border-color: #e2e8f0; }"
    "QPushButton[buttonRole='primary'] {"
    "  background: #2563eb; color: #ffffff; border-color: #2563eb; font-weight: 700;"
    "}"
    "QPushButton[buttonRole='primary']:hover { background: #1d4ed8; }"
    "QPushButton[buttonRole='danger'] {"
    "  background: #ffffff; color: #dc2626; border-color: #fecaca; font-weight: 700;"
    "}"
    "QPushButton[buttonRole='danger']:hover { background: #fef2f2; }"
    "QPushButton[buttonRole='quiet'] { background: transparent; color: #64748b; border: 0; }"
    "QPushButton[buttonRole='quiet']:hover { background: #f1f5f9; }"

    // ═══════════════════════════════════════════════════════════════
    //  INPUTS
    // ═══════════════════════════════════════════════════════════════
    "QLineEdit, QComboBox {"
    "  min-height: 32px; padding: 0 10px; border: 1px solid #cbd5e1;"
    "  border-radius: 6px; background: #ffffff; color: #0f172a;"
    "}"
    "QLineEdit:focus, QComboBox:focus { border-color: #2563eb; }"
    "QComboBox::drop-down { border: 0; width: 24px; }"

    // ═══════════════════════════════════════════════════════════════
    //  TREE VIEW (user-grouped jobs) — branch icons hidden; hand-painted
    // ═══════════════════════════════════════════════════════════════
    "QTreeView#JobTree {"
    "  background: #ffffff; alternate-background-color: #f7f8fb;"
    "  border: none; border-radius: 6px; outline: none;"
    "  selection-background-color: transparent;"  // delegate paints selection
    "}"
    "QTreeView#JobTree::item {"
    "  padding: 0px; border: none;"
    "}"
    "QTreeView#JobTree::branch {"
    "  background: transparent;"         // no native +/- icons
    "}"
    "QTreeView#JobTree::branch:has-siblings:!adjoins-item {"
    "  border-image: none;"
    "}"
    "QTreeView#JobTree::branch:has-siblings:adjoins-item {"
    "  border-image: none;"
    "}"
    "QTreeView#JobTree::branch:!has-children:!has-siblings:adjoins-item {"
    "  border-image: none;"
    "}"
    "QTableView#TaskTable {"
    "  background: #ffffff; alternate-background-color: #f7f8fb;"
    "  border: none; border-radius: 6px; gridline-color: transparent;"
    "  selection-background-color: #dbeafe; selection-color: #0f172a;"
    "}"
    "QTableView#TaskTable::item {"
    "  padding: 0px 0px;"
    "  border-bottom: 1px solid #edf0f5;"
    "}"

    // ═══════════════════════════════════════════════════════════════
    //  HEADER VIEW — crisp, bordered
    // ═══════════════════════════════════════════════════════════════
    "QHeaderView::section {"
    "  background: #f1f5f9; color: #475569; padding: 7px 10px;"
    "  border: 0; border-bottom: 2px solid #cbd5e1;"
    "  border-right: 1px solid #e2e8f0;"
    "  font-weight: 700; font-size: 9pt;"
    "}"

    // ═══════════════════════════════════════════════════════════════
    //  TIMELINE
    // ═══════════════════════════════════════════════════════════════
    "QListWidget#TimelineList { background: #ffffff; outline: 0; border: none; }"
    "QListWidget#TimelineList::item {"
    "  padding: 8px 12px; border-bottom: 1px solid #f1f5f9; color: #334155;"
    "}"

    // ═══════════════════════════════════════════════════════════════
    //  SPLITTER — GripSplitter handles (10px wide with grip dots)
    // ═══════════════════════════════════════════════════════════════
    "QSplitter::handle {"
    "  background: #d4d9e2; border: none;"
    "}"
    "QSplitter::handle:horizontal {"
    "  background: qlineargradient(x1:0,y1:0,x2:0,y2:1,"
    "    stop:0 #ffffff, stop:0.3 #cbd1dc, stop:0.7 #cbd1dc, stop:1 #ffffff);"
    "}"
    "QSplitter::handle:vertical {"
    "  background: qlineargradient(x1:0,y1:0,x2:1,y2:0,"
    "    stop:0 #ffffff, stop:0.3 #cbd1dc, stop:0.7 #cbd1dc, stop:1 #ffffff);"
    "}"

    // ═══════════════════════════════════════════════════════════════
    //  STATUS BAR
    // ═══════════════════════════════════════════════════════════════
    "QStatusBar {"
    "  background: #ffffff; color: #64748b; border-top: 1px solid #e2e8f0;"
    "}"

    // ═══════════════════════════════════════════════════════════════
    //  SCROLLBARS
    // ═══════════════════════════════════════════════════════════════
    "QScrollBar:vertical { background: transparent; width: 8px; margin: 1px; }"
    "QScrollBar::handle:vertical { background: #cbd5e1; border-radius: 4px; min-height: 40px; }"
    "QScrollBar::handle:vertical:hover { background: #94a3b8; }"
    "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
    "QScrollBar:horizontal { background: transparent; height: 8px; margin: 1px; }"
    "QScrollBar::handle:horizontal { background: #cbd5e1; border-radius: 4px; min-width: 40px; }"
    "QScrollBar::handle:horizontal:hover { background: #94a3b8; }"
    "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }"
  ));

  wpce::ui::MainWindow window;
  window.show();

  return app.exec();
}
