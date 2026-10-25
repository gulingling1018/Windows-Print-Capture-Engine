#include "ui/quick_settle_dialog.h"
#include "billing/billing_service.h"
#include "domain/billing_batch.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QEvent>
#include <QResizeEvent>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QMouseEvent>
#include <QShowEvent>
#include <QSpinBox>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#include <windowsx.h>
#endif

namespace
{

/// Delegate for column 4 (pages) in QuickSettleDialog.
/// Allows the operator to edit only the colour-page count m in the "m+n"
/// display format.  n is always total_pages - m and is never editable.
class ColorPagesDelegate : public QStyledItemDelegate
{
public:
  using QStyledItemDelegate::QStyledItemDelegate;

  QWidget* createEditor(QWidget* parent,
                        const QStyleOptionViewItem& /*option*/,
                        const QModelIndex& index) const override
  {
    auto* editor = new QSpinBox(parent);
    int totalPages = index.data(Qt::UserRole + 1).toInt();
    editor->setRange(0, totalPages);
    editor->setButtonSymbols(QAbstractSpinBox::NoButtons);
    editor->setAlignment(Qt::AlignCenter);
    editor->setFrame(false);
    editor->setContentsMargins(0, 0, 0, 0);
    editor->setStyleSheet(QStringLiteral(
      "QSpinBox { padding: 0 2px; margin: 0; border: 1px solid #2563eb;"
      " border-radius: 3px; background: white; color: #0f172a; }"));
    editor->setToolTip(QStringLiteral("彩印页数 (0-%1)").arg(totalPages));
    return editor;
  }

  void updateEditorGeometry(QWidget* editor,
                            const QStyleOptionViewItem& option,
                            const QModelIndex&) const override
  {
    editor->setGeometry(option.rect.adjusted(1, 1, -1, -1));
  }

  void setEditorData(QWidget* editor, const QModelIndex& index) const override
  {
    const QString text = index.data(Qt::DisplayRole).toString();
    const int plusPos = text.indexOf(QChar('+'));
    int m = 0;
    if (plusPos > 0)
      m = text.left(plusPos).toInt();
    qobject_cast<QSpinBox*>(editor)->setValue(m);
  }

  void setModelData(QWidget* editor,
                    QAbstractItemModel* model,
                    const QModelIndex& index) const override
  {
    auto* spinBox = qobject_cast<QSpinBox*>(editor);
    const int newM = spinBox->value();
    const int totalPages = index.data(Qt::UserRole + 1).toInt();
    const int n = totalPages - newM;
    model->setData(index,
                   QStringLiteral("%1+%2").arg(newM).arg(n),
                   Qt::DisplayRole);
  }
};

bool is_user_activity_event(QEvent* event)
{
  switch (event->type())
  {
    case QEvent::KeyPress:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::Wheel:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
      return true;
    case QEvent::MouseMove:
      return static_cast<QMouseEvent*>(event)->buttons() != Qt::NoButton;
    default:
      return false;
  }
}

bool belongs_to_dialog(const wpce::ui::QuickSettleDialog* dialog, QObject* watched)
{
  if (dialog == nullptr || watched == nullptr) return false;
  auto* widget = qobject_cast<QWidget*>(watched);
  return widget != nullptr && (widget == dialog || dialog->isAncestorOf(widget));
}

}  // namespace

