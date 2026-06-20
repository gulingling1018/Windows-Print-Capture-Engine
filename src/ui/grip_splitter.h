#pragma once

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QSplitter>
#include <QSplitterHandle>

namespace wpce::ui
{

/// A splitter handle that draws visible grip dots and changes cursor.
class GripHandle final : public QSplitterHandle
{
  Q_OBJECT

 public:
  GripHandle(Qt::Orientation orientation, QSplitter* parent)
    : QSplitterHandle(orientation, parent)
  {
    setCursor(orientation == Qt::Horizontal
      ? Qt::SplitHCursor
      : Qt::SplitVCursor);
  }

 protected:
  void paintEvent(QPaintEvent*) override
  {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const bool hover = underMouse();
    const QColor base(hover ? 0x25 : 0x63, hover ? 0x63 : 0xeb, 0xeb);

    // Fill the handle with a subtle background.
    const QColor fill(hover ? 0xdb : 0xe2, hover ? 0xe8 : 0xe8, 0xf0);
    painter.fillRect(rect(), fill);

    // Draw grip dots along the center of the handle.
    painter.setPen(Qt::NoPen);
    painter.setBrush(base.lighter(hover ? 130 : 105));

    const bool horizontal = orientation() == Qt::Horizontal;
    const int length = horizontal ? height() : width();
    const int centerX = horizontal ? width() / 2 : width() / 4;
    const int centerY = horizontal ? height() / 4 : height() / 2;
    const int dotRadius = 3;
    const int dotSpacing = 14;
    const int dotCount = std::max(2, length / dotSpacing);

    const int totalDots = dotCount * dotSpacing;
    const int start = (length - totalDots) / 2 + dotSpacing / 2;

    for (int i = 0; i < dotCount; ++i)
    {
      const int pos = start + i * dotSpacing;
      if (horizontal)
      {
        painter.drawEllipse(QPoint(centerX, pos), dotRadius, dotRadius);
      }
      else
      {
        painter.drawEllipse(QPoint(pos, centerY), dotRadius, dotRadius);
      }
    }
  }

  void enterEvent(QEnterEvent*) override
  {
    setCursor(orientation() == Qt::Horizontal
      ? Qt::SplitHCursor
      : Qt::SplitVCursor);
    update();
  }

  void leaveEvent(QEvent*) override
  {
    update();
  }
};

/// A QSplitter that uses GripHandle for visible drag feedback.
class GripSplitter final : public QSplitter
{
  Q_OBJECT

 public:
  explicit GripSplitter(Qt::Orientation orientation, QWidget* parent = nullptr)
    : QSplitter(orientation, parent)
  {
    setHandleWidth(10);
    setChildrenCollapsible(false);
    setOpaqueResize(true);
  }

 protected:
  QSplitterHandle* createHandle() override
  {
    return new GripHandle(orientation(), this);
  }
};

}  // namespace wpce::ui
