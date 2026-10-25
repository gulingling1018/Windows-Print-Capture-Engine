#pragma once

#include <QDateTime>
#include <QStringList>

namespace wpce::ui
{

/// Presentation-only aggregate shared by the billing model and detail panel.
/// It intentionally contains no mutation or pricing logic.
struct UserGroupSummary
{
  int simplexPages = 0;
  int duplexPages = 0;
  int duplexSheets = 0;
  int colorPages = 0;
  int specialPaperPages = 0;
  int jobCount = 0;
  int pendingCount = 0;
  double totalPrice = 0.0;
  QStringList specialItems;
  QDateTime earliestJobTime;
};

}  // namespace wpce::ui
