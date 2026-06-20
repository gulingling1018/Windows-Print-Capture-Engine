#pragma once

#include "ui/job_table_model.h"

#include <QMouseEvent>
#include <QPainter>
#include <QStyledItemDelegate>

namespace wpce::ui
{

/// Renders the ColDocument column as a blue underlined link.
/// Clicking opens the captured document file.
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

    opt.widget->style()->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

    if (index.column() != JobTableModel::ColDocument)
    {
      painter->save();
      painter->setFont(opt.font);
      QRect textRect = opt.widget->style()->subElementRect(
        QStyle::SE_ItemViewItemText, &opt, opt.widget);
      painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, opt.text);
      painter->restore();
      return;
    }

    // Document column: blue underlined link.
    painter->save();
    QFont linkFont = opt.font;
    linkFont.setUnderline(true);
    painter->setFont(linkFont);
    painter->setPen(QColor(0x25, 0x63, 0xeb));
    QRect textRect = opt.widget->style()->subElementRect(
      QStyle::SE_ItemViewItemText, &opt, opt.widget);
    textRect.adjust(4, 0, -4, 0);
    painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, opt.text);
    painter->restore();
  }

  bool editorEvent(QEvent* event,
                   QAbstractItemModel* /*model*/,
                   const QStyleOptionViewItem& /*option*/,
                   const QModelIndex& index) override
  {
    if (event->type() == QEvent::MouseButtonRelease &&
        index.column() == JobTableModel::ColDocument)
    {
      auto* me = static_cast<QMouseEvent*>(event);
      if (me->button() == Qt::LeftButton)
      {
        const QString docName = index.data(Qt::DisplayRole).toString();
        emit documentClicked(docName);
        return true;
      }
    }
    return false;
  }

 signals:
  void documentClicked(const QString& documentName);
};

}  // namespace wpce::ui
