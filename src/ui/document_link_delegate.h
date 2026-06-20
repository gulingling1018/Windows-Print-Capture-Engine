#pragma once

#include "ui/job_table_model.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QStyledItemDelegate>

#include <algorithm>

namespace wpce::ui
{

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
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const bool selected = option.state.testFlag(QStyle::State_Selected);
    const QColor userColor =
      index.data(JobTableModel::RowColorRole).value<QColor>();
    const QColor userText =
      index.data(JobTableModel::RowTextColorRole).value<QColor>();

    if (index.column() == JobTableModel::ColJobId && userColor.isValid())
    {
      QRect stripe = option.rect.adjusted(0, 5, 0, -5);
      stripe.setWidth(5);
      painter->fillRect(stripe, userColor.darker(112));
    }

    QRect textRect = style->subElementRect(
      QStyle::SE_ItemViewItemText, &opt, opt.widget);
    textRect.adjust(9, 0, -9, 0);

    if (index.column() == JobTableModel::ColUser && userColor.isValid())
    {
      QRect pill = textRect;
      pill.adjust(0, 6, 0, -6);
      pill.setWidth(std::min(pill.width(), 150));

      painter->setPen(Qt::NoPen);
      painter->setBrush(selected ? userColor.darker(108) : userColor);
      painter->drawRoundedRect(pill, 10, 10);

      painter->setPen(userText.isValid() ? userText : QColor(0x1e, 0x29, 0x3b));
      QFont font = opt.font;
      font.setWeight(QFont::DemiBold);
      painter->setFont(font);
      painter->drawText(pill.adjusted(10, 0, -10, 0),
                        Qt::AlignVCenter | Qt::AlignLeft,
                        text);
      painter->restore();
      return;
    }

    if (index.column() == JobTableModel::ColDocument)
    {
      QFont font = opt.font;
      font.setUnderline(true);
      font.setWeight(QFont::DemiBold);
      painter->setFont(font);
      painter->setPen(selected ? QColor(0x0f, 0x17, 0x2a) : QColor(0x25, 0x63, 0xeb));
      painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, text);
      painter->restore();
      return;
    }

    QVariant alignment = index.data(Qt::TextAlignmentRole);
    int flags = alignment.isValid()
      ? alignment.toInt()
      : static_cast<int>(Qt::AlignVCenter | Qt::AlignLeft);

    if (!selected && index.column() == JobTableModel::ColUser && userText.isValid())
    {
      painter->setPen(userText);
    }
    else
    {
      painter->setPen(opt.palette.color(selected
        ? QPalette::HighlightedText
        : QPalette::Text));
    }
    painter->setFont(opt.font);
    painter->drawText(textRect, flags, text);
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
