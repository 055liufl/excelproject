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

// ============================================================================
// CompareDetailDialog.cpp — 字段级对比对话框的实现
// ============================================================================
//
// 【本文件做什么】实现 CompareDetailDialog：构造时搭建「图例 + 选项 + 工具条 + 双栏表格
//   + 底部按钮」的整套 UI，并把模型里的行级/单元格级 diff 渲染到左右两栏；响应用户的
//   「采用A 整行 / 采用A 单列 / 保留本地B」操作，把决策写进 Scenario2Model 的内存暂存，
//   再以颜色高亮与差异符号实时预览合并效果。
//
// 【渲染约定（贯穿 populateRow）】
//   · 右栏(A)=远端来源；左栏(B)=本地。两栏按同一 rows_ 顺序逐行对齐。
//   · 颜色编码即下方 6 个常量：红=字段不同、绿=仅A有(B需新增)、黄=仅B有、蓝=已采用A
//     的预览、灰=缺失占位、白=相同。已采用的字段在左栏会「预览」为 A 的值并加粗。
//   · 行差异类型 RowDiffKind：Added(仅A有) / Deleted(仅B有) / Modified(两端不同) / Same。
//     注意 Deleted 在本演示语义里表示「仅子节点B 有、A 没有」——因演示同步方向是 A→B，
//     不会删除 B 的本地独有行（见 onAcceptRemoteRow 的提示）。
//
// 【数据写入边界】本对话框只调用 model_ 的 acceptRemoteRow / stageCellValue / acceptLocalRow
//   等改「内存暂存」的方法，绝不直接落库；真正落库由父窗口的保存按钮触发 model->save()。
// ============================================================================

using namespace dbridge::sync;

// ── 配色 ─────────────────────────────────────────────────────────────────────
// 六个差异语义对应的背景色（RGB 十六进制）。populateRow 按行/单元格状态择一应用。
namespace {
const QColor kDiffBg(0xff, 0xd6, 0xd6);    // 差异（红）：该列两端值不同
const QColor kAddedBg(0xd6, 0xf5, 0xd6);   // 新增（绿）：仅中心A 有，B 需新增
const QColor kStagedBg(0xcf, 0xe2, 0xff);  // 已采用A（蓝，预览合并结果）
const QColor kMissBg(0xee, 0xee, 0xee);  // 缺失占位（灰）：该侧本来就没有这一行/格
const QColor kSameBg(0xff, 0xff, 0xff);   // 相同（白）：两端一致
const QColor kBonlyBg(0xff, 0xf3, 0xcd);  // 仅B有（淡黄）

const char* kMissText = "—";  // 缺失单元格的占位文本（破折号）
}  // namespace

