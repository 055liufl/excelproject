#include "CompareDetailDialog.h"

#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include "Scenario2Model.h"
#include <algorithm>

using namespace dbridge::sync;

// ── 配色 ─────────────────────────────────────────────────────────────────────
namespace {
const QColor kDiffBg(0xff, 0xd6, 0xd6);    // 差异（红）
const QColor kAddedBg(0xd6, 0xf5, 0xd6);   // 新增（绿）
const QColor kStagedBg(0xcf, 0xe2, 0xff);  // 已采用A（蓝，预览合并结果）
const QColor kMissBg(0xee, 0xee, 0xee);    // 缺失占位（灰）
const QColor kSameBg(0xff, 0xff, 0xff);    // 相同（白）
const QColor kBonlyBg(0xff, 0xf3, 0xcd);   // 仅B有（淡黄）

const char* kMissText = "—";
}  // namespace

CompareDetailDialog::CompareDetailDialog(Scenario2Model* model, const QString& table,
                                         QWidget* parent)
    : QDialog(parent), model_(model), table_(table) {
    setWindowTitle(
        QStringLiteral("字段级差异比对 — 表「%1」  子节点B ⇄ 中心节点A（Beyond Compare 风格）")
            .arg(table));
    setModal(true);

    columns_ = model_->columns(table_);
    pkColumn_ = model_->pkColumn(table_);

    auto* root = new QVBoxLayout(this);

    // —— 顶部：图例 + 选项 + 状态 ——
    auto* legend = new QLabel(this);
    legend->setTextFormat(Qt::RichText);
    legend->setText(QStringLiteral(
        "图例：<span style='background:#ffd6d6'>&nbsp;红=字段不同&nbsp;</span>&nbsp; "
        "<span style='background:#d6f5d6'>&nbsp;绿=仅中心A有(B需新增)&nbsp;</span>&nbsp; "
        "<span style='background:#fff3cd'>&nbsp;黄=仅子节点B有&nbsp;</span>&nbsp; "
        "<span style='background:#cfe2ff'>&nbsp;蓝=已采用中心A(预览)&nbsp;</span>"));
    root->addWidget(legend);

    auto* optRow = new QHBoxLayout();
    showSame_ = new QCheckBox(QStringLiteral("显示相同行"), this);
    connect(showSame_, &QCheckBox::toggled, this, &CompareDetailDialog::onToggleShowSame);
    optRow->addWidget(showSame_);
    optRow->addStretch(1);
    status_ = new QLabel(this);
    optRow->addWidget(status_);
    root->addLayout(optRow);

    // —— 操作工具条 ——
    auto* toolbar = new QHBoxLayout();
    auto* btnRow = new QPushButton(QStringLiteral("→ 采用中心A · 整行"), this);
    auto* btnCell = new QPushButton(QStringLiteral("→ 采用中心A · 选中字段（列级）"), this);
    auto* btnLocal = new QPushButton(QStringLiteral("← 保留本地B（撤销）"), this);
    btnRow->setToolTip(QStringLiteral("将选中行整体采用中心A 的版本（acceptRemote）"));
    btnCell->setToolTip(QStringLiteral("仅采用选中单元格所在列的中心A 值（stageCell，精确到列）"));
    btnLocal->setToolTip(QStringLiteral("撤销该行的采用决策，保留本地B（acceptLocal）"));
    connect(btnRow, &QPushButton::clicked, this, &CompareDetailDialog::onAcceptRemoteRow);
    connect(btnCell, &QPushButton::clicked, this, &CompareDetailDialog::onAcceptRemoteCell);
    connect(btnLocal, &QPushButton::clicked, this, &CompareDetailDialog::onAcceptLocalRow);
    toolbar->addWidget(btnRow);
    toolbar->addWidget(btnCell);
    toolbar->addWidget(btnLocal);
    toolbar->addStretch(1);
    root->addLayout(toolbar);

    // —— 中部：双栏对比 ——
    auto* splitter = new QSplitter(Qt::Horizontal, this);

    auto makePanel = [&](const QString& title, const QString& color) -> QTableWidget* {
        auto* panel = new QWidget(splitter);
        auto* v = new QVBoxLayout(panel);
        v->setContentsMargins(0, 0, 0, 0);
        auto* lbl = new QLabel(title, panel);
        lbl->setStyleSheet(QStringLiteral("font-weight:bold;color:%1;padding:2px;").arg(color));
        v->addWidget(lbl);
        auto* tbl = new QTableWidget(panel);
        tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);
        tbl->setSelectionMode(QAbstractItemView::SingleSelection);
        tbl->setSelectionBehavior(QAbstractItemView::SelectItems);
        tbl->horizontalHeader()->setStretchLastSection(true);
        v->addWidget(tbl);
        splitter->addWidget(panel);
        return tbl;
    };
    tableB_ = makePanel(QStringLiteral("子节点B（本地 / 当前节点）"), QStringLiteral("#c62828"));
    tableA_ = makePanel(QStringLiteral("中心节点A（远端 / 数据来源）"), QStringLiteral("#1565c0"));
    root->addWidget(splitter, 1);

    // —— 底部：关闭 ——
    auto* bottom = new QHBoxLayout();
    bottom->addStretch(1);
    auto* btnClose = new QPushButton(QStringLiteral("关闭"), this);
    connect(btnClose, &QPushButton::clicked, this, &QDialog::accept);
    bottom->addWidget(btnClose);
    root->addLayout(bottom);

    // —— 左右选择 / 滚动同步 ——
    connect(tableB_, &QTableWidget::currentCellChanged, this,
            &CompareDetailDialog::onLeftCurrentChanged);
    connect(tableA_, &QTableWidget::currentCellChanged, this,
            &CompareDetailDialog::onRightCurrentChanged);
    connect(tableB_->verticalScrollBar(), &QScrollBar::valueChanged, tableA_->verticalScrollBar(),
            &QScrollBar::setValue);
    connect(tableA_->verticalScrollBar(), &QScrollBar::valueChanged, tableB_->verticalScrollBar(),
            &QScrollBar::setValue);

    rebuildRows();
    buildTables();
}