namespace wpce::ui
{

QuickSettleDialog::QuickSettleDialog(BillingBatchModel* model,
                                      BillingService* service,
                                      const PricingConfig* pricing,
                                      int initialWidth,
                                      int initialHeight,
                                      int initialCollapsedHeight,
                                      bool initialCollapsed,
                                      QWidget* parent)
  : QDialog(parent), model_(model), service_(service), pricingConfig_(pricing),
    collapsed_(initialCollapsed), collapsedHeight_(initialCollapsedHeight)
{
  setWindowTitle(QStringLiteral("快速结算"));
  int w = std::max(initialWidth, 520);
  int h = std::max(initialHeight, 260);
  expandedSize_ = QSize(w, h);
  resize(w, h);
  setMinimumSize(420, 260);
  setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
  setAttribute(Qt::WA_TranslucentBackground, false);

  buildUi();
  qApp->installEventFilter(this);

  // Inactivity auto-refresh: jump to priority row after 10 s idle.
  inactivityTimer_.setSingleShot(true);
  inactivityTimer_.setInterval(kInactivityMs);
  connect(&inactivityTimer_, &QTimer::timeout, this, [this]() {
    if (!isVisible()) return;
    int row = priorityRow();
    if (row >= 0 && row != currentIndex_)
      setCurrentBatchIndex(row);
  });

  // Debounced refresh: coalesce rapid batchUpdated signals.
  refreshDebounce_.setSingleShot(true);
  refreshDebounce_.setInterval(kRefreshDebounceMs);
  connect(&refreshDebounce_, &QTimer::timeout, this, [this]() {
    if (!isVisible() || model_ == nullptr) return;
    if (!currentBatchId_.isEmpty())
    {
      const QModelIndex stableIndex = model_->batchIndex(currentBatchId_);
      if (stableIndex.isValid()) currentIndex_ = stableIndex.row();
    }
    if (currentIndex_ >= model_->batchCount())
      currentIndex_ = model_->batchCount() - 1;
    if (currentIndex_ < 0 && model_->batchCount() > 0)
      currentIndex_ = 0;
    if (currentIndex_ >= 0)
      rebuildUi();
  });
}

QuickSettleDialog::~QuickSettleDialog()
{
  qApp->removeEventFilter(this);
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
  titleLayout->setContentsMargins(14, 0, 12, 0);
  titleLayout->setSpacing(10);

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

  // Shared base for icon-only title-bar buttons — white on dark bg,
  // semi-transparent fill so the button boundary is clearly visible.
  auto makeTbBtn = [](QPushButton* btn, const QString& text,
                      const QString& hoverBg, const QString& pressedBg) {
    btn->setText(text);
    btn->setFixedSize(30, 26);
    btn->setStyleSheet(QStringLiteral(
      "QPushButton { border: 1px solid rgba(255,255,255,0.25);"
      "  background: rgba(255,255,255,0.12); color: #ffffff;"
      "  border-radius: 5px; font-size: 15px; font-weight: bold; }"
      "QPushButton:hover { background: %1; color: #ffffff;"
      "  border-color: transparent; }"
      "QPushButton:pressed { background: %2; color: #ffffff; }")
      .arg(hoverBg).arg(pressedBg));
  };

  pinBtn_ = new QPushButton(QStringLiteral("PIN"));
  pinBtn_->setFixedSize(38, 26);
  pinBtn_->setToolTip(QStringLiteral("置顶窗口"));
  pinBtn_->setCheckable(true);
  pinBtn_->setChecked(pinned_);
  pinBtn_->setStyleSheet(QStringLiteral(
    "QPushButton { border: 1px solid rgba(255,255,255,0.30);"
    "  background: rgba(255,255,255,0.12); font-size: 9px; font-weight: bold;"
    "  color: #ffffff; border-radius: 5px; letter-spacing: 1px; }"
    "QPushButton:checked { background: rgba(56,189,248,0.35);"
    "  color: #ffffff; border-color: #38bdf8; }"
    "QPushButton:hover { background: rgba(255,255,255,0.22);"
    "  color: #ffffff; border-color: rgba(255,255,255,0.55); }"
    "QPushButton:pressed { background: rgba(255,255,255,0.08); }"));
  connect(pinBtn_, &QPushButton::toggled, this, &QuickSettleDialog::updatePinnedState);
  titleLayout->addWidget(pinBtn_);

  collapseBtn_ = new QPushButton(QStringLiteral("▾"));
  collapseBtn_->setToolTip(QStringLiteral("折叠/展开 (C)"));
  makeTbBtn(collapseBtn_, QStringLiteral("▾"), QStringLiteral("#3b82f6"), QStringLiteral("#2563eb"));
  connect(collapseBtn_, &QPushButton::clicked, this, &QuickSettleDialog::toggleCollapsed);
  titleLayout->addWidget(collapseBtn_);

  minimizeBtn_ = new QPushButton(QStringLiteral("—"));
  minimizeBtn_->setToolTip(QStringLiteral("最小化到托盘"));
  makeTbBtn(minimizeBtn_, QStringLiteral("—"), QStringLiteral("#f59e0b"), QStringLiteral("#d97706"));
  connect(minimizeBtn_, &QPushButton::clicked, this, &QuickSettleDialog::minimizeToTray);
  titleLayout->addWidget(minimizeBtn_);

  closeBtn_ = new QPushButton(QStringLiteral("✕"));
  closeBtn_->setToolTip(QStringLiteral("关闭"));
  makeTbBtn(closeBtn_, QStringLiteral("✕"), QStringLiteral("#ef4444"), QStringLiteral("#dc2626"));
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
  firstAnchorFrame_ = firstFrame;
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
  // Columns: 0=文档, 1=打印机, 2=份数, 3=单|双面, 4=页数(m+n), 5=总页数, 6=价格
  jobTable_ = new QTableWidget(0, 7);
  jobTable_->setHorizontalHeaderLabels({
    QStringLiteral("文档"), QStringLiteral("打印机"),
    QStringLiteral("份数"), QStringLiteral("单|双面"),
    QStringLiteral("页数"), QStringLiteral("总页数"),
    QStringLiteral("价格") });
  jobTable_->horizontalHeader()->setMinimumSectionSize(34);
  // Document column stretches to fill remaining space.
  jobTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  jobTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
  jobTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
  jobTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
  jobTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
  jobTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Fixed);
  jobTable_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Fixed);
  jobTable_->setColumnWidth(1, 80);    // 打印机
  jobTable_->setColumnWidth(2, 36);    // 份数
  jobTable_->setColumnWidth(3, 50);    // 单|双面
  jobTable_->setColumnWidth(4, 78);    // 页数 (m+n)，为高 DPI 数字编辑器留足空间
  jobTable_->setColumnWidth(5, 56);    // 总页数
  jobTable_->setColumnWidth(6, 60);    // 价格
  jobTable_->verticalHeader()->hide();
  jobTable_->verticalHeader()->setDefaultSectionSize(28);
  jobTable_->setShowGrid(false);
  jobTable_->setAlternatingRowColors(true);
  jobTable_->setEditTriggers(QAbstractItemView::DoubleClicked);
  jobTable_->setSelectionMode(QAbstractItemView::SingleSelection);
  // Delegate for colour-pages editing (column 4 only)
  jobTable_->setItemDelegateForColumn(4, new ColorPagesDelegate(jobTable_));
  connect(jobTable_, &QTableWidget::cellChanged, this, &QuickSettleDialog::onPageCellChanged);
  jobTable_->setStyleSheet(QStringLiteral(
    "QTableWidget { border: 1px solid #e2e8f0; border-radius: 6px; background: white; }"
    "QTableWidget::item { padding: 4px 6px; font-size: 12px; }"
    "QHeaderView::section { background: #f1f5f9; color: #475569; font-weight: bold;"
    "  padding: 4px 4px; font-size: 11px; border: none;"
    "  border-bottom: 1px solid #cbd5e1; border-right: 1px solid #e2e8f0; }"
    "QHeaderView::section:last { border-right: none; }"));
  contentLayout->addWidget(jobTable_, 1);

  // ── Last document anchor (bottom, green) ────────────────────────
  auto* lastFrame = new QFrame();
  lastFrame->setObjectName(QStringLiteral("QsdLastAnchor"));
  lastAnchorFrame_ = lastFrame;
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

  // ── Color pending indicator (visible in collapsed mode) ─────────
  colorPendingLabel_ = new QLabel();
  colorPendingLabel_->setStyleSheet(QStringLiteral(
    "color: #92400e; font-size: 11px; font-weight: 600; padding: 4px 10px;"
    "background: #fef3c7; border: 1px solid #f59e0b; border-radius: 5px;"));
  colorPendingLabel_->setWordWrap(true);
  colorPendingLabel_->hide();
  contentLayout->addWidget(colorPendingLabel_);

  // ── Collapsed color input area (replaces text-only indicator) ──
  collapsedColorArea_ = new QWidget();
  collapsedColorArea_->setObjectName(QStringLiteral("QsdCollapsedColorArea"));
  collapsedColorArea_->hide();
  contentLayout->addWidget(collapsedColorArea_);

  // ── Bottom action bar ───────────────────────────────────────────
  auto* actionBar = new QWidget();
  actionBar_ = actionBar;
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
  contentArea_ = content;

  connect(settleBtn_, &QPushButton::clicked, this, &QuickSettleDialog::onSettleClicked);
  connect(actionBtn_, &QPushButton::clicked, this, &QuickSettleDialog::onActionClicked);

  createTrayIcon();
}