// ── 构造：搭建整套 UI 并首次渲染差异 ─────────────────────────────────────────
// 流程：设标题/模态 → 从模型取该表列名与主键 → 自上而下铺设五段布局
//   （图例 / 选项行 / 工具条 / 双栏对比 / 底部关闭）→ 连接选择与滚动联动 →
//   rebuildRows() 拉取差异行集 → buildTables() 填充两栏。
CompareDetailDialog::CompareDetailDialog(Scenario2Model* model, const QString& table,
                                         QWidget* parent)
    : QDialog(parent), model_(model), table_(table) {
    setWindowTitle(
        QStringLiteral("字段级差异比对 — 表「%1」  子节点B ⇄ 中心节点A（Beyond Compare 风格）")
            .arg(table));
    setModal(true);

    columns_ = model_->columns(table_);    // 该表全部列（含主键），决定两栏列序
    pkColumn_ = model_->pkColumn(table_);  // 主键列名（用于行排序/定位）

    auto* root = new QVBoxLayout(this);  // 自上而下的根布局

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
    // 用可拖拽分隔的 QSplitter 横向放置左右两个面板，用户可调左右宽度比。
    auto* splitter = new QSplitter(Qt::Horizontal, this);

    // 工厂 lambda：造一个「带标题标签 + 只读单选表格」的面板并加入 splitter，返回其表格。
    // 表格设为：不可编辑(NoEditTriggers，纯展示)、单选(SingleSelection)、按单元格选择
    //   (SelectItems，因为「采用单列」需要精确到单元格)、最后一列自动拉伸填满。
    auto makePanel = [&](const QString& title, const QString& color) -> QTableWidget* {
        auto* panel = new QWidget(splitter);
        auto* v = new QVBoxLayout(panel);
        v->setContentsMargins(0, 0, 0, 0);
        auto* lbl = new QLabel(title, panel);
        lbl->setStyleSheet(QStringLiteral("font-weight:bold;color:%1;padding:2px;").arg(color));
        v->addWidget(lbl);
        auto* tbl = new QTableWidget(panel);
        tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);    // 只读：禁止直接编辑
        tbl->setSelectionMode(QAbstractItemView::SingleSelection);  // 单选
        tbl->setSelectionBehavior(QAbstractItemView::SelectItems);  // 选单元格（支持列级采用）
        tbl->horizontalHeader()->setStretchLastSection(true);
        v->addWidget(tbl);
        splitter->addWidget(panel);
        return tbl;
    };
    // 左栏 B 用红色标题（本地）、右栏 A 用蓝色标题（远端来源）——与图例配色呼应。
    tableB_ = makePanel(QStringLiteral("子节点B（本地 / 当前节点）"), QStringLiteral("#c62828"));
    tableA_ = makePanel(QStringLiteral("中心节点A（远端 / 数据来源）"), QStringLiteral("#1565c0"));
    root->addWidget(splitter, 1);  // 拉伸因子 1：双栏区占据剩余全部高度

    // —— 底部：关闭 ——
    auto* bottom = new QHBoxLayout();
    bottom->addStretch(1);
    auto* btnClose = new QPushButton(QStringLiteral("关闭"), this);
    connect(btnClose, &QPushButton::clicked, this, &QDialog::accept);
    bottom->addWidget(btnClose);
    root->addLayout(bottom);

    // —— 左右选择 / 滚动同步 ——
    // 选择联动：一侧当前单元格变化 → 槽里把另一侧也定位到相同 (row,col)（受 syncGuard_ 防递归）。
    connect(tableB_, &QTableWidget::currentCellChanged, this,
            &CompareDetailDialog::onLeftCurrentChanged);
    connect(tableA_, &QTableWidget::currentCellChanged, this,
            &CompareDetailDialog::onRightCurrentChanged);
    // 垂直滚动联动：两栏滚动条互相绑定 setValue，使左右始终对齐到同一行（典型对比视图体验）。
    connect(tableB_->verticalScrollBar(), &QScrollBar::valueChanged, tableA_->verticalScrollBar(),
            &QScrollBar::setValue);
    connect(tableA_->verticalScrollBar(), &QScrollBar::valueChanged, tableB_->verticalScrollBar(),
            &QScrollBar::setValue);

    rebuildRows();  // 首次拉取差异行集
    buildTables();  // 首次填充两栏
}

// ── 行集构建 ────────────────────────────────────────────────────────────────

// rebuildRows —— 从模型拉取该表的行级 diff，按「是否显示相同行」过滤，再按主键升序排。
// 做什么：清空 rows_，遍历 model_->rowDiffs(table_)，当未勾选「显示相同行」时跳过 Same
//   行（只看有差异的），其余追加；最后按主键数值升序排序。
// 为什么按主键整数排序：本演示数据的主键是整数字符串，toLongLong() 后按数值排比按字符串
//   字典序更符合直觉（"10" 不会排在 "2" 前面）。
// 何时调用：构造时、以及切换「显示相同行」复选框时（onToggleShowSame）。
void CompareDetailDialog::rebuildRows() {
    rows_.clear();
    const bool showSame = showSame_ && showSame_->isChecked();
    for (const RowDiff& rd : model_->rowDiffs(table_)) {
        if (rd.kind == RowDiffKind::Same && !showSame)
            continue;  // 未勾选「显示相同行」时，相同行不进入展示集
        rows_.append(rd);
    }
    // 主键升序（演示数据主键为整数）。
    std::sort(rows_.begin(), rows_.end(), [](const RowDiff& a, const RowDiff& b) {
        return a.primaryKey.toLongLong() < b.primaryKey.toLongLong();
    });
}

// findCell —— 在一行的 cells 列表里按列名线性查找其 CellDiff。
// 返回：命中返回该 CellDiff 的地址；无此列返回 nullptr。复杂度 O(列数)（列数小，无碍）。
const CellDiff* CompareDetailDialog::findCell(const RowDiff& rd, const QString& column) const {
    for (const CellDiff& c : rd.cells)
        if (c.column == column)
            return &c;
    return nullptr;
}

