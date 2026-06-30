#include "MainWindow.h"

#include <QLabel>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

#include "Scenario1Widget.h"  // 场景1 部件的完整定义（头里只前置声明，此处才真正包含）
#include "Scenario2Widget.h"  // 场景2 部件的完整定义

// ============================================================================
// MainWindow.cpp — sync-suite 主窗口的实现（纯 UI 装配，无业务逻辑）
// ============================================================================
//
// 【本文件做什么】只有一个构造函数：把窗口的「标题 + 顶部说明条 + 双标签页」搭起来，
//   并实例化两个场景部件塞进标签页。没有任何同步/数据库逻辑——那些都在两个
//   Scenario*Widget 内部。整体职责与协作者详见配套头文件 MainWindow.h。
//
// 【Qt 内存管理小贴士（理解下面 new 不配 delete 的关键）】
//   Qt 的 QObject 父子机制：给子部件指定 parent 后，父对象析构时会自动递归删除所有子对象。
//   因此本文件里所有 new 出来的部件都通过「构造时传 parent」或「被加入布局/标签页」而挂上
//   父子链，无需也不应手动 delete——这是 Qt GUI 代码的惯用法，不是内存泄漏。
// ============================================================================

// 构造：装配整个主窗口界面。
MainWindow::MainWindow(const QString& ws, QWidget* parent) : QMainWindow(parent), ws_(ws) {
    // 设置窗口标题栏文字（中文，点明这是《数据库同步设计2》的两场景演示）。
    setWindowTitle(
        QStringLiteral("数据库同步设计2 — 场景演示（场景1 多节点同步 / 场景2 差异比对 GUI）"));

    // central：主窗口的「中央部件」容器；QMainWindow 要求把主体内容放在中央部件里。
    // 以 this 为父 → 随窗口一同销毁。
    auto* central = new QWidget(this);
    // 垂直布局：自上而下排「说明条」与「标签页」。把 central 作为布局父对象，部件加入后
    // 即被该布局接管定位。
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(8, 8, 8, 8);  // 四周留 8px 边距，避免内容贴边

    // 顶部说明条：概述两场景，呼应设计图。
    // 用一个 QLabel 以「富文本(RichText)」渲染一段带颜色强调的 HTML 说明，让用户一眼明白
    // 两个标签页各演示什么。
    auto* header = new QLabel(central);
    header->setTextFormat(Qt::RichText);  // 启用富文本：下面的 <b>/<span style> 才会被渲染
    header->setWordWrap(true);  // 自动换行，窗口变窄时文字回流而非被截断
    header->setText(QStringLiteral(
        "<b>《数据库同步设计2》演示</b> &nbsp;—&nbsp; "
        "<span style='color:#1565c0'>场景1</span>：中心节点A ⇄ 子节点B/C/D + 指定数据库，"
        "真实 UDP 多节点同步，<i>以指定数据为准</i>，全域收敛；&nbsp;&nbsp;"
        "<span style='color:#c62828'>场景2</span>：类 Beyond Compare 的「子节点B ⇄ 中心节点A」"
        "差异比对（<span style='color:#2e7d32'>绿=相同</span> / "
        "<span style='color:#c62828'>红=不同</span>），双击进入字段级对比，"
        "可<i>精确到列</i>采用中心A 数据并保存写回 B 库。"));
    // 给说明条加一个浅灰圆角卡片样式（QSS，类似 CSS），与正文区域形成视觉分隔。
    header->setStyleSheet(QStringLiteral(
        "QLabel{background:#f5f7fa;border:1px solid #d7dde5;border-radius:6px;padding:8px;}"));
    layout->addWidget(header);  // 说明条置于布局顶部

    // 标签页容器：两个场景各占一个标签页。
    auto* tabs = new QTabWidget(central);
    // 实例化两个场景部件，各自的工作目录由主目录 ws_ 派生出独立子目录，互不干扰。
    // 以 tabs 为 parent → 随标签页/窗口一同销毁。
    scenario1_ = new Scenario1Widget(ws_ + QStringLiteral("/scenario1"), tabs);
    scenario2_ = new Scenario2Widget(ws_ + QStringLiteral("/scenario2"), tabs);
    // 把两个部件加入标签页并各起一个中文标题。
    tabs->addTab(scenario1_, QStringLiteral("场景1 · 多节点指定库同步"));
    tabs->addTab(scenario2_, QStringLiteral("场景2 · 差异比对与列级同步（Beyond Compare 风格）"));
    // addWidget 的第二参数 stretch=1：让标签页区域吸收所有多余竖向空间（说明条保持自然高度，
    // 标签页随窗口拉伸而变高）。
    layout->addWidget(tabs, 1);

    // 把装好的中央部件交给主窗口。至此窗口构造完毕，可被 main() show() 出来。
    setCentralWidget(central);
}
