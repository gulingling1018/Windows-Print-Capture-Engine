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

    // ── Root background ────────────────────────────────────────────
    "QWidget#AppRoot { background: #f0f2f5; color: #1a1a2e; font-size: 10pt; }"

    // ═══════════════════════════════════════════════════════════════
    //  CONTAINER HIERARCHY — each zone visually distinct
    // ═══════════════════════════════════════════════════════════════

    // Hero / toolbar area — dark accent
    "QFrame#HeroBar {"
    "  background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 #1e3a5f, stop:1 #2563eb);"
    "  border: none; border-radius: 10px;"
    "}"

    // Table panel — crisp white
    "QFrame#TablePanel {"
    "  background: #ffffff; border: 1px solid #dee2e8; border-radius: 10px;"
    "}"

    // Metric cards — soft white with subtle shadow via border
    "QFrame#MetricCard {"
    "  background: #ffffff; border: 1px solid #e2e6ed; border-radius: 10px;"
    "}"
    "QFrame#MetricCard:hover { border-color: #2563eb; }"

    // Side panel containers
    "QWidget#DetailPanel, QWidget#TimelinePanel {"
    "  background: #ffffff; border: 1px solid #dee2e8; border-radius: 10px;"
    "}"

    // Bottom panel (stats + tasks)
    "QFrame#BottomPanel { background: transparent; border: 0; }"
    "QFrame#StatisticsPanel {"
    "  background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 #f8fafc, stop:1 #eef4ff);"
    "  border: 1px solid #d4dce8; border-radius: 10px;"
    "}"

    // ═══════════════════════════════════════════════════════════════
    //  TYPOGRAPHY HIERARCHY — 6 distinct levels
    // ═══════════════════════════════════════════════════════════════

    // L1 — Primary heading (white on hero)
    "QLabel#WindowTitle { color: #ffffff; font-size: 22px; font-weight: 800; }"
    // L1b — Hero subtitle
    "QLabel#WindowSubtitle { color: rgba(255,255,255,0.75); font-size: 10pt; }"

    // L2 — Panel headers (dark, authoritative)
    "QLabel#PanelHeader { color: #0f172a; font-size: 13px; font-weight: 700; }"

    // L3 — Body text (crisp dark gray)
    "* { color: #1e293b; }"

    // L4 — Metric values (large, impactful)
    "QLabel#MetricValue { color: #0f172a; font-size: 22px; font-weight: 800; }"
    "QLabel#MetricLabel { color: #64748b; font-size: 9pt; }"

    // L5 — Muted / secondary text
    "QLabel#MutedText, QLabel#EmptyHint { color: #94a3b8; }"

    // L6 — Path / file reference
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

    // ═══════════════════════════════════════════════════════════════
    //  STATISTICS ICONS
    // ═══════════════════════════════════════════════════════════════
    "QLabel#StatIcon { font-size: 18px; }"

    // ═══════════════════════════════════════════════════════════════
    //  DIVIDERS
    // ═══════════════════════════════════════════════════════════════
    "QFrame#Divider {"
    "  color: #e2e8f0; background: #e2e8f0; max-height: 1px; border: 0;"
    "}"

    // ═══════════════════════════════════════════════════════════════
    //  SECTION HEADER BARS (colored left accent)
    // ═══════════════════════════════════════════════════════════════
    "QLabel#SectionAccent {"
    "  background: #2563eb; border-radius: 2px; min-width: 4px; max-width: 4px;"
    "}"

    // ═══════════════════════════════════════════════════════════════
    //  FIELD ROW & BADGES
    // ═══════════════════════════════════════════════════════════════
    "QFrame#FieldRow {"
    "  background: #f8fafc; border: 1px solid #e8ecf1; border-radius: 6px;"
    "}"
    "QLabel#FieldName { color: #64748b; font-size: 9pt; }"
    "QLabel#MonoValue { color: #0f172a; }"
    "QLabel#StateBadge { border-radius: 10px; padding: 3px 8px; font-size: 9pt; font-weight: 700; }"
    "QLabel#StateBadge[state='reported']     { background: #dcfce7; color: #166534; }"
    "QLabel#StateBadge[state='unsupported']  { background: #fef3c7; color: #92400e; }"
    "QLabel#StateBadge[state='not_reported'] { background: #e2e8f0; color: #475569; }"
    "QLabel#StateBadge[state='unknown']      { background: #dbeafe; color: #1e40af; }"
    "QLabel#StateBadge[state='error']        { background: #fee2e2; color: #991b1b; }"

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

    // Primary — blue
    "QPushButton[buttonRole='primary'] {"
    "  background: #2563eb; color: #ffffff; border-color: #2563eb; font-weight: 700;"
    "}"
    "QPushButton[buttonRole='primary']:hover { background: #1d4ed8; border-color: #1d4ed8; }"
    "QPushButton[buttonRole='primary']:pressed { background: #1e40af; }"

    // Danger — red
    "QPushButton[buttonRole='danger'] {"
    "  background: #ffffff; color: #dc2626; border-color: #fecaca; font-weight: 700;"
    "}"
    "QPushButton[buttonRole='danger']:hover { background: #fef2f2; border-color: #fca5a5; }"

    // Quiet — ghost
    "QPushButton[buttonRole='quiet'] {"
    "  background: transparent; color: #64748b; border-color: transparent;"
    "}"
    "QPushButton[buttonRole='quiet']:hover { background: #f1f5f9; border-color: #cbd5e1; }"

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
    //  TABLES — Job table
    // ═══════════════════════════════════════════════════════════════
    "QTableView#JobTable {"
    "  background: #ffffff; alternate-background-color: #f8fafc;"
    "  border: none; border-radius: 6px; gridline-color: transparent;"
    "  selection-background-color: #dbeafe; selection-color: #0f172a;"
    "}"
    "QTableView#JobTable::item {"
    "  padding: 7px 10px; border-bottom: 1px solid #f1f5f9;"
    "}"
    "QTableView#JobTable::item:hover { background: #eef4ff; }"

    // ── Task table ──────────────────────────────────────────────────
    "QTableView#TaskTable {"
    "  background: #ffffff; alternate-background-color: #fafbfc;"
    "  border: none; border-radius: 6px; gridline-color: transparent;"
    "  selection-background-color: #dbeafe; selection-color: #0f172a;"
    "}"
    "QTableView#TaskTable::item {"
    "  padding: 7px 10px; border-bottom: 1px solid #f1f5f9;"
    "}"
    "QTableView#TaskTable::item:hover { background: #eef4ff; }"

    // ── User-color row backgrounds (applied via data roles) ─────────
    // These are programmatically set via BackgroundRole per row

    // ═══════════════════════════════════════════════════════════════
    //  HEADER VIEW
    // ═══════════════════════════════════════════════════════════════
    "QHeaderView::section {"
    "  background: #f1f5f9; color: #475569; padding: 8px 10px;"
    "  border: 0; border-bottom: 2px solid #e2e8f0; font-weight: 700; font-size: 9pt;"
    "}"

    // ═══════════════════════════════════════════════════════════════
    //  TIMELINE
    // ═══════════════════════════════════════════════════════════════
    "QListWidget#TimelineList { background: #ffffff; outline: 0; border: none; }"
    "QListWidget#TimelineList::item {"
    "  padding: 8px 12px; border-bottom: 1px solid #f1f5f9; color: #334155;"
    "}"

    // ═══════════════════════════════════════════════════════════════
    //  SPLITTER — wide grab zone, visible grip, cursor feedback
    // ═══════════════════════════════════════════════════════════════
    "QSplitter::handle {"
    "  background: #e2e8f0;"
    "}"
    // Horizontal (left-right drag) — 6px wide
    "QSplitter::handle:horizontal {"
    "  width: 6px; margin: 0;"
    "  image: url(none);"
    "}"
    "QSplitter::handle:horizontal:hover {"
    "  background: #2563eb; width: 6px;"
    "}"
    "QSplitter::handle:horizontal:pressed {"
    "  background: #1d4ed8; width: 6px;"
    "}"
    // Vertical (up-down drag) — 6px tall
    "QSplitter::handle:vertical {"
    "  height: 6px; margin: 0;"
    "}"
    "QSplitter::handle:vertical:hover {"
    "  background: #2563eb; height: 6px;"
    "}"
    "QSplitter::handle:vertical:pressed {"
    "  background: #1d4ed8; height: 6px;"
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
    "QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }"
    "QScrollBar::handle:vertical { background: #cbd5e1; border-radius: 5px; min-height: 40px; }"
    "QScrollBar::handle:vertical:hover { background: #94a3b8; }"
    "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
    "QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }"
    "QScrollBar::handle:horizontal { background: #cbd5e1; border-radius: 5px; min-width: 40px; }"
    "QScrollBar::handle:horizontal:hover { background: #94a3b8; }"
    "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }"

    // ═══════════════════════════════════════════════════════════════
    //  CLICKABLE LINK IN TABLE
    // ═══════════════════════════════════════════════════════════════
    "QLabel#DocLink { color: #2563eb; text-decoration: underline; }"
    "QLabel#DocLink:hover { color: #1d4ed8; font-weight: 700; }"
  ));

  wpce::ui::MainWindow window;
  window.show();

  return app.exec();
}