// cellValueString —— 取某行某列在指定一侧（remoteSide=true→A 远端 / false→B 本地）的显示串。
// 规则：该列无 CellDiff → 空串；值为 QVariant null → 空串；否则 toString()。
QString CompareDetailDialog::cellValueString(const RowDiff& rd, const QString& column,
                                             bool remoteSide) const {
    const CellDiff* c = findCell(rd, column);
    if (!c)
        return QString();  // 该列不在差异结构里 → 无可显示值
    const QVariant& v = remoteSide ? c->remoteValue : c->localValue;  // 按侧取远端/本地值
    return v.isNull() ? QString() : v.toString();
}

// ── 表填充 ──────────────────────────────────────────────────────────────────

// buildTables —— 依据当前 rows_ 重置左右两栏的列/行规模，并逐行渲染、刷新状态栏。
// 注意：对两栏统一 clear→setColumnCount→setHorizontalHeaderLabels→setRowCount，保证
//   左右结构完全一致（同列同行数），是后续逐行对齐渲染的前提。
void CompareDetailDialog::buildTables() {
    for (QTableWidget* t : {tableB_, tableA_}) {
        t->clear();
        t->setColumnCount(columns_.size());
        t->setHorizontalHeaderLabels(columns_);
        t->setRowCount(rows_.size());
    }
    for (int i = 0; i < rows_.size(); ++i)
        populateRow(i);    // 逐行填充两栏单元格 + 高亮
    refreshStatusLabel();  // 更新底部统计
}

// populateRow —— 渲染第 idx 行：左右两栏的全部单元格、差异符号、高亮与暂存预览。
// 这是本类视觉逻辑最密集的函数：对每一列，依「行差异类型 + 该列是否变化 + 该格/该行是否
//   已被采用」决定填什么文本、上什么背景色。
void CompareDetailDialog::populateRow(int idx) {
    if (idx < 0 || idx >= rows_.size())
        return;  // 越界保护
    const RowDiff& rd = rows_.at(idx);
    const QString pk = rd.primaryKey;
    const bool rowStaged = model_->isRowStaged(table_, pk);  // 该行是否已被「整行采用A」

    // 纵向表头：差异符号 + 主键（已采用则前缀 ✓）。
    // 符号约定：＋=新增(仅A有) / －=删除(仅B有) / ≠=修改(两端不同) / ＝=相同。
    const char* sym = rd.kind == RowDiffKind::Added      ? "＋"
                      : rd.kind == RowDiffKind::Deleted  ? "－"
                      : rd.kind == RowDiffKind::Modified ? "≠"
                                                         : "＝";
    // 形如 "✓≠ #42"：可选的 ✓(已采用) + 差异符 + "#主键"。左右两栏共用同一行头。
    const QString head =
        QStringLiteral("%1%2 #%3").arg(rowStaged ? QStringLiteral("✓") : QString(), sym, pk);
    tableB_->setVerticalHeaderItem(idx, new QTableWidgetItem(head));
    tableA_->setVerticalHeaderItem(idx, new QTableWidgetItem(head));

    // 逐列渲染。changed=该列两端不同；cellStaged=该列已被「列级采用A」。
    for (int j = 0; j < columns_.size(); ++j) {
        const QString col = columns_.at(j);
        const CellDiff* cd = findCell(rd, col);
        const bool changed = cd && cd->changed;                         // 该列是否有差异
        const bool cellStaged = model_->isCellStaged(table_, pk, col);  // 该列是否已采用A
        const QString localStr = cellValueString(rd, col, /*remoteSide=*/false);  // B 值
        const QString remoteStr = cellValueString(rd, col, /*remoteSide=*/true);  // A 值

        // 右栏（中心A）：始终显示 A 的真实值；颜色按行/列状态而定。
        auto* aItem = new QTableWidgetItem();
        if (rd.kind == RowDiffKind::Deleted) {
            // 该行仅 B 有、A 没有 → A 侧显示灰色占位「—」。
            aItem->setText(QString::fromUtf8(kMissText));
            aItem->setForeground(QBrush(QColor(0x99, 0x99, 0x99)));
            aItem->setBackground(kMissBg);
        } else {
            aItem->setText(remoteStr);
            if (rd.kind == RowDiffKind::Added)
                aItem->setBackground(cellStaged ? kStagedBg : kAddedBg);  // 新增行：绿/已采用蓝
            else if (changed)
                aItem->setBackground(cellStaged ? kStagedBg : kDiffBg);  // 差异列：红/已采用蓝
            else
                aItem->setBackground(kSameBg);  // 相同列：白
        }
        tableA_->setItem(idx, j, aItem);

        // 左栏（子节点B）。已采用的字段预览为中心A 值（蓝），即「合并后会变成什么」的预演。
        auto* bItem = new QTableWidgetItem();
        if (rd.kind == RowDiffKind::Added) {
            // 新增行(仅A有)：B 本来没有这一行。若已采用则预览为 A 值(蓝)，否则灰占位「—」。
            bItem->setText(cellStaged ? remoteStr : QString::fromUtf8(kMissText));
            bItem->setBackground(cellStaged ? kStagedBg : kMissBg);
            if (!cellStaged)
                bItem->setForeground(QBrush(QColor(0x99, 0x99, 0x99)));  // 未采用时文字置灰
        } else if (changed) {
            // 差异列：已采用 → 预览 A 值(蓝)；未采用 → 显示 B 原值(红)。
            bItem->setText(cellStaged ? remoteStr : localStr);
            bItem->setBackground(cellStaged ? kStagedBg : kDiffBg);
        } else {
            // 相同列：显示 B 值。若整行是「仅B有(Deleted)」则用淡黄背景标识本地独有行。
            bItem->setText(localStr);
            bItem->setBackground(rd.kind == RowDiffKind::Deleted ? kBonlyBg : kSameBg);
        }
        if (cellStaged) {
            // 已采用的字段在左栏加粗，进一步强调「这格将被改为 A 的值」。
            QFont f = bItem->font();
            f.setBold(true);
            bItem->setFont(f);
        }
        tableB_->setItem(idx, j, bItem);
    }
}

