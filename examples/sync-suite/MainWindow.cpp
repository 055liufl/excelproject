#include "MainWindow.h"

#include <QLabel>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

#include "Scenario1Widget.h"
#include "Scenario2Widget.h"

MainWindow::MainWindow(const QString& ws, QWidget* parent) : QMainWindow(parent), ws_(ws) {
    setWindowTitle(
        QStringLiteral("数据库同步设计2 — 场景演示（场景1 多节点同步 / 场景2 差异比对 GUI）"));

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(8, 8, 8, 8);

    // 顶部说明条：概述两场景，呼应设计图。
    auto* header = new QLabel(central);
    header->setTextFormat(Qt::RichText);
    header->setWordWrap(true);
    header->setText(QStringLiteral(
        "<b>《数据库同步设计2》演示</b> &nbsp;—&nbsp; "
        "<span style='color:#1565c0'>场景1</span>：中心节点A ⇄ 子节点B/C/D + 指定数据库，"
        "真实 UDP 多节点同步，<i>以指定数据为准</i>，全域收敛；&nbsp;&nbsp;"
        "<span style='color:#c62828'>场景2</span>：类 Beyond Compare 的「子节点B ⇄ 中心节点A」"
        "差异比对（<span style='color:#2e7d32'>绿=相同</span> / "
        "<span style='color:#c62828'>红=不同</span>），双击进入字段级对比，"
        "可<i>精确到列</i>采用中心A 数据并保存写回 B 库。"));
    header->setStyleSheet(QStringLiteral(
        "QLabel{background:#f5f7fa;border:1px solid #d7dde5;border-radius:6px;padding:8px;}"));
    layout->addWidget(header);

    auto* tabs = new QTabWidget(central);
    scenario1_ = new Scenario1Widget(ws_ + QStringLiteral("/scenario1"), tabs);
    scenario2_ = new Scenario2Widget(ws_ + QStringLiteral("/scenario2"), tabs);
    tabs->addTab(scenario1_, QStringLiteral("场景1 · 多节点指定库同步"));
    tabs->addTab(scenario2_, QStringLiteral("场景2 · 差异比对与列级同步（Beyond Compare 风格）"));
    layout->addWidget(tabs, 1);

    setCentralWidget(central);
}
