/// @file user_group_delegate.h
/// Custom delegate for the user-group tree view.
///
/// Two paint modes:
///   Group row (depth 0) — summary stats (pages, duplex, colour, special items), status badge.
///   Job row (depth 1)  — per-job columns: document link, phase badge,
///     progress bar, page count, colour dot, duplex, paper, time.
///
/// Header-only; adapted from document_link_delegate.h.

#pragma once

#include "ui/job_table_model.h"
#include "ui/user_group_model.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTreeView>

#include <algorithm>

namespace wpce::ui
{

class UserGroupDelegate : public QStyledItemDelegate
{
  Q_OBJECT

 public:
  using QStyledItemDelegate::QStyledItemDelegate;

  void paint(QPainter* painter, const QStyleOptionViewItem& option,
             const QModelIndex& index) const override
  {
    const bool isGroup = index.data(UserGroupModel::GroupTypeRole).toString() == QStringLiteral("group");
    if (isGroup)
    {
      if (index.column() == UserGroupModel::ColName)
        paintGroupRow(painter, option, index);
      else
        paintEmptyGroupCell(painter, option, index);
    }
    else
      paintJobRow(painter, option, index);
  }

  QSize sizeHint(const QStyleOptionViewItem& option,
                 const QModelIndex& index) const override
  {
    const bool isGroup = index.data(UserGroupModel::GroupTypeRole).toString() == QStringLiteral("group");
    auto sz = QStyledItemDelegate::sizeHint(option, index);
    if (isGroup)
      sz.setHeight(64);   // group card with stats (no avatar)
    else
      sz.setHeight(34);   // standard job row
    return sz;
  }

 signals:
  void documentClicked(const QModelIndex& index);

 private:
  // ── Colour constants for selected state ───────────────────────────
  // Selected background is light grey (#f1f5f9).  All text must stay
  // dark enough to be readable on this background.
  static QColor selNormalText() { return QColor(0x0f, 0x17, 0x2a); }
  static QColor selMutedText()  { return QColor(0x33, 0x41, 0x55); }
  static QColor selDocBlue()    { return QColor(0x1e, 0x40, 0xaf); }
  static QColor selBg()         { return QColor(0xf1, 0xf5, 0xf9); }

  // ── Group row painting ────────────────────────────────────────────

  void paintEmptyGroupCell(QPainter* painter, const QStyleOptionViewItem& option,
                           const QModelIndex& index) const
  {
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    opt.text.clear();
    const bool selected = opt.state & QStyle::State_Selected;
    if (selected)
    {
      opt.palette.setColor(QPalette::Highlight, selBg());
      opt.palette.setColor(QPalette::HighlightedText, selNormalText());
    }
    QStyle* style = opt.widget ? opt.widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);
  }

  void paintGroupRow(QPainter* painter, const QStyleOptionViewItem& option,
                     const QModelIndex& index) const
  {
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    opt.text.clear();
    const bool selected = opt.state & QStyle::State_Selected;
    if (selected)
    {
      opt.palette.setColor(QPalette::Highlight, selBg());
      opt.palette.setColor(QPalette::HighlightedText, selNormalText());
    }
    QStyle* style = opt.widget ? opt.widget->style() : QApplication::style();

    // Background (uses modified palette when selected).
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const QRect r = opt.rect;
    const QString userId = index.data(UserGroupModel::UserIdRole).toString();
    const bool completed = index.data(UserGroupModel::GroupCompletedRole).toBool();

    // Read summary data from siblings.
    const QString jobCountStr  = index.siblingAtColumn(UserGroupModel::ColProgress).data(Qt::DisplayRole).toString();
    const QString totalPages   = index.siblingAtColumn(UserGroupModel::ColPages).data(Qt::DisplayRole).toString();
    const QString specItems    = index.siblingAtColumn(UserGroupModel::ColPaper).data(Qt::DisplayRole).toString();
    const QString colorPages   = index.siblingAtColumn(UserGroupModel::ColColor).data(Qt::DisplayRole).toString();
    const QString duplexSheets = index.siblingAtColumn(UserGroupModel::ColDuplex).data(Qt::DisplayRole).toString();

    // ── Content area ──────────────────────────────────────────────────
    const int contentX = r.left() + 10;
    const int contentW = r.right() - contentX - 10;
    const int line1Y = r.top() + 8;

    // ── User name ─────────────────────────────────────────────────────
    QFont nameFont;
    nameFont.setPointSize(11);
    nameFont.setWeight(QFont::DemiBold);
    painter->setFont(nameFont);
    painter->setPen(selected ? selNormalText() : QColor(0x0f, 0x17, 0x2a));
    QRect nameRect(contentX, line1Y, contentW - 80, 18);
    painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                      opt.fontMetrics.elidedText(userId, Qt::ElideRight, contentW - 80));