// refreshStatusLabel —— 刷新底部状态栏：当前展示的差异行数 / 本表已采用行数 / 全部待保存行数。
// 三个数字：① 当前 rows_ 里非 Same 的行数；② 当前 rows_ 里已被整行采用的行数（用就地
//   lambda 现算）；③ 模型层全表跨所有表的待保存总数 pendingCount()（供用户掌握全局进度）。
void CompareDetailDialog::refreshStatusLabel() {
    int diffRows = 0;
    for (const RowDiff& rd : rows_)
        if (rd.kind != RowDiffKind::Same)
            ++diffRows;  // 统计有差异的行
    status_->setText(QStringLiteral("差异行：%1  ·  本表已暂存采用：%2 行  ·  全部待保存：%3 行")
                         .arg(diffRows)
                         .arg([&] {  // 立即调用的 lambda：数当前行集中「已整行采用」的行数
                             int n = 0;
                             for (const RowDiff& rd : rows_)
                                 if (model_->isRowStaged(table_, rd.primaryKey))
                                     ++n;
                             return n;
                         }())
                         .arg(model_->pendingCount()));  // 全局待保存计数（含其它表）
}

// ── 选择 / 滚动同步 ──────────────────────────────────────────────────────────

// onLeftCurrentChanged —— 左栏(B)选择变化时，把右栏(A)同步定位到相同 (row,col)。
// syncGuard_ 防递归：setCurrentCell 会触发 A 的 onRightCurrentChanged，后者又要回设 B……
//   置位 guard 后对方槽会直接 return，从而打破「左设右→右设左→……」的无限互相触发。
void CompareDetailDialog::onLeftCurrentChanged(int row, int col, int, int) {
    if (syncGuard_)
        return;  // 正处于一次联动中：不再反向触发
    syncGuard_ = true;
    if (row >= 0 && col >= 0)
        tableA_->setCurrentCell(row, col);  // 镜像到另一栏
    syncGuard_ = false;
}

// onRightCurrentChanged —— 右栏(A)选择变化时，把左栏(B)同步定位（与上对称，同样防递归）。
void CompareDetailDialog::onRightCurrentChanged(int row, int col, int, int) {
    if (syncGuard_)
        return;
    syncGuard_ = true;
    if (row >= 0 && col >= 0)
        tableB_->setCurrentCell(row, col);
    syncGuard_ = false;
}

// currentRowIndex —— 返回「当前选中行」的下标：优先看右栏A，A 无选中再退回左栏B；都无则 -1。
// 因左右栏选择被联动保持一致，二者通常相同；此处兜底处理「只有一侧有选中」的情形。
int CompareDetailDialog::currentRowIndex() const {
    int r = tableA_->currentRow();
    if (r < 0)
        r = tableB_->currentRow();
    return r;
}

