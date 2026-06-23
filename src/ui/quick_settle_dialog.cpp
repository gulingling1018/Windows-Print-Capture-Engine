#include "ui/quick_settle_dialog.h"
#include "billing/billing_service.h"
#include "domain/billing_batch.h"

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMouseEvent>
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

  buildUi();
}

void QuickSettleDialog::buildUi()
{
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  // ── Custom title bar (draggable) ────────────────────────────────
  titleBar_ = new QWidget(this);
  titleBar_->setFixedHeight(40);
  titleBar_->setObjectName(QStringLiteral("QsdTitleBar"));
  titleBar_->setStyleSheet(QStringLiteral(
    "QWidget#QsdTitleBar { background: #1e293b; border-top-left-radius: 8px;"
    "  border-top-right-radius: 8px; }"));

  auto* titleLayout = new QHBoxLayout(titleBar_);
  titleLayout->setContentsMargins(12, 0, 8, 0);
  titleLayout->setSpacing(8);

  titleLabel_ = new QLabel(QStringLiteral("—"));
  titleLabel_->setStyleSheet(QStringLiteral(
    "color: #f1f5f9; font-size: 14px; font-weight: bold;"));

  stateBadgeLabel_ = new QLabel(QStringLiteral("●"));
  stateBadgeLabel_->setStyleSheet(QStringLiteral(
    "color: #3b82f6; font-size: 12px; font-weight: bold;"));

  titleLayout->addWidget(stateBadgeLabel_);
  titleLayout->addWidget(titleLabel_, 1);

  pinBtn_ = new QPushButton(QStringLiteral("📌"));
  pinBtn_->setFixedSize(28, 28);
  pinBtn_->setToolTip(QStringLiteral("置顶窗口"));
  pinBtn_->setCheckable(true);
  pinBtn_->setChecked(pinned_);
  pinBtn_->setStyleSheet(QStringLiteral(
    "QPushButton { border: none; background: transparent; font-size: 14px;"
    "  border-radius: 4px; }"
    "QPushButton:checked { background: #334155; color: #3b82f6; }"
    "QPushButton:!checked { color: #64748b; }"
    "QPushButton:hover { background: #334155; }"));
  connect(pinBtn_, &QPushButton::toggled, this, &QuickSettleDialog::updatePinnedState);
  titleLayout->addWidget(pinBtn_);

  closeBtn_ = new QPushButton(QStringLiteral("✕"));
  closeBtn_->setFixedSize(28, 28);
  closeBtn_->setStyleSheet(QStringLiteral(
    "QPushButton { border: none; background: transparent; color: #94a3b8;"
    "  font-size: 12px; border-radius: 4px; }"
    "QPushButton:hover { background: #dc2626; color: white; }"));
  connect(closeBtn_, &QPushButton::clicked, this, &QDialog::close);
  titleLayout->addWidget(closeBtn_);

  root->addWidget(titleBar_);

  // ── Content area ────────────────────────────────────────────────
  auto* content = new QWidget(this);
  content->setObjectName(QStringLiteral("QsdContent"));
  content->setStyleSheet(QStringLiteral(
    "QWidget#QsdContent { background: #f8fafc; }"));
  auto* contentLayout = new QVBoxLayout(content);
  contentLayout->setContentsMargins(12, 10, 12, 10);
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
    "  padding: 4px 6px; font-size: 11px; border-bottom: 1px solid #cbd5e1; }"));
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

  // ── Bottom actions ──────────────────────────────────────────────
  auto* actionRow = new QHBoxLayout();
  actionRow->setSpacing(8);

  prevBtn_ = new QPushButton(QStringLiteral("◀"));
  prevBtn_->setFixedSize(36, 36);
  prevBtn_->setToolTip(QStringLiteral("上一个批次"));
  prevBtn_->setStyleSheet(QStringLiteral(
    "QPushButton { background: #f1f5f9; border: 1px solid #cbd5e1; border-radius: 6px;"
    "  font-size: 13px; color: #475569; }"
    "QPushButton:hover { background: #e2e8f0; }"));

  hangUpBtn_ = new QPushButton(QStringLiteral("挂单"));
  hangUpBtn_->setFixedSize(52, 36);
  hangUpBtn_->setToolTip(QStringLiteral("挂起当前批次，切到新用户"));
  hangUpBtn_->setCheckable(true);
  hangUpBtn_->setStyleSheet(QStringLiteral(
    "QPushButton { background: #fffbeb; border: 1px solid #f59e0b; border-radius: 6px;"
    "  font-size: 11px; font-weight: 600; color: #92400e; }"
    "QPushButton:hover { background: #fef3c7; }"
    "QPushButton:checked { background: #f59e0b; color: white; }"));

  resumeBtn_ = new QPushButton(QStringLiteral("恢复"));
  resumeBtn_->setFixedSize(52, 36);
  resumeBtn_->setToolTip(QStringLiteral("恢复挂单的批次"));
  resumeBtn_->setStyleSheet(QStringLiteral(
    "QPushButton { background: #f0fdf4; border: 1px solid #16a34a; border-radius: 6px;"
    "  font-size: 11px; font-weight: 600; color: #166534; }"
    "QPushButton:hover { background: #dcfce7; }"
    "QPushButton:disabled { background: #f1f5f9; border-color: #cbd5e1; color: #94a3b8; }"));
  resumeBtn_->setEnabled(false);

  nextBtn_ = new QPushButton(QStringLiteral("▶"));
  nextBtn_->setFixedSize(36, 36);
  nextBtn_->setToolTip(QStringLiteral("下一个批次"));
  nextBtn_->setStyleSheet(QStringLiteral(
    "QPushButton { background: #f1f5f9; border: 1px solid #cbd5e1; border-radius: 6px;"
    "  font-size: 13px; color: #475569; }"
    "QPushButton:hover { background: #e2e8f0; }"));

  settleBtn_ = new QPushButton(QStringLiteral("结算"));
  settleBtn_->setMinimumHeight(36);
  settleBtn_->setMinimumWidth(80);
  settleBtn_->setStyleSheet(QStringLiteral(
    "QPushButton { background: #2563eb; border: none; border-radius: 6px;"
    "  color: white; font-size: 13px; font-weight: 700; padding: 6px 18px; }"
    "QPushButton:hover { background: #1d4ed8; }"
    "QPushButton:pressed { background: #1e40af; }"));

  actionRow->addWidget(prevBtn_);
  actionRow->addWidget(hangUpBtn_);
  actionRow->addWidget(resumeBtn_);
  actionRow->addWidget(nextBtn_);
  actionRow->addStretch(1);
  actionRow->addWidget(settleBtn_);
  contentLayout->addLayout(actionRow);

  root->addWidget(content, 1);

  connect(prevBtn_, &QPushButton::clicked, this, &QuickSettleDialog::switchToPrev);
  connect(nextBtn_, &QPushButton::clicked, this, &QuickSettleDialog::switchToNext);
  connect(settleBtn_, &QPushButton::clicked, this, &QuickSettleDialog::onSettleClicked);
  connect(hangUpBtn_, &QPushButton::clicked, this, &QuickSettleDialog::onHangUpClicked);
  connect(resumeBtn_, &QPushButton::clicked, this, &QuickSettleDialog::onResumeClicked);
}