    // ── Status badge (top-right) ──────────────────────────────────────
    const QString statusText = completed ? QStringLiteral("已结算") : QStringLiteral("进行中");
    const QColor statusBg = completed
      ? QColor(0xdc, 0xfc, 0xe7) : QColor(0xdb, 0xea, 0xfe);
    const QColor statusFg = completed
      ? QColor(0x16, 0x65, 0x34) : QColor(0x1e, 0x40, 0xaf);
    QFont stFont;
    stFont.setPointSize(8);
    stFont.setWeight(QFont::DemiBold);
    painter->setFont(stFont);
    QFontMetrics stFm(stFont);
    const int stW = stFm.horizontalAdvance(statusText) + 14;
    const int stH = stFm.height() + 4;
    const int stX = r.right() - stW - 10;
    const int stY = r.top() + 8;
    QRect stRect(stX, stY, stW, stH);
    {
      QPainterPath stPath;
      stPath.addRoundedRect(stRect, 8, 8);
      painter->setPen(Qt::NoPen);
      painter->setBrush(statusBg);
      painter->drawPath(stPath);
      painter->setPen(statusFg);
      painter->drawText(stRect, Qt::AlignCenter, statusText);
    }

    // ── Summary stats (line 2) ───────────────────────────────────────
    const int line2Y = line1Y + 20;
    const int line2X = contentX;
    QFont sumFont;
    sumFont.setPointSize(9);
    painter->setFont(sumFont);
    QColor mutedText = selected ? selMutedText() : QColor(0x64, 0x74, 0x8b);

    QStringList stats;
    if (!jobCountStr.isEmpty())
      stats.append(jobCountStr);
    if (!totalPages.isEmpty() && totalPages != QStringLiteral("0"))
      stats.append(totalPages + QStringLiteral(" 页"));
    if (!duplexSheets.isEmpty() && duplexSheets != QStringLiteral("0"))
      stats.append(QStringLiteral("双面 ") + duplexSheets + QStringLiteral(" 张"));
    if (!colorPages.isEmpty())
      stats.append(colorPages);

    QString statText = stats.join(QStringLiteral("  |  "));
    if (!statText.isEmpty())
    {
      painter->setPen(mutedText);
      painter->drawText(line2X, line2Y, contentW, 16,
                        Qt::AlignLeft | Qt::AlignVCenter, statText);
    }

    // ── Special items line ────────────────────────────────────────────
    if (!specItems.isEmpty())
    {
      const int line3Y = line2Y + 18;
      QFont specFont;
      specFont.setPointSize(8);
      painter->setFont(specFont);
      painter->setPen(selected ? QColor(0x92, 0x40, 0x0e) : QColor(0xb4, 0x53, 0x09));
      painter->drawText(line2X, line3Y, contentW, 14,
                        Qt::AlignLeft | Qt::AlignVCenter,
                        specItems);
    }