void QuickSettleDialog::setCurrentBatchIndex(int index)
{
  if (model_ == nullptr) return;
  if (index < 0 || index >= model_->batchCount()) return;
  currentIndex_ = index;
  rebuildUi();
  updateNavButtonStates();

  QModelIndex idx = model_->index(currentIndex_, 0);
  QString batchId = model_->batchIdAt(idx);
  if (!batchId.isEmpty())
  {
    currentBatchId_ = batchId;
    emit batchFocusChanged(batchId);
  }
}

void QuickSettleDialog::refresh()
{
  if (!isVisible()) return;
  if (model_ == nullptr) return;

  if (!currentBatchId_.isEmpty())
  {
    const QModelIndex stableIndex = model_->batchIndex(currentBatchId_);
    if (stableIndex.isValid()) currentIndex_ = stableIndex.row();
  }

  // Clamp current index if batches were removed.
  if (currentIndex_ >= model_->batchCount())
    currentIndex_ = model_->batchCount() - 1;
  if (currentIndex_ < 0 && model_->batchCount() > 0)
    currentIndex_ = 0;

  // Debounce: coalesce rapid updates within kRefreshDebounceMs.
  if (!refreshDebounce_.isActive())
    refreshDebounce_.start();
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

  // Job table — columns: 0=文档, 1=打印机, 2=份数, 3=单|双面, 4=页数, 5=总页数, 6=价格
  int childCount = model_->rowCount(batchIdx);
  // Block signals while populating to prevent onPageCellChanged re-entrancy.
  jobTable_->blockSignals(true);
  jobTable_->setRowCount(childCount);
  double totalPrice = 0.0;
  int totalColorPages = 0;
  int collapsedColorRows = 0;

  for (int r = 0; r < childCount; ++r)
  {
    QModelIndex childIdx = model_->index(r, 0, batchIdx);
    const auto* job = model_->jobAt(childIdx);
    if (job == nullptr) continue;

    int copies = parse_copies(job->metadata);
    int pages = parse_total_pages(job->metadata);
    int totalPgs = pages * copies;
    bool jobIsColor = common::is_color_job(job->metadata);
    bool isColorDowngraded = job->pricing.color_downgraded;
    bool isColorJobOnColorPrinter = jobIsColor && !isColorDowngraded;
    if (isColorJobOnColorPrinter)
      ++collapsedColorRows;
    jobTable_->setRowHidden(r, collapsed_ && !isColorJobOnColorPrinter);

    // Column 0: 文档 (read-only)
    auto* docItem = new QTableWidgetItem(
      QString::fromStdString(job->snapshot.document_name));
    docItem->setFlags(docItem->flags() & ~Qt::ItemIsEditable);
    jobTable_->setItem(r, 0, docItem);

    // Column 1: 打印机 (read-only)
    QString printerStr = QString::fromStdString(job->snapshot.printer_name);
    if (printerStr.length() > 12) printerStr = printerStr.left(10) + QStringLiteral("..");
    auto* printerItem = new QTableWidgetItem(printerStr);
    printerItem->setFlags(printerItem->flags() & ~Qt::ItemIsEditable);
    jobTable_->setItem(r, 1, printerItem);

    // Column 2: 份数 (read-only)
    auto* copiesItem = new QTableWidgetItem(QString::number(copies));
    copiesItem->setFlags(copiesItem->flags() & ~Qt::ItemIsEditable);
    jobTable_->setItem(r, 2, copiesItem);

    // Column 3: 单|双面 (read-only)
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
    auto* duplexItem = new QTableWidgetItem(duplexLabel);
    duplexItem->setFlags(duplexItem->flags() & ~Qt::ItemIsEditable);
    jobTable_->setItem(r, 3, duplexItem);

    // Column 4: 页数 (m+n format for color jobs on color printers — ONLY editable column)
    QString pagesStr;
    auto* pagesItem = new QTableWidgetItem();
    if (isColorJobOnColorPrinter)
    {
      int m = job->pricing.color_pages;
      int n = pages - m;
      pagesStr = QStringLiteral("%1+%2").arg(m).arg(n);
      totalColorPages += m;
      // Allow operator to edit m (colour pages) in the "m+n" display.
      pagesItem->setFlags(pagesItem->flags() | Qt::ItemIsEditable);
      pagesItem->setData(Qt::UserRole, job->snapshot.job_id);
      pagesItem->setData(Qt::UserRole + 1, pages);
    }
    else
    {
      pagesStr = QString::number(pages);
      pagesItem->setFlags(pagesItem->flags() & ~Qt::ItemIsEditable);
    }
    pagesItem->setText(pagesStr);
    jobTable_->setItem(r, 4, pagesItem);

    // Column 5: 总页数 (read-only)
    QString totalPagesStr;
    if (isColorJobOnColorPrinter)
    {
      int m = job->pricing.color_pages;
      int n = pages - m;
      int totalM = m * copies;
      int totalN = n * copies;
      totalPagesStr = QStringLiteral("%1+%2").arg(totalM).arg(totalN);
    }
    else
    {
      totalPagesStr = QString::number(totalPgs);
    }
    auto* totalPagesItem = new QTableWidgetItem(totalPagesStr);
    totalPagesItem->setFlags(totalPagesItem->flags() & ~Qt::ItemIsEditable);
    jobTable_->setItem(r, 5, totalPagesItem);

    // Column 6: 价格 (read-only)
    double price = job->pricing.total_price;
    auto* priceItem = new QTableWidgetItem();
    priceItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    priceItem->setFlags(priceItem->flags() & ~Qt::ItemIsEditable);
    if (job->pricing.price_pending)
    {
      if (job->pricing.pending_reason == "awaiting_color_pages")
        priceItem->setText(QStringLiteral("待填彩印"));
      else if (job->pricing.pending_reason == "unmatched_rule")
        priceItem->setText(QStringLiteral("无规则"));
      else
        priceItem->setText(QStringLiteral("待定价"));
      priceItem->setBackground(QColor(0xff, 0xfb, 0xeb));
      priceItem->setForeground(QColor(0x92, 0x40, 0x0e));
    }
    else if (price > 0.0)
    {
      priceItem->setText(QString::number(price, 'f', 2));
      priceItem->setForeground(QColor(0x16, 0x65, 0x34));
    }
    else
    {
      priceItem->setText(QStringLiteral("0.00"));
    }
    jobTable_->setItem(r, 6, priceItem);

    totalPrice += price;
  }
  jobTable_->blockSignals(false);

  // Collapsed color area is no longer used — job table remains visible.
  collapsedColorArea_->hide();
  colorPendingLabel_->hide();
  collapsedTableHeight_ = 0;
  if (collapsed_)
  {
    if (collapsedColorRows > 0)
    {
      jobTable_->show();
      const int rowHeight = std::max(24, jobTable_->verticalHeader()->defaultSectionSize());
      const int headerHeight = std::max(28, jobTable_->horizontalHeader()->height());
      const int targetHeight = std::clamp(headerHeight + rowHeight * collapsedColorRows + 8,
                                          86, 240);
      collapsedTableHeight_ = targetHeight;
      jobTable_->setMaximumHeight(targetHeight);
    }
    else
    {
      jobTable_->hide();
    }
  }
  else
  {
    collapsedTableHeight_ = 0;
    jobTable_->show();
    jobTable_->setMaximumHeight(QWIDGETSIZE_MAX);
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
  QString colorPagesText = totalColorPages > 0
    ? QStringLiteral(" · 彩印%1页").arg(totalColorPages) : QString();
  summaryLabel_->setText(
    QStringLiteral("文档%1 · 单%2页 · 双%3页%4%5 · ¥%6")
      .arg(batch->document_count)
      .arg(batch->simplex_pages)
      .arg(batch->duplex_pages)
      .arg(colorPagesText)
      .arg(pendingText)
      .arg(QString::number(totalPrice, 'f', 2)));

  // Update action buttons based on state.
  updateActionButtons();

  // If collapsed, ensure the window is tall enough to show summary + action bar
  // (jobTable_ is hidden in collapsed mode so sizeHint is compact).
  if (collapsed_)
  {
    int target = std::max(sizeHint().height(), minimumCollapsedHeight());
    setMinimumHeight(target);
    if (height() < target)
    {
      inToggle_ = true;
      resize(width(), target);
      collapsedHeight_ = target;
      inToggle_ = false;
    }
  }
  else
  {
    setMinimumSize(420, 260);
    updateHeightResponsiveState();
  }
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
  settleBtn_->setEnabled(active && batch->pending_count == 0);
  if (suspended)
    settleBtn_->setToolTip(QStringLiteral("需先恢复才能结算"));
  else
    settleBtn_->setToolTip(batch->pending_count > 0
      ? QStringLiteral("仍有待定价项目")
      : QStringLiteral("结算当前批次 (Enter)"));

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

void QuickSettleDialog::updateNavButtonStates()
{
  if (model_ == nullptr) return;
  int n = model_->batchCount();

  bool atLeft  = (n == 0 || currentIndex_ <= 0);
  bool atRight = (n == 0 || currentIndex_ >= n - 1);

  prevBtn_->setEnabled(!atLeft);
  nextBtn_->setEnabled(!atRight);
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

int QuickSettleDialog::minimumCollapsedHeight() const
{
  const int titleH = titleBar_ != nullptr ? titleBar_->height() : 44;
  int contentH = 0;
  int visibleCount = 0;

  auto addVisible = [&](const QWidget* widget, int preferredHeight = -1) {
    if (widget == nullptr || widget->isHidden()) return;
    if (visibleCount > 0) contentH += 8;
    const int h = preferredHeight > 0 ? preferredHeight : widget->sizeHint().height();
    contentH += std::max(0, h);
    ++visibleCount;
  };

  QMargins margins;
  if (contentArea_ != nullptr && contentArea_->layout() != nullptr)
    margins = contentArea_->layout()->contentsMargins();

  int tableH = collapsedTableHeight_;
  if (tableH <= 0 && jobTable_ != nullptr && !jobTable_->isHidden())
  {
    tableH = jobTable_->maximumHeight() < QWIDGETSIZE_MAX
               ? jobTable_->maximumHeight()
               : jobTable_->sizeHint().height();
  }

  addVisible(jobTable_, tableH);
  addVisible(summaryLabel_);
  addVisible(colorPendingLabel_);
  addVisible(collapsedColorArea_);
  addVisible(actionBar_, actionBar_ != nullptr ? actionBar_->height() : -1);

  return std::max(150, titleH + margins.top() + contentH + margins.bottom());
}

void QuickSettleDialog::updateHeightResponsiveState()
{
  if (contentArea_ == nullptr || collapsed_) return;

  const bool compact = height() > 0 && height() < 360;
  compactHeightMode_ = compact;

  if (auto* layout = qobject_cast<QVBoxLayout*>(contentArea_->layout()))
  {
    layout->setContentsMargins(compact ? 10 : 14,
                               compact ? 8 : 12,
                               compact ? 10 : 14,
                               compact ? 8 : 12);
    layout->setSpacing(compact ? 6 : 8);
  }

  if (firstAnchorFrame_ != nullptr)
    firstAnchorFrame_->setVisible(!compact);
  if (lastAnchorFrame_ != nullptr)
    lastAnchorFrame_->setVisible(!compact);

  if (jobTable_ != nullptr)
  {
    jobTable_->verticalHeader()->setDefaultSectionSize(compact ? 24 : 28);
    jobTable_->horizontalHeader()->setVisible(height() >= 300);
  }

  if (actionBar_ != nullptr)
    actionBar_->setFixedHeight(compact ? 48 : 56);

  const QSize navSize(compact ? 34 : 38, compact ? 34 : 38);
  if (prevBtn_ != nullptr) prevBtn_->setFixedSize(navSize);
  if (nextBtn_ != nullptr) nextBtn_->setFixedSize(navSize);
  if (actionBtn_ != nullptr) actionBtn_->setFixedHeight(compact ? 34 : 36);
  if (settleBtn_ != nullptr) settleBtn_->setFixedHeight(compact ? 36 : 40);
}

void QuickSettleDialog::switchToPrev()
{
  if (model_ == nullptr || model_->batchCount() == 0) return;
  if (currentIndex_ <= 0) return; // 已在最左边界，非循环
  resetInactivityTimer();
  resetInactivityTimer();
  setCurrentBatchIndex(currentIndex_ - 1);
}

void QuickSettleDialog::switchToNext()
{
  if (model_ == nullptr || model_->batchCount() == 0) return;
  if (currentIndex_ >= model_->batchCount() - 1) return; // 已在最右边界，非循环
  resetInactivityTimer();
  resetInactivityTimer();
  setCurrentBatchIndex(currentIndex_ + 1);
}

void QuickSettleDialog::switchToPrevRelevant()
{
  if (model_ == nullptr || model_->batchCount() == 0) return;
  int prev = model_->prevRelevantRow(currentIndex_);
  if (prev >= 0) { resetInactivityTimer(); setCurrentBatchIndex(prev); }
}

void QuickSettleDialog::switchToNextRelevant()
{
  if (model_ == nullptr || model_->batchCount() == 0) return;
  int next = model_->nextRelevantRow(currentIndex_);
  if (next >= 0) { resetInactivityTimer(); setCurrentBatchIndex(next); }
}

void QuickSettleDialog::onSettleClicked()
{
  if (model_ == nullptr || currentIndex_ < 0) return;
  resetInactivityTimer();
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
    switchToNextRelevant();
  });
}

void QuickSettleDialog::onActionClicked()
{
  if (model_ == nullptr || currentIndex_ < 0) return;
  resetInactivityTimer();
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
      switchToNextRelevant();
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

void QuickSettleDialog::onPageCellChanged(int row, int col)
{
  if (col != 4) return;

  auto* item = jobTable_->item(row, col);
  if (item == nullptr) return;

  const bool ok = item->data(Qt::UserRole).isValid();
  if (!ok) return;

  const std::uint32_t jobId = item->data(Qt::UserRole).toUInt();
  if (jobId == 0) return;

  // Parse the new m from the "m+n" display text written by the delegate.
  const QString text = item->text();
  const int plusPos = text.indexOf(QChar('+'));
  if (plusPos < 0) return;

  bool parseOk = false;
  const int newM = text.left(plusPos).toInt(&parseOk);
  if (!parseOk) return;

  resetInactivityTimer();

  // Defer via single-shot to avoid re-entrancy during active edit.
  QTimer::singleShot(0, this, [this, jobId, newM]() {
    if (service_ != nullptr)
      service_->setColorPages(jobId, newM);
  });
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
    case Qt::Key_C:
      toggleCollapsed();
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

void QuickSettleDialog::showEvent(QShowEvent* event)
{
  QDialog::showEvent(event);
  resetInactivityTimer();

  if (firstShow_ && collapsed_)
  {
    firstShow_ = false;
    // collapsed_ is currently true (from config). We need to flip it to false
    // so toggleCollapsed() treats this as "expanding → collapsing" direction.
    // But we actually want to go INTO collapsed mode, so:
    collapsed_ = false;  // pretend we're expanded so toggleCollapsed collapses us
    toggleCollapsed();   // now properly enters collapsed state
  }
  else
  {
    firstShow_ = false;
  }
}

void QuickSettleDialog::resizeEvent(QResizeEvent* event)
{
  QDialog::resizeEvent(event);
  if (inToggle_) return;
  if (collapsed_)
    collapsedHeight_ = height();
  else
  {
    expandedSize_ = size();
    updateHeightResponsiveState();
  }
  emit geometryChanged(expandedSize_.width(), expandedSize_.height(),
                       collapsedHeight_, collapsed_);
}

void QuickSettleDialog::hideEvent(QHideEvent* event)
{
  emit geometryChanged(expandedSize_.width(), expandedSize_.height(),
                       collapsedHeight_, collapsed_);
  QDialog::hideEvent(event);
}

void QuickSettleDialog::closeEvent(QCloseEvent* event)
{
  auto reply = QMessageBox::question(this, QStringLiteral("确认关闭"),
                                     QStringLiteral("确定要关闭快速结算窗口吗？"),
                                     QMessageBox::Yes | QMessageBox::No);
  if (reply == QMessageBox::Yes)
  {
    emit geometryChanged(expandedSize_.width(), expandedSize_.height(),
                         collapsedHeight_, collapsed_);
    if (trayIcon_ != nullptr)
      trayIcon_->hide();
    event->accept();
  }
  else
  {
    event->ignore();
  }
}

bool QuickSettleDialog::eventFilter(QObject* watched, QEvent* event)
{
  if (is_user_activity_event(event) && belongs_to_dialog(this, watched))
    resetInactivityTimer();

  if (watched != prevBtn_ && watched != nextBtn_)
    return QDialog::eventFilter(watched, event);

  if (event->type() == QEvent::MouseButtonPress)
  {
    auto* me = static_cast<QMouseEvent*>(event);
    if (me->button() == Qt::LeftButton)
    {
      qint64 now = QDateTime::currentMSecsSinceEpoch();
      if (now - lastNavPressMs_ < static_cast<qint64>(QApplication::doubleClickInterval()))
      {
        navIsDoubleClick_ = true;
        if (watched == prevBtn_) switchToPrevRelevant();
        else switchToNextRelevant();
      }
      else
      {
        navIsDoubleClick_ = false;
      }
      lastNavPressMs_ = now;
    }
    return false; // let button show press effect
  }
  else if (event->type() == QEvent::MouseButtonRelease)
  {
    auto* me = static_cast<QMouseEvent*>(event);
    if (me->button() == Qt::LeftButton && !navIsDoubleClick_)
    {
      if (watched == prevBtn_) switchToPrev();
      else switchToNext();
    }
    navIsDoubleClick_ = false;
    if (auto* btn = qobject_cast<QPushButton*>(watched))
      btn->setDown(false);
    return true; // suppress clicked() to avoid duplicate action
  }
  else if (event->type() == QEvent::MouseButtonDblClick)
  {
    return true; // already handled in press
  }

  return QDialog::eventFilter(watched, event);
}

#ifdef _WIN32
bool QuickSettleDialog::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
  auto* msg = static_cast<MSG*>(message);
  if (msg->message == WM_NCHITTEST)
  {
    // All coordinates in physical pixels — WM_NCHITTEST lParam is always
    // physical regardless of DPI, and GetWindowRect returns physical too.
    HWND hwnd = reinterpret_cast<HWND>(winId());
    RECT rc;
    GetWindowRect(hwnd, &rc);

    const int sx = GET_X_LPARAM(msg->lParam);
    const int sy = GET_Y_LPARAM(msg->lParam);
    const int x  = sx - rc.left;
    const int y  = sy - rc.top;
    const int w  = rc.right - rc.left;
    const int h  = rc.bottom - rc.top;

    if (w <= 0 || h <= 0)
      return QDialog::nativeEvent(eventType, message, result);

    // Scale logical sizes to physical pixels to match the coordinate space.
    const double dpr = devicePixelRatio();
    const int kEdge  = static_cast<int>(6 * dpr);
    const int titleH = static_cast<int>(
        (titleBar_ != nullptr ? titleBar_->height() : 44) * dpr);

    // ── Resize edges (physical pixel boundary) ──────────────────────
    const bool top    = y < kEdge;
    const bool bottom = y > h - kEdge;
    const bool left   = x < kEdge;
    const bool right  = x > w - kEdge;

    if (top && left)     { *result = HTTOPLEFT;     return true; }
    if (top && right)    { *result = HTTOPRIGHT;    return true; }
    if (bottom && left)  { *result = HTBOTTOMLEFT;  return true; }
    if (bottom && right) { *result = HTBOTTOMRIGHT; return true; }
    if (top)             { *result = HTTOP;         return true; }
    if (bottom)          { *result = HTBOTTOM;      return true; }
    if (left)            { *result = HTLEFT;        return true; }
    if (right)           { *result = HTRIGHT;       return true; }

    // ── Title bar — native drag via HTCAPTION ───────────────────────
    if (y < titleH)
    {
      // Only interactive child widgets (buttons) need HTCLIENT.
      // Labels and the title bar background itself should drag.
      QPoint logicalPos(static_cast<int>(x / dpr),
                        static_cast<int>(y / dpr));
      QWidget* child = childAt(logicalPos);
      if (child != nullptr && qobject_cast<QPushButton*>(child) != nullptr)
      {
        *result = HTCLIENT;
        return true;
      }
      *result = HTCAPTION;
      return true;
    }

    *result = HTCLIENT;
    return true;
  }
  return QDialog::nativeEvent(eventType, message, result);
}
#endif

int QuickSettleDialog::priorityRow() const
{
  if (model_ == nullptr || model_->batchCount() == 0) return -1;

  int firstActive = -1;
  int firstSuspended = -1;
  int lastSettled = -1;

  for (int i = 0; i < model_->batchCount(); ++i)
  {
    const auto* b = model_->batchAt(model_->index(i, 0));
    if (b == nullptr) continue;
    if (b->is_active() && firstActive < 0) firstActive = i;
    else if (b->is_suspended() && firstSuspended < 0) firstSuspended = i;
    else if (b->is_settled()) lastSettled = i;
  }

  if (firstActive >= 0) return firstActive;
  if (firstSuspended >= 0) return firstSuspended;
  if (lastSettled >= 0) return lastSettled;
  return 0;
}

void QuickSettleDialog::ensurePriorityView()
{
  if (!isVisible()) return;
  if (model_ == nullptr || model_->batchCount() == 0) return;
  if (currentIndex_ < 0 || currentIndex_ >= model_->batchCount()) return;

  // Batch-structure changes can arrive while the operator is reviewing a
  // different card.  Defer the fallback jump until the user has been idle.
  if (inactivityTimer_.isActive()) return;

  // Auto-jump if the idle dialog is not already showing the priority row.
  // New active batches still jump immediately through onBatchCreated().
  const auto* curBatch = model_->batchAt(model_->index(currentIndex_, 0));
  if (curBatch != nullptr && curBatch->is_active()) return;

  int prio = priorityRow();
  if (prio >= 0 && prio != currentIndex_)
    setCurrentBatchIndex(prio);
}

void QuickSettleDialog::resetInactivityTimer()
{
  if (isVisible())
    inactivityTimer_.start(kInactivityMs);
}

void QuickSettleDialog::createTrayIcon()
{
  if (trayIcon_ != nullptr) return;

  trayMenu_ = new QMenu(this);
  auto* showAction = new QAction(QStringLiteral("显示快速结算窗口"), this);
  connect(showAction, &QAction::triggered, this, &QuickSettleDialog::showFromTray);
  trayMenu_->addAction(showAction);

  auto* closeAction = new QAction(QStringLiteral("关闭"), this);
  connect(closeAction, &QAction::triggered, this, &QDialog::close);
  trayMenu_->addAction(closeAction);

  trayIcon_ = new QSystemTrayIcon(this);
  trayIcon_->setContextMenu(trayMenu_);
  trayIcon_->setToolTip(QStringLiteral("WPCE 快速结算"));
  if (!windowIcon().isNull())
    trayIcon_->setIcon(windowIcon());
  else if (qApp->windowIcon().isNull() == false)
    trayIcon_->setIcon(qApp->windowIcon());

  connect(trayIcon_, &QSystemTrayIcon::activated,
          this, &QuickSettleDialog::onTrayActivated);
}

void QuickSettleDialog::minimizeToTray()
{
  minimizedToTray_ = true;
  if (trayIcon_ != nullptr)
  {
    trayIcon_->show();
    trayIcon_->showMessage(
      QStringLiteral("WPCE 快速结算"),
      QStringLiteral("窗口已最小化到托盘，点击图标可恢复。"),
      QSystemTrayIcon::Information, 3000);
  }
  hide();
}

void QuickSettleDialog::showFromTray()
{
  minimizedToTray_ = false;
  showNormal();
  raise();
  activateWindow();
}

void QuickSettleDialog::onTrayActivated(QSystemTrayIcon::ActivationReason reason)
{
  if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
  {
    if (isVisible())
    {
      hide();
      minimizedToTray_ = true;
    }
    else
    {
      showFromTray();
    }
  }
}

void QuickSettleDialog::toggleCollapsed()
{
  inToggle_ = true;

  if (!collapsed_)
  {
    // Snapshot expanded size before any layout change.
    expandedSize_ = size();
    collapsed_ = true;

    firstAnchorFrame_->hide();
    lastAnchorFrame_->hide();
    colorPendingLabel_->hide();
    collapsedColorArea_->hide();
    collapseBtn_->setText(QStringLiteral("▴"));
    collapseBtn_->setToolTip(QStringLiteral("展开 (C)"));
    setMinimumSize(320, 120);
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    rebuildUi();

    // sizeHint() recalculates with hidden widgets, giving the compact height.
    int computedH = std::max(sizeHint().height(), minimumCollapsedHeight());
    // Accept saved height only if it looks like a real collapsed height
    // (not a leftover expanded height from a previous bugged session).
    bool savedOk = (collapsedHeight_ >= computedH &&
                    collapsedHeight_ < expandedSize_.height() - 30);
    int h = savedOk ? collapsedHeight_ : computedH;
    setMinimumHeight(computedH);
    resize(width(), h);
    collapsedHeight_ = h;
  }
  else
  {
    // Snapshot collapsed height before any layout change.
    collapsedHeight_ = height();
    collapsed_ = false;

    jobTable_->show();
    jobTable_->setMaximumHeight(QWIDGETSIZE_MAX);
    collapsedTableHeight_ = 0;
    setMinimumSize(420, 260);
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    firstAnchorFrame_->show();
    lastAnchorFrame_->show();
    colorPendingLabel_->hide();
    collapsedColorArea_->hide();
    collapseBtn_->setText(QStringLiteral("▾"));
    collapseBtn_->setToolTip(QStringLiteral("折叠 (C)"));
    compactHeightMode_ = false;
    rebuildUi();
    resize(expandedSize_);
    updateHeightResponsiveState();
  }

  inToggle_ = false;
  emit geometryChanged(expandedSize_.width(), expandedSize_.height(),
                       collapsedHeight_, collapsed_);
}

void QuickSettleDialog::onBatchCreated(const QString& batchId)
{
  if (!isVisible()) return;
  if (model_ == nullptr) return;

  // Find the row for the newly created batch.
  QModelIndex idx = model_->batchIndex(batchId);
  if (!idx.isValid()) return;

  // Always switch to a new active batch — it represents the current customer.
  const auto* batch = model_->batchAt(idx);
  if (batch != nullptr && batch->is_active())
    setCurrentBatchIndex(idx.row());
}

}  // namespace wpce::ui
