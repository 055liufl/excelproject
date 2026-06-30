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

void Scenario2Widget::onCancel() {
    if (!ready_)
        return;
    model_->discard();
    refreshTable();
}

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