// ── 操作 ────────────────────────────────────────────────────────────────────

// onAcceptRemoteRow —— 「→ 采用中心A · 整行」：把当前行整体采纳为 A 版本（写入暂存）。
// 守卫顺序：未选中行 → 提示；Deleted(仅B有) → 提示「A→B 方向不删 B 独有行」并拒绝；
//   Same(两端相同) → 提示「无需采用」。仅 Added/Modified 行可整行采用。
// 成功后：写模型暂存 → 重渲染该行(预览合并) → 刷新状态栏 → 发 stagingChanged 通知父窗口。
void CompareDetailDialog::onAcceptRemoteRow() {
    const int idx = currentRowIndex();
    if (idx < 0 || idx >= rows_.size()) {
        QMessageBox::information(this, windowTitle(), QStringLiteral("请先选中一行。"));
        return;
    }
    const RowDiff& rd = rows_.at(idx);
    if (rd.kind == RowDiffKind::Deleted) {
        // 仅 B 有的行：本演示同步方向 A→B，不会反向删 B 的本地独有行 → 引导用户「保留本地B」。
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
    model_->acceptRemoteRow(table_, rd.primaryKey);  // 写内存暂存：整行采用 A
    populateRow(idx);       // 重绘该行（左栏预览为 A 值、行头加 ✓）
    refreshStatusLabel();   // 更新统计
    emit stagingChanged();  // 通知父窗口刷新清单红绿/计数
}

// onAcceptRemoteCell —— 「→ 采用中心A · 选中字段(列级)」：仅把当前单元格所在列采纳为 A 值。
// 比整行采用更细的粒度。守卫：未选行/列 → 提示；Added 行 → 引导用整行采用（新增行无单列
//   概念）；Deleted/Same 行 → 无可采用差异；该列未变化(changed=false) → 提示两端相同。
// 取列号时优先用右栏A 的当前列，A 无则退回左栏B（与 currentRowIndex 同思路）。
void CompareDetailDialog::onAcceptRemoteCell() {
    const int idx = currentRowIndex();
    if (idx < 0 || idx >= rows_.size()) {
        QMessageBox::information(this, windowTitle(), QStringLiteral("请先选中一个单元格。"));
        return;
    }
    int col = tableA_->currentColumn();
    if (col < 0)
        col = tableB_->currentColumn();  // A 侧无选中 → 退回 B 侧的当前列
    if (col < 0 || col >= columns_.size()) {
        QMessageBox::information(this, windowTitle(),
                                 QStringLiteral("请先选中一个字段（单元格）。"));
        return;
    }
    const RowDiff& rd = rows_.at(idx);
    const QString column = columns_.at(col);

    if (rd.kind == RowDiffKind::Added) {
        // 新增行(仅A有)：B 整行都缺，无法只采一列 → 引导整行采用。
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
        // 该列两端本就相同 → 没有 A 值需要采用。
        QMessageBox::information(this, windowTitle(),
                                 QStringLiteral("字段「%1」两端相同，无需采用。").arg(column));
        return;
    }
    model_->stageCellValue(table_, rd.primaryKey, column, cd->remoteValue);  // 暂存该列的 A 值
    populateRow(idx);
    refreshStatusLabel();
    emit stagingChanged();
}

// onAcceptLocalRow —— 「← 保留本地B(撤销)」：撤销该行的所有采用决策，回到保留 B 本地值。
// 无差异守卫提示（容许对任意选中行执行撤销，无害）；仅在未选中行时静默返回。
void CompareDetailDialog::onAcceptLocalRow() {
    const int idx = currentRowIndex();
    if (idx < 0 || idx >= rows_.size())
        return;
    const RowDiff& rd = rows_.at(idx);
    model_->acceptLocalRow(table_, rd.primaryKey);  // 撤销该行采用，恢复保留本地B
    populateRow(idx);
    refreshStatusLabel();
    emit stagingChanged();
}

// onToggleShowSame —— 「显示相同行」复选框切换：按新选项重建行集并重填两栏。
// 参数被忽略（用 showSame_->isChecked() 在 rebuildRows 内读取最新勾选态）。
void CompareDetailDialog::onToggleShowSame(bool) {
    rebuildRows();
    buildTables();
}
