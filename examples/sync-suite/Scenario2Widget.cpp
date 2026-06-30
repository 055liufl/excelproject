#include "Scenario2Widget.h"

#include <QBrush>
#include <QColor>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include "CompareDetailDialog.h"

// ============================================================================
// Scenario2Widget.cpp — 场景2 顶层界面实现（表对比清单 + 保存/取消/重置）
// ============================================================================
//
// 本类是一层纯「视图 + 交互」：全部业务逻辑都委托给 Scenario2Model（model_），自身只负责
//   ① 搭出控件树（说明文字 + 表对比清单 + 底部操作条）；
//   ② 把模型的差异状态渲染成红/绿表格（refreshTable）；
//   ③ 把按钮/双击事件转成对 model_ 的调用，并据结果弹框提示。
// 线程：纯主线程 GUI；不直接碰数据库/同步引擎（那些都在 model_ 内部）。
// ============================================================================

// 构造 —— 搭建界面并初始化模型。
// 步骤：建布局 → 说明 RichText → 三列「表对比清单」表格（B表名/A表名/差异属性）→ 操作条
//   （重置/状态标签/取消/保存）→ 调 model_->setup() 真正建库与比对会话；setup 失败则弹错并把
//   失败原因追加到说明区，但仍构造出界面（ready_=false 时各操作会被禁用/短路）。
// 末尾 refreshTable() 首次渲染。
Scenario2Widget::Scenario2Widget(const QString& ws, QWidget* parent)
    : QWidget(parent), ws_(ws), model_(std::make_unique<Scenario2Model>(ws)) {
    auto* root = new QVBoxLayout(this);

    // —— 说明 ——
    auto* desc = new QLabel(this);
    desc->setTextFormat(Qt::RichText);
    desc->setWordWrap(true);
    desc->setText(QStringLiteral(
        "<b>场景2 · 同步前差异比对（类 Beyond Compare）</b><br/>"
        "自动比较「子节点B（当前节点）」与「中心节点A」两个 SQLite 库中各表的数据差异。"
        "<b>双击</b>任意一行进入字段级对比窗口，可<i>精确到列</i>采用中心A 的数据；"
        "点「保存」把内存中的合并决策写回 B 库（A→B 同步），点「取消」放弃暂存。"));
    root->addWidget(desc);

    // —— 表对比清单 ——
    tableList_ = new QTableWidget(this);
    tableList_->setColumnCount(3);
    tableList_->setHorizontalHeaderLabels({QStringLiteral("子节点B 表名"),
                                           QStringLiteral("中心节点A 表名"),
                                           QStringLiteral("差异属性")});
    tableList_->horizontalHeader()->setStretchLastSection(true);
    tableList_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    tableList_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    tableList_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tableList_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableList_->setSelectionMode(QAbstractItemView::SingleSelection);
    tableList_->verticalHeader()->setVisible(false);
    connect(tableList_, &QTableWidget::cellDoubleClicked, this,
            &Scenario2Widget::onRowDoubleClicked);
    root->addWidget(tableList_, 1);

    // —— 操作条 ——
    auto* bar = new QHBoxLayout();
    btnReset_ = new QPushButton(QStringLiteral("重置演示数据"), this);
    btnReset_->setToolTip(QStringLiteral("把 B / A 两库恢复为初始的差异状态，便于反复演示"));
    connect(btnReset_, &QPushButton::clicked, this, &Scenario2Widget::onReset);
    bar->addWidget(btnReset_);
    bar->addStretch(1);
    status_ = new QLabel(this);
    bar->addWidget(status_);
    bar->addSpacing(12);
    btnCancel_ = new QPushButton(QStringLiteral("取消"), this);
    btnCancel_->setToolTip(QStringLiteral("放弃所有内存暂存（discard）"));
    connect(btnCancel_, &QPushButton::clicked, this, &Scenario2Widget::onCancel);
    bar->addWidget(btnCancel_);
    btnSave_ = new QPushButton(QStringLiteral("保存"), this);
    btnSave_->setToolTip(QStringLiteral("将内存中的合并决策写回子节点B 数据库（save）"));
    btnSave_->setStyleSheet(QStringLiteral("font-weight:bold;"));
    connect(btnSave_, &QPushButton::clicked, this, &Scenario2Widget::onSave);
    bar->addWidget(btnSave_);
    root->addLayout(bar);

    // —— 初始化 model ——
    QString err;
    ready_ = model_->setup(&err);
    if (!ready_) {
        QMessageBox::critical(this, QStringLiteral("场景2 初始化失败"),
                              QStringLiteral("无法建立比对会话：\n%1").arg(err));
        desc->setText(
            desc->text() +
            QStringLiteral("<br/><span style='color:#c62828'>初始化失败：%1</span>").arg(err));
    }
    refreshTable();
}