// ── 行集构建 ────────────────────────────────────────────────────────────────

void CompareDetailDialog::rebuildRows() {
    rows_.clear();
    const bool showSame = showSame_ && showSame_->isChecked();
    for (const RowDiff& rd : model_->rowDiffs(table_)) {
        if (rd.kind == RowDiffKind::Same && !showSame)
            continue;
        rows_.append(rd);
    }
    // 主键升序（演示数据主键为整数）。
    std::sort(rows_.begin(), rows_.end(), [](const RowDiff& a, const RowDiff& b) {
        return a.primaryKey.toLongLong() < b.primaryKey.toLongLong();
    });
}

const CellDiff* CompareDetailDialog::findCell(const RowDiff& rd, const QString& column) const {
    for (const CellDiff& c : rd.cells)
        if (c.column == column)
            return &c;
    return nullptr;
}

QString CompareDetailDialog::cellValueString(const RowDiff& rd, const QString& column,
                                             bool remoteSide) const {
    const CellDiff* c = findCell(rd, column);
    if (!c)
        return QString();
    const QVariant& v = remoteSide ? c->remoteValue : c->localValue;
    return v.isNull() ? QString() : v.toString();
}

// ── 表填充 ──────────────────────────────────────────────────────────────────

void CompareDetailDialog::buildTables() {
    for (QTableWidget* t : {tableB_, tableA_}) {
        t->clear();
        t->setColumnCount(columns_.size());
        t->setHorizontalHeaderLabels(columns_);
        t->setRowCount(rows_.size());
    }
    for (int i = 0; i < rows_.size(); ++i)
        populateRow(i);
    refreshStatusLabel();
}

void CompareDetailDialog::populateRow(int idx) {
    if (idx < 0 || idx >= rows_.size())
        return;
    const RowDiff& rd = rows_.at(idx);
    const QString pk = rd.primaryKey;
    const bool rowStaged = model_->isRowStaged(table_, pk);

    // 纵向表头：差异符号 + 主键（已采用则前缀 ✓）。
    const char* sym = rd.kind == RowDiffKind::Added      ? "＋"
                      : rd.kind == RowDiffKind::Deleted  ? "－"
                      : rd.kind == RowDiffKind::Modified ? "≠"
                                                         : "＝";
    const QString head =
        QStringLiteral("%1%2 #%3").arg(rowStaged ? QStringLiteral("✓") : QString(), sym, pk);
    tableB_->setVerticalHeaderItem(idx, new QTableWidgetItem(head));
    tableA_->setVerticalHeaderItem(idx, new QTableWidgetItem(head));

    for (int j = 0; j < columns_.size(); ++j) {
        const QString col = columns_.at(j);
        const CellDiff* cd = findCell(rd, col);
        const bool changed = cd && cd->changed;
        const bool cellStaged = model_->isCellStaged(table_, pk, col);
        const QString localStr = cellValueString(rd, col, /*remoteSide=*/false);
        const QString remoteStr = cellValueString(rd, col, /*remoteSide=*/true);

        // 右栏（中心A）。
        auto* aItem = new QTableWidgetItem();
        if (rd.kind == RowDiffKind::Deleted) {
            aItem->setText(QString::fromUtf8(kMissText));
            aItem->setForeground(QBrush(QColor(0x99, 0x99, 0x99)));
            aItem->setBackground(kMissBg);
        } else {
            aItem->setText(remoteStr);
            if (rd.kind == RowDiffKind::Added)
                aItem->setBackground(cellStaged ? kStagedBg : kAddedBg);
            else if (changed)
                aItem->setBackground(cellStaged ? kStagedBg : kDiffBg);
            else
                aItem->setBackground(kSameBg);
        }
        tableA_->setItem(idx, j, aItem);

        // 左栏（子节点B）。已采用的字段预览为中心A 值（蓝）。
        auto* bItem = new QTableWidgetItem();
        if (rd.kind == RowDiffKind::Added) {
            bItem->setText(cellStaged ? remoteStr : QString::fromUtf8(kMissText));
            bItem->setBackground(cellStaged ? kStagedBg : kMissBg);
            if (!cellStaged)
                bItem->setForeground(QBrush(QColor(0x99, 0x99, 0x99)));
        } else if (changed) {
            bItem->setText(cellStaged ? remoteStr : localStr);
            bItem->setBackground(cellStaged ? kStagedBg : kDiffBg);
        } else {
            bItem->setText(localStr);
            bItem->setBackground(rd.kind == RowDiffKind::Deleted ? kBonlyBg : kSameBg);
        }
        if (cellStaged) {
            QFont f = bItem->font();
            f.setBold(true);
            bItem->setFont(f);
        }
        tableB_->setItem(idx, j, bItem);
    }
}