void QuickSettleDialog::setCurrentBatchIndex(int index)
{
  if (model_ == nullptr) return;
  if (index < 0 || index >= model_->batchCount()) return;
  currentIndex_ = index;
  rebuildUi();
}

void QuickSettleDialog::rebuildUi()
{
  if (model_ == nullptr || currentIndex_ < 0) return;

  QModelIndex batchIdx = model_->index(currentIndex_, 0);
  const auto* batch = model_->batchAt(batchIdx);
  if (batch == nullptr) return;

  titleLabel_->setText(QString::fromStdString(batch->display_name));

  // State badge color.
  QString badgeColor, badgeText;
  switch (batch->state)
  {
    case domain::BatchState::Active:
      badgeColor = QStringLiteral("#3b82f6"); badgeText = QStringLiteral("●"); break;
    case domain::BatchState::Suspended:
      badgeColor = QStringLiteral("#f59e0b"); badgeText = QStringLiteral("⏸"); break;
    case domain::BatchState::Settled:
      badgeColor = QStringLiteral("#16a34a"); badgeText = QStringLiteral("✓"); break;
    default:
      badgeColor = QStringLiteral("#64748b"); badgeText = QStringLiteral("·"); break;
  }
  stateBadgeLabel_->setText(badgeText);
  stateBadgeLabel_->setStyleSheet(
    QStringLiteral("color: %1; font-size: 14px; font-weight: bold;").arg(badgeColor));

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

    // Duplex label: 长边/短边/单面
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

    // Paper size
    QString paperStr = QStringLiteral("-");
    if (job->metadata.paper_size.state == core::FieldState::Reported)
      paperStr = QString::fromStdString(job->metadata.paper_size.value);
    jobTable_->setItem(r, 5, new QTableWidgetItem(paperStr));

    // Printer name (short)
    QString printerStr = QString::fromStdString(job->snapshot.printer_name);
    if (printerStr.length() > 12) printerStr = printerStr.left(10) + QStringLiteral("..");
    jobTable_->setItem(r, 6, new QTableWidgetItem(printerStr));

    // Price
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

  // Button states.
  updateHangUpState();
  settleBtn_->setEnabled(!batch->is_settled());

  // Resume button: enabled only when there are suspended batches.
  bool hasSuspended = false;
  for (int i = 0; i < model_->batchCount(); ++i)
  {
    const auto* b = model_->batchAt(model_->index(i, 0));
    if (b != nullptr && b->is_suspended()) { hasSuspended = true; break; }
  }
  resumeBtn_->setEnabled(hasSuspended);

  updateColumnVisibility();
}