// refreshTable —— 依据 model_ 的最新差异重绘清单与按钮态（每次决策/保存/重置后都会调）。
// 做什么：逐表填三列；差异属性列按 identical 着色——相同=绿底、不同=红底并附 ≠修改 ＋新增 －删除
//   计数。底部状态标签显示「待保存行数」，并据 pendingCount 启停「保存/取消」按钮。
// 注意 statusOf 是个就地小工具：把 tableStatuses() 列表查成「按表名取状态」，找不到则返回空状态。
void Scenario2Widget::refreshTable() {
    const QStringList tables = model_->tables();
    tableList_->setRowCount(tables.size());

    const auto statuses = ready_ ? model_->tableStatuses() : QList<Scenario2Model::TableStatus>{};
    auto statusOf = [&](const QString& t) -> Scenario2Model::TableStatus {
        for (const auto& s : statuses)
            if (s.table == t)
                return s;
        Scenario2Model::TableStatus empty;
        empty.table = t;
        return empty;
    };

    for (int i = 0; i < tables.size(); ++i) {
        const QString t = tables.at(i);
        const auto st = statusOf(t);

        auto* bItem = new QTableWidgetItem(t);
        auto* aItem = new QTableWidgetItem(t);
        tableList_->setItem(i, 0, bItem);
        tableList_->setItem(i, 1, aItem);

        auto* diffItem = new QTableWidgetItem();
        if (st.identical) {
            diffItem->setText(QStringLiteral("● 相同"));
            diffItem->setBackground(QBrush(QColor(0xd6, 0xf5, 0xd6)));  // 绿
            diffItem->setForeground(QBrush(QColor(0x1b, 0x5e, 0x20)));
        } else {
            diffItem->setText(QStringLiteral("● 不同   ≠%1  ＋%2  －%3")
                                  .arg(st.modified)
                                  .arg(st.added)
                                  .arg(st.deleted));
            diffItem->setBackground(QBrush(QColor(0xff, 0xd6, 0xd6)));  // 红
            diffItem->setForeground(QBrush(QColor(0xb7, 0x1c, 0x1c)));
        }
        tableList_->setItem(i, 2, diffItem);
    }

    const int pending = ready_ ? model_->pendingCount() : 0;
    status_->setText(QStringLiteral("待保存（已采用中心A）：%1 行").arg(pending));
    btnSave_->setEnabled(ready_ && pending > 0);
    btnCancel_->setEnabled(ready_ && pending > 0);
}

// onRowDoubleClicked —— 双击某表行 → 打开该表的字段级对比对话框（CompareDetailDialog）。
// 把 model_ 指针与表名传给对话框；订阅它的 stagingChanged 信号以便用户在对话框里做决策时
//   实时刷新主清单；exec() 模态阻塞，关闭后再 refreshTable 一次确保同步最终状态。
void Scenario2Widget::onRowDoubleClicked(int row, int /*column*/) {
    if (!ready_)
        return;
    const QStringList tables = model_->tables();
    if (row < 0 || row >= tables.size())
        return;
    const QString table = tables.at(row);

    CompareDetailDialog dlg(model_.get(), table, this);
    connect(&dlg, &CompareDetailDialog::stagingChanged, this, &Scenario2Widget::refreshTable);
    dlg.exec();
    refreshTable();
}

// onSave —— 「保存」按钮：把内存暂存的合并决策写回 B 库。
// 先记下 pending 行数（save 成功后会清零，故须提前取以便提示）；失败弹错框、成功弹信息框，
//   两种情况都 refreshTable（成功后差异通常大幅减少）。
void Scenario2Widget::onSave() {
    if (!ready_)
        return;
    const int pending = model_->pendingCount();
    QString err;
    if (!model_->save(&err)) {
        QMessageBox::critical(this, QStringLiteral("保存失败"),
                              QStringLiteral("写回子节点B 数据库失败：\n%1").arg(err));
        return;
    }
    QMessageBox::information(
        this, QStringLiteral("保存成功"),
        QStringLiteral("已将 %1 行「采用中心A」的合并决策写回子节点B 数据库。\n"
                       "差异清单已刷新。")
            .arg(pending));
    refreshTable();
}

// onCancel —— 「取消」按钮：放弃所有内存暂存（discard），回到全部保留本地，刷新界面。
void Scenario2Widget::onCancel() {
    if (!ready_)
        return;
    model_->discard();
    refreshTable();
}

// onReset —— 「重置演示数据」按钮：reseed 把 A/B 两库恢复到初始差异态，便于反复演示。
// reseed 内部即整套 setup 重跑，成功后更新 ready_ 并刷新清单；失败弹错框。
void Scenario2Widget::onReset() {
    QString err;
    ready_ = model_->reseed(&err);
    if (!ready_) {
        QMessageBox::critical(this, QStringLiteral("重置失败"), err);
        return;
    }
    QMessageBox::information(this, QStringLiteral("已重置"),
                             QStringLiteral("B / A 两库已恢复为初始差异状态。"));
    refreshTable();
}