void CompareDetailDialog::refreshStatusLabel() {
    int diffRows = 0;
    for (const RowDiff& rd : rows_)
        if (rd.kind != RowDiffKind::Same)
            ++diffRows;
    status_->setText(QStringLiteral("差异行：%1  ·  本表已暂存采用：%2 行  ·  全部待保存：%3 行")
                         .arg(diffRows)
                         .arg([&] {
                             int n = 0;
                             for (const RowDiff& rd : rows_)
                                 if (model_->isRowStaged(table_, rd.primaryKey))
                                     ++n;
                             return n;
                         }())
                         .arg(model_->pendingCount()));
}

// ── 选择 / 滚动同步 ──────────────────────────────────────────────────────────

void CompareDetailDialog::onLeftCurrentChanged(int row, int col, int, int) {
    if (syncGuard_)
        return;
    syncGuard_ = true;
    if (row >= 0 && col >= 0)
        tableA_->setCurrentCell(row, col);
    syncGuard_ = false;
}

void CompareDetailDialog::onRightCurrentChanged(int row, int col, int, int) {
    if (syncGuard_)
        return;
    syncGuard_ = true;
    if (row >= 0 && col >= 0)
        tableB_->setCurrentCell(row, col);
    syncGuard_ = false;
}

int CompareDetailDialog::currentRowIndex() const {
    int r = tableA_->currentRow();
    if (r < 0)
        r = tableB_->currentRow();
    return r;
}

// ── 操作 ────────────────────────────────────────────────────────────────────

void CompareDetailDialog::onAcceptRemoteRow() {
    const int idx = currentRowIndex();
    if (idx < 0 || idx >= rows_.size()) {
        QMessageBox::information(this, windowTitle(), QStringLiteral("请先选中一行。"));
        return;
    }
    const RowDiff& rd = rows_.at(idx);
    if (rd.kind == RowDiffKind::Deleted) {
        QMessageBox::information(
            this, windowTitle(),
            QStringLiteral("该行仅子节点B 存在、中心A 没有；本演示的同步方向为 A→B，"
                           "不会删除B 的本地独有行。可点「保留本地B」。"));
        return;
    }
    if (rd.kind == RowDiffKind::Same) {
        QMessageBox::information(this, windowTitle(), QStringLiteral("该行两端相同，无需采用。"));
        return;
    }
    model_->acceptRemoteRow(table_, rd.primaryKey);
    populateRow(idx);
    refreshStatusLabel();
    emit stagingChanged();
}

void CompareDetailDialog::onAcceptRemoteCell() {
    const int idx = currentRowIndex();
    if (idx < 0 || idx >= rows_.size()) {
        QMessageBox::information(this, windowTitle(), QStringLiteral("请先选中一个单元格。"));
        return;
    }
    int col = tableA_->currentColumn();
    if (col < 0)
        col = tableB_->currentColumn();
    if (col < 0 || col >= columns_.size()) {
        QMessageBox::information(this, windowTitle(),
                                 QStringLiteral("请先选中一个字段（单元格）。"));
        return;
    }
    const RowDiff& rd = rows_.at(idx);
    const QString column = columns_.at(col);

    if (rd.kind == RowDiffKind::Added) {
        QMessageBox::information(this, windowTitle(),
                                 QStringLiteral("新增行（仅中心A 有）请使用「整行采用」。"));
        return;
    }
    if (rd.kind == RowDiffKind::Deleted || rd.kind == RowDiffKind::Same) {
        QMessageBox::information(this, windowTitle(),
                                 QStringLiteral("该字段无可采用的中心A 差异值。"));
        return;
    }
    const CellDiff* cd = findCell(rd, column);
    if (!cd || !cd->changed) {
        QMessageBox::information(this, windowTitle(),
                                 QStringLiteral("字段「%1」两端相同，无需采用。").arg(column));
        return;
    }
    model_->stageCellValue(table_, rd.primaryKey, column, cd->remoteValue);
    populateRow(idx);
    refreshStatusLabel();
    emit stagingChanged();
}

void CompareDetailDialog::onAcceptLocalRow() {
    const int idx = currentRowIndex();
    if (idx < 0 || idx >= rows_.size())
        return;
    const RowDiff& rd = rows_.at(idx);
    model_->acceptLocalRow(table_, rd.primaryKey);
    populateRow(idx);
    refreshStatusLabel();
    emit stagingChanged();
}

void CompareDetailDialog::onToggleShowSame(bool) {
    rebuildRows();
    buildTables();
}
