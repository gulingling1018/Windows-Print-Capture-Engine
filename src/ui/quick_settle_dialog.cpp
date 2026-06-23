#include "ui/quick_settle_dialog.h"
#include "billing/billing_service.h"
#include "domain/billing_batch.h"

#include <QCloseEvent>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMouseEvent>
#include <QTimer>
#include <QVBoxLayout>

namespace wpce::ui
{

QuickSettleDialog::QuickSettleDialog(BillingBatchModel* model,
                                      BillingService* service,
                                      const PricingConfig* pricing,
                                      QWidget* parent)
  : QDialog(parent), model_(model), service_(service), pricingConfig_(pricing)
{
  setWindowTitle(QStringLiteral("快速结算"));
  setMinimumSize(460, 600);
  setMaximumSize(900, 800);
  resize(460, 600);
  setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
  setAttribute(Qt::WA_TranslucentBackground, false);

  buildUi();
}

void QuickSettleDialog::buildUi()
{
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  // ── Custom title bar (draggable) ────────────────────────────────
  titleBar_ = new QWidget(this);
  titleBar_->setFixedHeight(44);
  titleBar_->setObjectName(QStringLiteral("QsdTitleBar"));
  titleBar_->setStyleSheet(QStringLiteral(
    "QWidget#QsdTitleBar {"
    "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
    "    stop:0 #1e293b, stop:1 #334155);"
    "  border-top-left-radius: 10px;"
    "  border-top-right-radius: 10px;"
    "}"));

  auto* titleLayout = new QHBoxLayout(titleBar_);
  titleLayout->setContentsMargins(14, 0, 10, 0);
  titleLayout->setSpacing(8);

  stateBadgeLabel_ = new QLabel(QStringLiteral("●"));
  stateBadgeLabel_->setStyleSheet(QStringLiteral(
    "color: #3b82f6; font-size: 12px; font-weight: bold;"));

  titleLabel_ = new QLabel(QStringLiteral("—"));
  titleLabel_->setStyleSheet(QStringLiteral(
    "color: #f1f5f9; font-size: 14px; font-weight: bold;"));

  positionLabel_ = new QLabel(QStringLiteral("0/0"));
  positionLabel_->setStyleSheet(QStringLiteral(
    "color: #94a3b8; font-size: 11px; font-weight: 600;"
    "background: #475569; border-radius: 8px; padding: 2px 8px;"));

  titleLayout->addWidget(stateBadgeLabel_);
  titleLayout->addWidget(titleLabel_, 1);
  titleLayout->addWidget(positionLabel_);

  pinBtn_ = new QPushButton(QStringLiteral("PIN"));
  pinBtn_->setFixedSize(36, 26);
  pinBtn_->setToolTip(QStringLiteral("置顶窗口"));
  pinBtn_->setCheckable(true);
  pinBtn_->setChecked(pinned_);
  pinBtn_->setStyleSheet(QStringLiteral(
    "QPushButton { border: 1px solid #475569; background: transparent;"
    "  font-size: 9px; font-weight: bold; color: #64748b;"
    "  border-radius: 4px; letter-spacing: 1px; }"
    "QPushButton:checked { background: #475569; color: #38bdf8; border-color: #38bdf8; }"
    "QPushButton:hover { background: #475569; }"));
  connect(pinBtn_, &QPushButton::toggled, this, &QuickSettleDialog::updatePinnedState);
  titleLayout->addWidget(pinBtn_);

  closeBtn_ = new QPushButton(QStringLiteral("×"));
  closeBtn_->setFixedSize(28, 28);
  closeBtn_->setStyleSheet(QStringLiteral(
    "QPushButton { border: none; background: transparent; color: #94a3b8;"
    "  font-size: 16px; font-weight: bold; border-radius: 4px; }"
    "QPushButton:hover { background: #dc2626; color: white; }"));
  connect(closeBtn_, &QPushButton::clicked, this, &QDialog::close);
  titleLayout->addWidget(closeBtn_);

  root->addWidget(titleBar_);

  // ── Content area ────────────────────────────────────────────────
  auto* content = new QWidget(this);
  content->setObjectName(QStringLiteral("QsdContent"));
  content->setStyleSheet(QStringLiteral(
    "QWidget#QsdContent { background: #f8fafc;"
    "  border-bottom-left-radius: 10px;"
    "  border-bottom-right-radius: 10px; }"));
  auto* contentLayout = new QVBoxLayout(content);
  contentLayout->setContentsMargins(14, 12, 14, 12);
  contentLayout->setSpacing(8);

  // ── First document anchor (top, blue) ───────────────────────────
  auto* firstFrame = new QFrame();
  firstFrame->setObjectName(QStringLiteral("QsdFirstAnchor"));
  firstFrame->setStyleSheet(QStringLiteral(
    "QFrame#QsdFirstAnchor { background: #eff6ff; border: 1px solid #bfdbfe;"
    "  border-radius: 6px; }"));
  auto* firstLayout = new QHBoxLayout(firstFrame);
  firstLayout->setContentsMargins(10, 6, 10, 6);
  firstLayout->setSpacing(6);

  auto* firstIcon = new QLabel(QStringLiteral("▲"));
  firstIcon->setStyleSheet(QStringLiteral("color: #2563eb; font-size: 10px;"));
  firstDocLabel_ = new QLabel(QStringLiteral("首: —"));
  firstDocLabel_->setStyleSheet(QStringLiteral(
    "color: #1e40af; font-size: 11px; font-weight: 600;"));
  firstDocLabel_->setWordWrap(false);
  firstTimeLabel_ = new QLabel(QStringLiteral(""));
  firstTimeLabel_->setStyleSheet(QStringLiteral("color: #64748b; font-size: 10px;"));
  firstPagesLabel_ = new QLabel(QStringLiteral(""));
  firstPagesLabel_->setStyleSheet(QStringLiteral("color: #64748b; font-size: 10px;"));

  firstLayout->addWidget(firstIcon);
  firstLayout->addWidget(firstDocLabel_, 1);
  firstLayout->addWidget(firstPagesLabel_);
  firstLayout->addWidget(firstTimeLabel_);
  contentLayout->addWidget(firstFrame);

  // ── Job table ───────────────────────────────────────────────────
  jobTable_ = new QTableWidget(0, 8);
  jobTable_->setHorizontalHeaderLabels({
    QStringLiteral("#"), QStringLiteral("文档"),
    QStringLiteral("份"), QStringLiteral("单双"),
    QStringLiteral("页"), QStringLiteral("纸张"),
    QStringLiteral("打印机"), QStringLiteral("¥") });
  jobTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  jobTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  jobTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  jobTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
  jobTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
  jobTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
  jobTable_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
  jobTable_->horizontalHeader()->setSectionResizeMode(7, QHeaderView::ResizeToContents);
  jobTable_->verticalHeader()->hide();
  jobTable_->setShowGrid(false);
  jobTable_->setAlternatingRowColors(true);
  jobTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  jobTable_->setSelectionMode(QAbstractItemView::NoSelection);
  jobTable_->setStyleSheet(QStringLiteral(
    "QTableWidget { border: 1px solid #e2e8f0; border-radius: 6px; background: white; }"
    "QTableWidget::item { padding: 4px 6px; font-size: 12px; }"
    "QHeaderView::section { background: #f1f5f9; color: #475569; font-weight: bold;"
    "  padding: 4px 6px; font-size: 11px; border: none; border-bottom: 1px solid #cbd5e1; }"));
  contentLayout->addWidget(jobTable_, 1);

  // ── Last document anchor (bottom, green) ────────────────────────
  auto* lastFrame = new QFrame();
  lastFrame->setObjectName(QStringLiteral("QsdLastAnchor"));
  lastFrame->setStyleSheet(QStringLiteral(
    "QFrame#QsdLastAnchor { background: #f0fdf4; border: 1px solid #bbf7d0;"
    "  border-radius: 6px; }"));
  auto* lastLayout = new QHBoxLayout(lastFrame);
  lastLayout->setContentsMargins(10, 6, 10, 6);
  lastLayout->setSpacing(6);

  auto* lastIcon = new QLabel(QStringLiteral("▼"));
  lastIcon->setStyleSheet(QStringLiteral("color: #16a34a; font-size: 10px;"));
  lastDocLabel_ = new QLabel(QStringLiteral("尾: —"));
  lastDocLabel_->setStyleSheet(QStringLiteral(
    "color: #166534; font-size: 11px; font-weight: 600;"));
  lastDocLabel_->setWordWrap(false);
  lastTimeLabel_ = new QLabel(QStringLiteral(""));
  lastTimeLabel_->setStyleSheet(QStringLiteral("color: #64748b; font-size: 10px;"));
  lastPagesLabel_ = new QLabel(QStringLiteral(""));
  lastPagesLabel_->setStyleSheet(QStringLiteral("color: #64748b; font-size: 10px;"));

  lastLayout->addWidget(lastIcon);
  lastLayout->addWidget(lastDocLabel_, 1);
  lastLayout->addWidget(lastPagesLabel_);
  lastLayout->addWidget(lastTimeLabel_);
  contentLayout->addWidget(lastFrame);

  // ── Summary bar ─────────────────────────────────────────────────
  summaryLabel_ = new QLabel(QStringLiteral("文档0 · 单0页 · 双0页 · ¥0.00"));
  summaryLabel_->setStyleSheet(QStringLiteral(
    "color: #0f172a; font-size: 13px; font-weight: bold; padding: 8px 12px;"
    "background: #f1f5f9; border: 1px solid #e2e8f0; border-radius: 6px;"));
  summaryLabel_->setAlignment(Qt::AlignCenter);
  contentLayout->addWidget(summaryLabel_);

  // ── Bottom action bar ───────────────────────────────────────────
  auto* actionBar = new QWidget();
  actionBar->setObjectName(QStringLiteral("QsdActionBar"));
  actionBar->setStyleSheet(QStringLiteral(
    "QWidget#QsdActionBar { background: #f1f5f9;"
    "  border-top: 1px solid #e2e8f0; border-radius: 0; }"));
  actionBar->setFixedHeight(56);

  auto* actionRow = new QHBoxLayout(actionBar);
  actionRow->setContentsMargins(12, 8, 12, 8);
  actionRow->setSpacing(8);

  prevBtn_ = new QPushButton(QStringLiteral("◀"));
  prevBtn_->setFixedSize(38, 38);
  prevBtn_->setToolTip(QStringLiteral("上一个 (←)"));
  prevBtn_->setStyleSheet(QStringLiteral(
    "QPushButton { background: white; border: 1px solid #cbd5e1; border-radius: 8px;"
    "  font-size: 14px; color: #475569; }"
    "QPushButton:hover { background: #e2e8f0; border-color: #94a3b8; }"
    "QPushButton:pressed { background: #cbd5e1; }"));

  actionBtn_ = new QPushButton(QStringLiteral("挂单"));
  actionBtn_->setFixedHeight(36);
  actionBtn_->setMinimumWidth(64);
  actionBtn_->setToolTip(QStringLiteral("挂单/恢复 (H)"));
  actionBtn_->setStyleSheet(QStringLiteral(
    "QPushButton { background: #fffbeb; border: 1px solid #f59e0b; border-radius: 6px;"
    "  font-size: 12px; font-weight: 600; color: #92400e; padding: 0 12px; }"
    "QPushButton:hover { background: #fef3c7; }"
    "QPushButton:disabled { background: #f9fafb; border-color: #e5e7eb; color: #d1d5db; }"));

  nextBtn_ = new QPushButton(QStringLiteral("▶"));
  nextBtn_->setFixedSize(38, 38);
  nextBtn_->setToolTip(QStringLiteral("下一个 (→)"));
  nextBtn_->setStyleSheet(QStringLiteral(
    "QPushButton { background: white; border: 1px solid #cbd5e1; border-radius: 8px;"
    "  font-size: 14px; color: #475569; }"
    "QPushButton:hover { background: #e2e8f0; border-color: #94a3b8; }"
    "QPushButton:pressed { background: #cbd5e1; }"));

  settleBtn_ = new QPushButton(QStringLiteral("结算"));
  settleBtn_->setFixedHeight(40);
  settleBtn_->setMinimumWidth(100);
  settleBtn_->setToolTip(QStringLiteral("结算当前批次 (Enter)"));
  settleBtn_->setStyleSheet(QStringLiteral(
    "QPushButton { background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
    "    stop:0 #3b82f6, stop:1 #2563eb);"
    "  border: none; border-radius: 8px;"
    "  color: white; font-size: 14px; font-weight: 700; padding: 0 20px; }"
    "QPushButton:hover { background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
    "    stop:0 #2563eb, stop:1 #1d4ed8); }"
    "QPushButton:pressed { background: #1e40af; }"
    "QPushButton:disabled { background: #93c5fd; color: rgba(255,255,255,0.7); }"));

  actionRow->addWidget(prevBtn_);
  actionRow->addWidget(actionBtn_);
  actionRow->addWidget(nextBtn_);
  actionRow->addStretch(1);
  actionRow->addWidget(settleBtn_);
  contentLayout->addWidget(actionBar);

  root->addWidget(content, 1);

  connect(prevBtn_, &QPushButton::clicked, this, &QuickSettleDialog::switchToPrev);
  connect(nextBtn_, &QPushButton::clicked, this, &QuickSettleDialog::switchToNext);
  connect(settleBtn_, &QPushButton::clicked, this, &QuickSettleDialog::onSettleClicked);
  connect(actionBtn_, &QPushButton::clicked, this, &QuickSettleDialog::onActionClicked);
}

void QuickSettleDialog::setCurrentBatchIndex(int index)
{
  if (model_ == nullptr) return;
  if (index < 0 || index >= model_->batchCount()) return;
  currentIndex_ = index;
  rebuildUi();

  QModelIndex idx = model_->index(currentIndex_, 0);
  QString batchId = model_->batchIdAt(idx);
  if (!batchId.isEmpty())
    emit batchFocusChanged(batchId);
}

void QuickSettleDialog::refresh()
{
  if (!isVisible()) return;
  if (model_ == nullptr || currentIndex_ < 0) return;
  if (currentIndex_ >= model_->batchCount())
    currentIndex_ = model_->batchCount() - 1;
  if (currentIndex_ >= 0)
    rebuildUi();
}

void QuickSettleDialog::rebuildUi()
{
  if (model_ == nullptr || currentIndex_ < 0) return;

  QModelIndex batchIdx = model_->index(currentIndex_, 0);
  const auto* batch = model_->batchAt(batchIdx);
  if (batch == nullptr) return;

  titleLabel_->setText(QString::fromStdString(batch->display_name));

  // State badge color + content background tint for visual distinction.
  QString badgeColor, badgeText, contentBg;
  switch (batch->state)
  {
    case domain::BatchState::Active:
      badgeColor = QStringLiteral("#3b82f6");
      badgeText = QStringLiteral("●");
      contentBg = QStringLiteral("#f8fafc");
      break;
    case domain::BatchState::Suspended:
      badgeColor = QStringLiteral("#f59e0b");
      badgeText = QStringLiteral("⏸");
      contentBg = QStringLiteral("#fffdf7");
      break;
    case domain::BatchState::Settled:
      badgeColor = QStringLiteral("#16a34a");
      badgeText = QStringLiteral("✓");
      contentBg = QStringLiteral("#f0fdf4");
      break;
    default:
      badgeColor = QStringLiteral("#64748b");
      badgeText = QStringLiteral("·");
      contentBg = QStringLiteral("#f8fafc");
      break;
  }
  stateBadgeLabel_->setText(badgeText);
  stateBadgeLabel_->setStyleSheet(
    QStringLiteral("color: %1; font-size: 14px; font-weight: bold;").arg(badgeColor));

  // Tint the content area to reflect state.
  auto* contentWidget = findChild<QWidget*>(QStringLiteral("QsdContent"));
  if (contentWidget != nullptr)
  {
    contentWidget->setStyleSheet(QStringLiteral(
      "QWidget#QsdContent { background: %1;"
      "  border-bottom-left-radius: 10px;"
      "  border-bottom-right-radius: 10px; }").arg(contentBg));
  }

  updatePositionIndicator();

  // First/last doc anchors.
  auto formatTime = [](const std::string& iso) -> QString {
    if (iso.empty()) return QStringLiteral("");
    QDateTime dt = QDateTime::fromString(QString::fromStdString(iso), Qt::ISODate);
    if (dt.isValid()) return dt.toString(QStringLiteral("HH:mm"));
    return QString::fromStdString(iso);
  };

  firstDocLabel_->setText(QStringLiteral("首: %1")
    .arg(batch->first_doc_anchor.document_name.empty()
         ? QStringLiteral("—")
         : QString::fromStdString(batch->first_doc_anchor.document_name)));
  firstTimeLabel_->setText(formatTime(batch->first_doc_anchor.submit_time_utc));
  firstPagesLabel_->setText(batch->first_doc_anchor.page_count > 0
    ? QStringLiteral("%1页").arg(batch->first_doc_anchor.page_count) : QString());

  lastDocLabel_->setText(QStringLiteral("尾: %1")
    .arg(batch->last_doc_anchor.document_name.empty()
         ? QStringLiteral("—")
         : QString::fromStdString(batch->last_doc_anchor.document_name)));
  lastTimeLabel_->setText(formatTime(batch->last_doc_anchor.submit_time_utc));
  lastPagesLabel_->setText(batch->last_doc_anchor.page_count > 0
    ? QStringLiteral("%1页").arg(batch->last_doc_anchor.page_count) : QString());

  // Job table.
  int childCount = model_->rowCount(batchIdx);
  jobTable_->setRowCount(childCount);
  double totalPrice = 0.0;

  for (int r = 0; r < childCount; ++r)
  {
    QModelIndex childIdx = model_->index(r, 0, batchIdx);
    const auto* job = model_->jobAt(childIdx);
    if (job == nullptr) continue;

    int copies = parse_copies(job->metadata);
    int pages = parse_total_pages(job->metadata);

    QString duplexLabel;
    if (job->metadata.duplex.state == core::FieldState::Reported)
    {
      const auto& dv = job->metadata.duplex.value;
      if (dv == "simplex") duplexLabel = QStringLiteral("单面");
      else if (dv == "long-edge (book)" || dv == "TwoSidedLongEdge" || dv == "vertical")
        duplexLabel = QStringLiteral("长边");
      else if (dv == "short-edge (calendar)" || dv == "TwoSidedShortEdge" || dv == "horizontal")
        duplexLabel = QStringLiteral("短边");
      else if (dv == "duplex") duplexLabel = QStringLiteral("双面");
      else duplexLabel = QString::fromStdString(dv);
    }
    else duplexLabel = QStringLiteral("-");

    jobTable_->setItem(r, 0, new QTableWidgetItem(QString::number(r + 1)));
    jobTable_->setItem(r, 1, new QTableWidgetItem(
      QString::fromStdString(job->snapshot.document_name)));
    jobTable_->setItem(r, 2, new QTableWidgetItem(QString::number(copies)));
    jobTable_->setItem(r, 3, new QTableWidgetItem(duplexLabel));
    jobTable_->setItem(r, 4, new QTableWidgetItem(QString::number(pages)));

    QString paperStr = QStringLiteral("-");
    if (job->metadata.paper_size.state == core::FieldState::Reported)
      paperStr = QString::fromStdString(job->metadata.paper_size.value);
    jobTable_->setItem(r, 5, new QTableWidgetItem(paperStr));

    QString printerStr = QString::fromStdString(job->snapshot.printer_name);
    if (printerStr.length() > 12) printerStr = printerStr.left(10) + QStringLiteral("..");
    jobTable_->setItem(r, 6, new QTableWidgetItem(printerStr));

    double price = job->pricing.total_price;
    if (job->pricing.price_pending) price = 0.0;
    auto* priceItem = new QTableWidgetItem(QStringLiteral("%1")
      .arg(QString::number(price, 'f', 2)));
    priceItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    if (job->pricing.price_pending)
    {
      priceItem->setBackground(QColor(0xff, 0xfb, 0xeb));
      priceItem->setForeground(QColor(0x92, 0x40, 0x0e));
    }
    else if (price > 0.0)
    {
      priceItem->setForeground(QColor(0x16, 0x65, 0x34));
    }
    jobTable_->setItem(r, 7, priceItem);

    totalPrice += price;
  }

  // Summary bar.
  bool hasPending = batch->pending_count > 0;
  summaryLabel_->setStyleSheet(QStringLiteral(
    "color: %1; font-size: 13px; font-weight: bold; padding: 8px 12px;"
    "background: %2; border: 1px solid %3; border-radius: 6px;")
    .arg(hasPending ? QStringLiteral("#92400e") : QStringLiteral("#0f172a"))
    .arg(hasPending ? QStringLiteral("#fffbeb") : QStringLiteral("#f1f5f9"))
    .arg(hasPending ? QStringLiteral("#f59e0b") : QStringLiteral("#e2e8f0")));

  QString pendingText = hasPending
    ? QStringLiteral(" · 待填%1项").arg(batch->pending_count) : QString();
  summaryLabel_->setText(
    QStringLiteral("文档%1 · 单%2页 · 双%3页%4 · ¥%5")
      .arg(batch->document_count)
      .arg(batch->simplex_pages)
      .arg(batch->duplex_pages)
      .arg(pendingText)
      .arg(QString::number(totalPrice, 'f', 2)));

  // Update action buttons based on state.
  updateActionButtons();
  updateColumnVisibility();
}

void QuickSettleDialog::updateActionButtons()
{
  if (model_ == nullptr || currentIndex_ < 0) return;
  const auto* batch = model_->batchAt(model_->index(currentIndex_, 0));
  if (batch == nullptr) return;

  bool settled = batch->is_settled();
  bool suspended = batch->is_suspended();
  bool active = batch->is_active();

  // Suspended batches cannot be settled directly — must resume first.
  settleBtn_->setEnabled(active);
  if (suspended)
    settleBtn_->setToolTip(QStringLiteral("需先恢复才能结算"));
  else
    settleBtn_->setToolTip(QStringLiteral("结算当前批次 (Enter)"));

  if (settled)
  {
    actionBtn_->setText(QStringLiteral("已结"));
    actionBtn_->setEnabled(false);
    actionBtn_->setStyleSheet(QStringLiteral(
      "QPushButton { background: #f9fafb; border: 1px solid #e5e7eb; border-radius: 6px;"
      "  font-size: 12px; font-weight: 600; color: #d1d5db; padding: 0 12px; }"));
    settleBtn_->setEnabled(false);
  }
  else if (suspended)
  {
    actionBtn_->setText(QStringLiteral("恢复"));
    actionBtn_->setEnabled(true);
    actionBtn_->setStyleSheet(QStringLiteral(
      "QPushButton { background: #f0fdf4; border: 1px solid #16a34a; border-radius: 6px;"
      "  font-size: 12px; font-weight: 600; color: #166534; padding: 0 12px; }"
      "QPushButton:hover { background: #dcfce7; }"));
  }
  else if (active)
  {
    actionBtn_->setText(QStringLiteral("挂单"));
    actionBtn_->setEnabled(true);
    actionBtn_->setStyleSheet(QStringLiteral(
      "QPushButton { background: #fffbeb; border: 1px solid #f59e0b; border-radius: 6px;"
      "  font-size: 12px; font-weight: 600; color: #92400e; padding: 0 12px; }"
      "QPushButton:hover { background: #fef3c7; }"));
  }
}

void QuickSettleDialog::updatePositionIndicator()
{
  if (model_ == nullptr) { positionLabel_->setText(QStringLiteral("0/0")); return; }

  int unsettledCount = 0;
  int currentPos = 0;
  for (int i = 0; i < model_->batchCount(); ++i)
  {
    const auto* b = model_->batchAt(model_->index(i, 0));
    if (b == nullptr || b->is_settled()) continue;
    ++unsettledCount;
    if (i == currentIndex_) currentPos = unsettledCount;
  }

  if (currentPos == 0)
  {
    // Current is a settled batch — show absolute position.
    positionLabel_->setText(QStringLiteral("%1/%2")
      .arg(currentIndex_ + 1).arg(model_->batchCount()));
    positionLabel_->setStyleSheet(QStringLiteral(
      "color: #6b7280; font-size: 11px; font-weight: 600;"
      "background: #374151; border-radius: 8px; padding: 2px 8px;"));
  }
  else
  {
    positionLabel_->setText(QStringLiteral("%1/%2").arg(currentPos).arg(unsettledCount));
    positionLabel_->setStyleSheet(QStringLiteral(
      "color: #e0f2fe; font-size: 11px; font-weight: 600;"
      "background: #475569; border-radius: 8px; padding: 2px 8px;"));
  }
}

void QuickSettleDialog::updateColumnVisibility()
{
  int w = width();
  jobTable_->setColumnHidden(2, w < 581);  // copies
  jobTable_->setColumnHidden(5, w < 801);  // paper
  jobTable_->setColumnHidden(6, w < 681);  // printer
}

void QuickSettleDialog::switchToPrev()
{
  if (model_ == nullptr || model_->batchCount() == 0) return;
  int prev = model_->prevRelevantRow(currentIndex_);
  if (prev >= 0) setCurrentBatchIndex(prev);
}

void QuickSettleDialog::switchToNext()
{
  if (model_ == nullptr || model_->batchCount() == 0) return;
  int next = model_->nextRelevantRow(currentIndex_);
  if (next >= 0) setCurrentBatchIndex(next);
}

void QuickSettleDialog::onSettleClicked()
{
  if (model_ == nullptr || currentIndex_ < 0) return;
  QModelIndex idx = model_->index(currentIndex_, 0);
  const auto* batch = model_->batchAt(idx);
  if (batch == nullptr) return;

  if (batch->is_suspended())
  {
    // Flash the action button to hint: resume first.
    actionBtn_->setStyleSheet(QStringLiteral(
      "QPushButton { background: #fef3c7; border: 2px solid #f59e0b; border-radius: 6px;"
      "  font-size: 12px; font-weight: 600; color: #92400e; padding: 0 12px; }"));
    QTimer::singleShot(800, this, [this]() { updateActionButtons(); });
    return;
  }

  if (batch->is_settled()) return;

  QString batchId = model_->batchIdAt(idx);
  if (!batchId.isEmpty())
    emit settleRequested(batchId);

  // Flash settle button green to confirm.
  settleBtn_->setStyleSheet(QStringLiteral(
    "QPushButton { background: #16a34a; border: none; border-radius: 8px;"
    "  color: white; font-size: 14px; font-weight: 700; padding: 0 20px; }"));
  settleBtn_->setText(QStringLiteral("✓ 完成"));

  QTimer::singleShot(600, this, [this]() {
    settleBtn_->setText(QStringLiteral("结算"));
    settleBtn_->setStyleSheet(QStringLiteral(
      "QPushButton { background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
      "    stop:0 #3b82f6, stop:1 #2563eb);"
      "  border: none; border-radius: 8px;"
      "  color: white; font-size: 14px; font-weight: 700; padding: 0 20px; }"
      "QPushButton:hover { background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
      "    stop:0 #2563eb, stop:1 #1d4ed8); }"
      "QPushButton:pressed { background: #1e40af; }"
      "QPushButton:disabled { background: #93c5fd; color: rgba(255,255,255,0.7); }"));
    switchToNext();
  });
}

void QuickSettleDialog::onActionClicked()
{
  if (model_ == nullptr || currentIndex_ < 0) return;
  QModelIndex idx = model_->index(currentIndex_, 0);
  const auto* batch = model_->batchAt(idx);
  if (batch == nullptr) return;

  QString batchId = model_->batchIdAt(idx);
  if (batchId.isEmpty()) return;

  if (batch->is_active())
  {
    emit hangUpRequested(batchId);
    // Flash amber confirmation.
    actionBtn_->setText(QStringLiteral("✓ 已挂"));
    actionBtn_->setStyleSheet(QStringLiteral(
      "QPushButton { background: #f59e0b; border: none; border-radius: 6px;"
      "  font-size: 12px; font-weight: 600; color: white; padding: 0 12px; }"));
    QTimer::singleShot(500, this, [this]() {
      updateActionButtons();
      switchToNext();
    });
  }
  else if (batch->is_suspended())
  {
    emit resumeRequested(batchId);
    // Flash green confirmation.
    actionBtn_->setText(QStringLiteral("✓ 已恢复"));
    actionBtn_->setStyleSheet(QStringLiteral(
      "QPushButton { background: #16a34a; border: none; border-radius: 6px;"
      "  font-size: 12px; font-weight: 600; color: white; padding: 0 12px; }"));
    QTimer::singleShot(500, this, [this]() { updateActionButtons(); });
  }
}

void QuickSettleDialog::updatePinnedState()
{
  pinned_ = pinBtn_->isChecked();
  Qt::WindowFlags flags = windowFlags();
  if (pinned_)
    flags |= Qt::WindowStaysOnTopHint;
  else
    flags &= ~Qt::WindowStaysOnTopHint;
  setWindowFlags(flags);
  show();
}

void QuickSettleDialog::keyPressEvent(QKeyEvent* event)
{
  switch (event->key())
  {
    case Qt::Key_Left:
      switchToPrev();
      event->accept();
      return;
    case Qt::Key_Right:
      switchToNext();
      event->accept();
      return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
      onSettleClicked();
      event->accept();
      return;
    case Qt::Key_H:
      if (actionBtn_->isEnabled())
        onActionClicked();
      event->accept();
      return;
    case Qt::Key_Escape:
      close();
      event->accept();
      return;
    default:
      break;
  }
  QDialog::keyPressEvent(event);
}

void QuickSettleDialog::closeEvent(QCloseEvent* event)
{
  event->accept();
}

void QuickSettleDialog::resizeEvent(QResizeEvent* event)
{
  QDialog::resizeEvent(event);
  updateColumnVisibility();
}

void QuickSettleDialog::mousePressEvent(QMouseEvent* event)
{
  if (event->button() == Qt::LeftButton && titleBar_ != nullptr)
  {
    if (event->pos().y() < titleBar_->height())
    {
      dragging_ = true;
      dragStartPos_ = event->globalPosition().toPoint() - frameGeometry().topLeft();
      event->accept();
    }
  }
}

void QuickSettleDialog::mouseMoveEvent(QMouseEvent* event)
{
  if (dragging_ && (event->buttons() & Qt::LeftButton))
  {
    move(event->globalPosition().toPoint() - dragStartPos_);
    event->accept();
  }
}

void QuickSettleDialog::mouseReleaseEvent(QMouseEvent* event)
{
  if (event->button() == Qt::LeftButton)
  {
    dragging_ = false;
    event->accept();
  }
}

}  // namespace wpce::ui
