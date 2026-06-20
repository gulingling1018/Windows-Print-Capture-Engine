#pragma once

#include <QColor>
#include <QHash>
#include <QString>

namespace wpce::ui
{

/// Generate a stable, visually distinct color for a given user identity.
/// Uses a palette of 12 hand-picked pastel background colors + matching text colors.
inline QColor userBackgroundColor(const QString& userId)
{
  static const QColor kPalette[] = {
    QColor(0xdb, 0xea, 0xfe),  // blue
    QColor(0xdc, 0xfc, 0xe7),  // green
    QColor(0xfe, 0xf3, 0xc7),  // amber
    QColor(0xfc, 0xe7, 0xf3),  // pink
    QColor(0xe0, 0xe7, 0xff),  // indigo
    QColor(0xd1, 0xfa, 0xe5),  // emerald
    QColor(0xff, 0xe4, 0xe6),  // rose
    QColor(0xcd, 0xd5, 0xfd),  // violet
    QColor(0xa7, 0xf3, 0xd0),  // teal
    QColor(0xfe, 0xd7, 0xaa),  // orange
    QColor(0xbb, 0xf7, 0xd0),  // lime
    QColor(0xe9, 0xd5, 0xff),  // purple
  };

  const std::size_t index = static_cast<std::size_t>(
    qHash(userId) % static_cast<uint>(12));
  return kPalette[index];
}

/// Text color that pairs well with the background for readability.
inline QColor userTextColor(const QString& userId)
{
  static const QColor kTextPalette[] = {
    QColor(0x1e, 0x40, 0xaf),  // blue-900
    QColor(0x16, 0x53, 0x34),  // green-900
    QColor(0x92, 0x40, 0x0e),  // amber-900
    QColor(0x9d, 0x17, 0x4d),  // pink-900
    QColor(0x31, 0x2e, 0x81),  // indigo-900
    QColor(0x06, 0x47, 0x10),  // emerald-900
    QColor(0x9f, 0x12, 0x39),  // rose-900
    QColor(0x4c, 0x1d, 0x95),  // violet-900
    QColor(0x13, 0x4e, 0x4a),  // teal-900
    QColor(0x9a, 0x34, 0x12),  // orange-900
    QColor(0x3f, 0x62, 0x12),  // lime-900
    QColor(0x6b, 0x21, 0xa8),  // purple-900
  };

  const std::size_t index = static_cast<std::size_t>(
    qHash(userId) % static_cast<uint>(12));
  return kTextPalette[index];
}

/// A darker border color derived from the user color.
inline QColor userBorderColor(const QString& userId)
{
  const QColor bg = userBackgroundColor(userId);
  return bg.darker(115);
}

}  // namespace wpce::ui