void QuickSettleDialog::updateColumnVisibility()
{
  int w = width();
  // Priority: price|pages > duplex > copies > printer > paper
  // 460-580: # / doc / pages / duplex / price
  // 581-680: + copies
  // 681-800: + printer
  // 801-900: + paper

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
  QString batchId = model_->batchIdAt(idx);
  if (!batchId.isEmpty())
    emit settleRequested(batchId);
}

void QuickSettleDialog::onHangUpClicked()
{
  if (model_ == nullptr || currentIndex_ < 0) return;
  QModelIndex idx = model_->index(currentIndex_, 0);
  QString batchId = model_->batchIdAt(idx);
  if (!batchId.isEmpty())
    emit hangUpRequested(batchId);
  // Switch to next relevant batch.
  switchToNext();
}

void QuickSettleDialog::onResumeClicked()
{
  // Find the most recent suspended batch and resume it.
  if (model_ == nullptr) return;
  for (int i = 0; i < model_->batchCount(); ++i)
  {
    QModelIndex idx = model_->index(i, 0);
    const auto* batch = model_->batchAt(idx);
    if (batch != nullptr && batch->is_suspended())
    {
      QString batchId = model_->batchIdAt(idx);
      emit resumeRequested(batchId);
      setCurrentBatchIndex(i);
      return;
    }
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

void QuickSettleDialog::updateHangUpState()
{
  if (model_ == nullptr || currentIndex_ < 0) return;
  const auto* batch = model_->batchAt(model_->index(currentIndex_, 0));
  if (batch == nullptr) return;
  bool isSuspended = batch->is_suspended();
  hangUpBtn_->setChecked(isSuspended);
  hangUpBtn_->setText(isSuspended ? QStringLiteral("已挂") : QStringLiteral("挂单"));
  hangUpBtn_->setEnabled(!batch->is_settled());
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
    // Check if click is in title bar area.
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

}  // namespace wpce::ui
