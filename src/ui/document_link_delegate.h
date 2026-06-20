#pragma once

#include "ui/job_table_model.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyledItemDelegate>

#include <algorithm>

namespace wpce::ui
{

/// Custom delegate for the job table — renders every column with
/// rich, visually distinct styling per data type.
class DocumentLinkDelegate final : public QStyledItemDelegate
{
  Q_OBJECT

 public:
  using QStyledItemDelegate::QStyledItemDelegate;

  void paint(QPainter* painter,
             const QStyleOptionViewItem& option,
             const QModelIndex& index) const override
  {
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    const QString text = opt.text;
    opt.text.clear();

    QStyle* style = opt.widget != nullptr ? opt.widget->style() : QApplication::style();
    const bool selected = option.state.testFlag(QStyle::State_Selected);
    const int col = index.column();

    // ── Draw background ──────────────────────────────────────────
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, opt.widget);
    textRect.adjust(6, 0, -6, 0);

    const QVariant alignVar = index.data(Qt::TextAlignmentRole);
    const int align = alignVar.isValid()
      ? alignVar.toInt()
      : static_cast<int>(Qt::AlignVCenter | Qt::AlignLeft);

    const QColor userColor = index.data(JobTableModel::RowColorRole).value<QColor>();
    const QColor userText  = index.data(JobTableModel::RowTextColorRole).value<QColor>();
    const QString userId   = index.data(JobTableModel::UserIdRole).toString();

    // ── Default pen (for generic text columns) ───────────────────
    const QColor normalText = selected
      ? opt.palette.color(QPalette::HighlightedText)
      : QColor(0x1e, 0x29, 0x3b);
    const QColor mutedText = selected
      ? opt.palette.color(QPalette::HighlightedText).lighter(130)
      : QColor(0x64, 0x74, 0x8b);

    painter->setPen(normalText);
    painter->setFont(opt.font);

    switch (col)
    {
      // ── ColJobId — left color stripe + bold number ─────────────
      case JobTableModel::ColJobId:
      {
        if (userColor.isValid())
        {
          QRect stripe = textRect.adjusted(-6, 4, -textRect.width() + 4, -4);
          painter->fillRect(stripe, userColor.darker(112));
        }
        QFont f = opt.font;
        f.setWeight(QFont::DemiBold);
        painter->setFont(f);
        painter->setPen(selected ? normalText : QColor(0x47, 0x55, 0x69));
        painter->drawText(textRect, Qt::AlignCenter, text);
        break;
      }

      // ── ColDocument — blue underlined link ─────────────────────
      case JobTableModel::ColDocument:
      {
        QFont f = opt.font;
        f.setUnderline(true);
        f.setWeight(QFont::DemiBold);
        painter->setFont(f);
        painter->setPen(selected ? QColor(0x0f, 0x17, 0x2a) : QColor(0x25, 0x63, 0xeb));
        painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, text);
        break;
      }

      // ── ColUser — coloured pill badge ──────────────────────────
      case JobTableModel::ColUser:
      {
        QRect pill = textRect;
        pill.adjust(0, 4, 0, -4);
        pill.setWidth(std::min(pill.width(), 140));

        painter->setPen(Qt::NoPen);
        painter->setBrush(selected
          ? userColor.darker(108)
          : QColor(
              (userColor.red()   * 5 + 255 * 1) / 6,
              (userColor.green() * 5 + 255 * 1) / 6,
              (userColor.blue()  * 5 + 255 * 1) / 6));
        painter->drawRoundedRect(pill, 8, 8);

        painter->setPen(userText.isValid() ? userText : QColor(0x1e, 0x29, 0x3b));
        QFont f = opt.font;
        f.setWeight(QFont::DemiBold);
        f.setPointSize(9);
        painter->setFont(f);
        painter->drawText(pill.adjusted(8, 0, -8, 0),
                          Qt::AlignVCenter | Qt::AlignLeft, text);
        break;
      }