    painter->restore();
  }

  // ── Job row painting ─────────────────────────────────────────────

  void paintJobRow(QPainter* painter, const QStyleOptionViewItem& option,
                   const QModelIndex& index) const
  {
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    opt.text.clear();
    const bool selected = opt.state & QStyle::State_Selected;
    if (selected)
    {
      opt.palette.setColor(QPalette::Highlight, selBg());
      opt.palette.setColor(QPalette::HighlightedText, selNormalText());
    }

    const int col = index.column();
    QStyle* style = opt.widget ? opt.widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, opt.widget);
    textRect.adjust(4, 0, -4, 0);

    const bool pricePending = index.data(UserGroupModel::PricePendingRole).toBool();
    const QColor normalText = selected ? selNormalText() : QColor(0x1e, 0x29, 0x3b);
    const QColor muted      = selected ? selMutedText()  : QColor(0x64, 0x74, 0x8b);
    const QString text = index.data(Qt::DisplayRole).toString();

    // ── Price-pending warning stripe ────────────────────────────────
    if (pricePending && col != UserGroupModel::ColName)
    {
      const int stripeW = 3;
      QRect stripe(textRect.left() - 2, opt.rect.top() + 4, stripeW,
                   opt.rect.height() - 8);
      painter->setPen(Qt::NoPen);
      painter->setBrush(QColor(0xf5, 0x9e, 0x0b)); // amber-500
      painter->drawRoundedRect(stripe, 1, 1);
      textRect.adjust(4, 0, 0, 0);
    }

    if (col == UserGroupModel::ColName)
    {
      // ── Document name ─────────────────────────────────────────────
      QFont f;
      f.setPointSize(9);
      f.setWeight(QFont::DemiBold);
      painter->setFont(f);
      painter->setPen(selected ? selDocBlue() : QColor(0x25, 0x63, 0xeb));

      int offsetX = 0;
      if (pricePending)
      {
        // Warning dot before document name.
        const int dotSize = 8;
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0xf5, 0x9e, 0x0b));
        painter->drawEllipse(QPointF(textRect.left() + dotSize / 2.0,
                                     textRect.center().y()),
                             dotSize / 2.0, dotSize / 2.0);
        offsetX = dotSize + 6;
      }

      QRect docRect(textRect.left() + offsetX, textRect.top(),
                    textRect.width() - offsetX, textRect.height());
      painter->drawText(docRect, Qt::AlignVCenter | Qt::AlignLeft,
                        opt.fontMetrics.elidedText(text, Qt::ElideRight, docRect.width()));
    }
    else if (col == UserGroupModel::ColPhase)
    {
      // ── Phase text with colour ────────────────────────────────────
      const QColor pc = index.data(Qt::ForegroundRole).value<QColor>();
      QFont f;
      f.setPointSize(8);
      f.setWeight(QFont::DemiBold);
      painter->setFont(f);
      // Phase colour is kept even when selected so the operator can still
      // quickly spot printing / failed / completed states.
      painter->setPen(pc);
      painter->drawText(textRect, Qt::AlignCenter, text);
    }
    else if (col == UserGroupModel::ColProgress)
    {
      // ── Mini progress bar ─────────────────────────────────────────
      QFont f;
      f.setPointSize(8);
      painter->setFont(f);
      painter->setPen(normalText);
      painter->drawText(textRect, Qt::AlignCenter, text);

      int pct = index.data(UserGroupModel::ProgressPercentRole).toInt();
      if (pct >= 0 && opt.rect.height() > 26)
      {
        const int barH = 3, padH = 6;
        const int barY = textRect.bottom() - barH - padH;
        const int barW = textRect.width() - 6;
        const int barX = textRect.left() + 3;

        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0xe2, 0xe8, 0xf0));
        painter->drawRoundedRect(QRect(barX, barY, barW, barH), 1, 1);

        QColor fillC = (pct >= 100) ? QColor(0x16, 0x65, 0x34)
                       : (pct >= 50) ? QColor(0x25, 0x63, 0xeb)
                       : QColor(0xf5, 0x9e, 0x0b);
        painter->setBrush(fillC);
        painter->drawRoundedRect(
          QRect(barX, barY, barW * std::clamp(pct, 0, 100) / 100, barH), 1, 1);
      }
    }
    else if (col == UserGroupModel::ColColor)
    {
      // ── Colour dot + label ────────────────────────────────────────
      const int dotSize = 8;
      const int textW = opt.fontMetrics.horizontalAdvance(text);
      const int totalW = dotSize + 4 + textW;
      const int startX = textRect.left() + (textRect.width() - totalW) / 2;

      QColor dotC = QColor(0x94, 0xa3, 0xb8);
      if (text.contains(QStringLiteral("彩"))) dotC = QColor(0xdc, 0x26, 0x26);
      else if (text.contains(QStringLiteral("黑"))) dotC = QColor(0x1e, 0x29, 0x3b);

      painter->setPen(Qt::NoPen);
      painter->setBrush(dotC);
      painter->drawEllipse(QPointF(startX + dotSize / 2., textRect.center().y()),
                           dotSize / 2., dotSize / 2.);

      QFont f;
      f.setPointSize(8);
      painter->setFont(f);
      painter->setPen(normalText);
      painter->drawText(QRect(startX + dotSize + 4, textRect.top(), textW, textRect.height()),
                        Qt::AlignVCenter, text);
    }
    else if (col == UserGroupModel::ColDuplex || col == UserGroupModel::ColPaper)
    {
      QFont f;
      f.setPointSize(8);
      painter->setFont(f);
      painter->setPen(muted);
      painter->drawText(textRect, Qt::AlignCenter, text);
    }
    else if (col == UserGroupModel::ColSubmitTime)
    {
      QFont f(QStringLiteral("Cascadia Mono"));
      f.setStyleHint(QFont::Monospace);
      f.setPointSize(8);
      painter->setFont(f);
      painter->setPen(muted);
      painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, text);
    }
    else if (col == UserGroupModel::ColPages)
    {
      QFont f;
      f.setPointSize(8);
      painter->setFont(f);
      painter->setPen(normalText);
      painter->drawText(textRect, Qt::AlignCenter, text);
    }
    else
    {
      painter->setPen(normalText);
      painter->drawText(textRect, Qt::AlignCenter, text);
    }

    painter->restore();
  }
};

}  // namespace wpce::ui
