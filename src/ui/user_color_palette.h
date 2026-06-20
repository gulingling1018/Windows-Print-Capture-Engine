#pragma once

#include <QColor>
#include <QHash>
#include <QString>

namespace wpce::ui
{

/// 12-colour palette — saturated enough to be clearly distinguishable.
inline QColor userBackgroundColor(const QString& userId)
{
  static const QColor kPalette[] = {
    QColor(0xd0, 0xe2, 0xff),  // blue    (azure)
    QColor(0xc8, 0xf5, 0xd6),  // green   (mint)
    QColor(0xfe, 0xeb, 0xa0),  // amber   (gold)
    QColor(0xfc, 0xd4, 0xe3),  // pink    (blush)
    QColor(0xd6, 0xdf, 0xff),  // indigo  (periwinkle)
    QColor(0xb5, 0xf0, 0xd9),  // emerald (seafoam)
    QColor(0xff, 0xd2, 0xd6),  // rose    (salmon)
    QColor(0xc8, 0xce, 0xfc),  // violet  (lavender)
    QColor(0x8e, 0xea, 0xc8),  // teal    (aquamarine)
    QColor(0xfe, 0xc8, 0x8e),  // orange  (peach)
    QColor(0xa8, 0xef, 0xc8),  // lime    (spearmint)
    QColor(0xe2, 0xc8, 0xfe),  // purple  (orchid)
  };

  const auto idx = static_cast<std::size_t>(
    qHash(userId) % static_cast<uint>(12));
  return kPalette[idx];
}

inline QColor userTextColor(const QString& userId)
{
  static const QColor kTextPalette[] = {
    QColor(0x0d, 0x2b, 0x6e),
    QColor(0x0d, 0x3b, 0x1e),
    QColor(0x5c, 0x2b, 0x00),
    QColor(0x5e, 0x0e, 0x2f),
    QColor(0x1a, 0x1a, 0x5e),
    QColor(0x03, 0x2e, 0x20),
    QColor(0x5e, 0x0b, 0x20),
    QColor(0x2e, 0x10, 0x5e),
    QColor(0x08, 0x2e, 0x2c),
    QColor(0x5e, 0x1f, 0x08),
    QColor(0x1e, 0x3a, 0x08),
    QColor(0x38, 0x10, 0x5e),
  };

  const auto idx = static_cast<std::size_t>(
    qHash(userId) % static_cast<uint>(12));
  return kTextPalette[idx];
}

inline QColor userBorderColor(const QString& userId)
{
  return userBackgroundColor(userId).darker(120);
}

/// Row tint: blends background with white (2:1 ratio for visible colour).
inline QColor subtleUserTint(const QString& userId)
{
  const QColor c = userBackgroundColor(userId);
  return QColor(
    (c.red()   * 3 + 255 * 2) / 5,
    (c.green() * 3 + 255 * 2) / 5,
    (c.blue()  * 3 + 255 * 2) / 5);
}

/// Slightly stronger tint for hover or non-table elements.
inline QColor mediumUserTint(const QString& userId)
{
  const QColor c = userBackgroundColor(userId);
  return QColor(
    (c.red()   * 4 + 255 * 1) / 5,
    (c.green() * 4 + 255 * 1) / 5,
    (c.blue()  * 4 + 255 * 1) / 5);
}

}  // namespace wpce::ui