      // ── ColPrinter — muted monospace printer name ──────────────
      case JobTableModel::ColPrinter:
      {
        painter->setPen(mutedText);
        QFont f = opt.font;
        f.setPointSize(9);
        painter->setFont(f);
        painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, text);
        break;
      }

      // ── ColPhase — coloured bold text ──────────────────────────
      case JobTableModel::ColPhase:
      {
        painter->setPen(index.data(Qt::ForegroundRole).value<QColor>());
        QFont f = opt.font;
        f.setWeight(QFont::DemiBold);
        f.setPointSize(9);
        painter->setFont(f);
        painter->drawText(textRect, align, text);
        break;
      }

      // ── ColProgress — numeric + mini-bar indicator ─────────────
      case JobTableModel::ColProgress:
      {
        const int pct = index.data(JobTableModel::ProgressPercentRole).toInt();
        painter->setPen(normalText);
        QFont f = opt.font;
        f.setPointSize(9);
        painter->setFont(f);
        painter->drawText(textRect, align, text);

        // Mini progress bar below text.
        if (pct >= 0 && textRect.height() > 26)
        {
          QRect bar = textRect;
          bar.setTop(bar.bottom() - 5);
          bar.setHeight(3);
          bar.adjust(4, 0, -4, 0);

          painter->setPen(Qt::NoPen);
          painter->setBrush(QColor(0xe2, 0xe8, 0xf0));
          painter->drawRoundedRect(bar, 2, 2);

          QRect fill = bar;
          fill.setWidth(bar.width() * pct / 100);
          if (pct >= 100) painter->setBrush(QColor(0x16, 0x65, 0x34));
          else if (pct >= 50) painter->setBrush(QColor(0x25, 0x63, 0xeb));
          else painter->setBrush(QColor(0xf5, 0x9e, 0x0b));
          painter->drawRoundedRect(fill, 2, 2);
        }
        break;
      }

      // ── ColTotalPages / ColCopies — centred number ─────────────
      case JobTableModel::ColTotalPages:
      case JobTableModel::ColCopies:
      {
        painter->setPen(normalText);
        QFont f = opt.font;
        f.setPointSize(9);
        painter->setFont(f);
        painter->drawText(textRect, Qt::AlignCenter, text);
        break;
      }

      // ── ColPrice — monospace right-aligned, ¥ prefix ───────────
      case JobTableModel::ColPrice:
      {
        QFont f;
        f.setFamily(QStringLiteral("Cascadia Mono, Consolas"));
        f.setPointSize(10);
        f.setWeight(QFont::DemiBold);
        painter->setFont(f);
        const double price = text.toDouble();
        painter->setPen(selected
          ? normalText
          : price > 0 ? QColor(0x06, 0x76, 0x47) : QColor(0x94, 0xa3, 0xb8));
        painter->drawText(textRect, align, text);
        break;
      }

      // ── ColSubmitTime — muted monospace timestamp ──────────────
      case JobTableModel::ColSubmitTime:
      {
        painter->setPen(mutedText);
        QFont f = opt.font;
        f.setFamily(QStringLiteral("Cascadia Mono, Consolas"));
        f.setPointSize(9);
        painter->setFont(f);
        painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, text);
        break;
      }

      // ── Fallback ───────────────────────────────────────────────
      default:
      {
        painter->drawText(textRect, align, text);
        break;
      }
    }

    painter->restore();
  }

  bool editorEvent(QEvent* event,
                   QAbstractItemModel*,
                   const QStyleOptionViewItem&,
                   const QModelIndex& index) override
  {
    if (event->type() != QEvent::MouseButtonRelease ||
        index.column() != JobTableModel::ColDocument)
    {
      return false;
    }

    auto* mouse = static_cast<QMouseEvent*>(event);
    if (mouse->button() != Qt::LeftButton)
    {
      return false;
    }

    emit documentClicked(index);
    return true;
  }

 signals:
  void documentClicked(const QModelIndex& proxyIndex);
};

}  // namespace wpce::ui
