/**
 * sync-suite — 《数据库同步设计2》两场景完整演示程序入口
 * ============================================================================
 *
 * 本程序在**一个** Qt Widgets 应用中完整实现设计图《数据库同步设计2.png》的
 * 两个场景，且不做任何简化：
 *
 *   ┌─ 场景1（多节点指定库同步） ───────────────────────────────────────────┐
 *   │  拓扑：中心节点A ⇄ 子节点B / 子节点C / 子节点D（同属一个域）          │
 *   │  指定数据库（authoritative source）通过中心A 下发到全域；            │
 *   │  子节点本地改动与"所连接的指定数据库"冲突时——以指定数据为准；        │
 *   │  最终指定数据库的数据同步到域内所有子节点（全域收敛）。              │
 *   │  使用真实的 ISyncEngine + UDP 文件传输层（复用 sync-demo 传输实现）。 │
 *   └──────────────────────────────────────────────────────────────────────┘
 *   ┌─ 场景2（类 Beyond Compare 差异比对 GUI） ─────────────────────────────┐
 *   │  同步前自动比较「子节点B（当前节点）」与「中心节点A」两个 SQLite      │
 *   │  数据库中各表的数据差异：相同=绿色，不同=红色；                       │
 *   │  双击某表 → 打开类 Beyond Compare 的双栏字段级对比窗口，             │
 *   │  可"精确到列"地采用中心A 的数据；                                     │
 *   │  点击「保存」→ 将内存中的合并决策写回子节点B 的数据库。              │
 *   │  使用真实的 IComparisonSession（DiffEngine + StagingBuffer）。       │
 *   └──────────────────────────────────────────────────────────────────────┘
 *
 * ----------------------------------------------------------------------------
 * 运行方式
 *   构建产物：build_qmake_demos/examples/sync-suite/sync-suite
 *
 *   # 环境变量（本 demo 为 GUI 程序，QApplication 启动即加载平台插件）：
 *   #  ① LD_LIBRARY_PATH：本机 shell 默认指向 QtCreator 自带的 Qt 5.15.2，与项目使用的
 *   #     Qt 5.12.12 冲突，必须把 5.12.12 的库路径前置，否则启动 abort：
 *   #     "Cannot mix incompatible Qt library (5.12.12) with this library (5.15.2)"。
 *   #  ② QT_QPA_PLATFORM_PLUGIN_PATH：指向 5.12.12 的平台插件目录（否则会误用系统/
 *   #     QtCreator 的插件，同样导致版本不匹配 abort）。
 *   #  ③ QT_QPA_PLATFORM=offscreen：仅无界面验证（--selftest / 无 X 显示）时设置。
 *   export LD_LIBRARY_PATH=/opt/Qt5.12.12/5.12.12/gcc_64/lib:$LD_LIBRARY_PATH
 *   export QT_QPA_PLATFORM_PLUGIN_PATH=/opt/Qt5.12.12/5.12.12/gcc_64/plugins/platforms
 *
 *   cd build_qmake_demos/examples/sync-suite
 *
 *   # 1) 启动 GUI（默认 ws=系统临时目录；需要 X/Wayland 显示环境）
 *   ./sync-suite [--ws <workspace-dir>]
 *
 *   # 2) 无界面自检：headless 跑通两场景逻辑（用于编译后运行验证，退出码 0 即通过）
 *   QT_QPA_PLATFORM=offscreen ./sync-suite --selftest [--ws <dir>]
 *   # 例：QT_QPA_PLATFORM=offscreen ./sync-suite --selftest --ws /tmp/sync-suite-ws
 *
 * --selftest 用于"编译后运行验证"：它不弹出窗口，而是直接驱动两个场景的核心
 * 逻辑（与 GUI 完全相同的代码路径），校验数据收敛/写回正确后以退出码 0 返回。
 * ============================================================================
 */

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QStringList>
#include <QVector>

#include "MainWindow.h"
#include "Scenario1Runner.h"
#include "Scenario2Model.h"
#include <iostream>

// ── QSQLITE session 插件加载修复（与 sync-demo / diff-demo 同源） ─────────────
//
// Qt 5.12 把 applicationDirPath() 追加到 libraryPaths() 的**末尾**，导致系统
// 自带、未启用 SESSION 的 QSQLITE 插件优先被加载。这里把 applicationDirPath()
// 重排到首位，使可执行文件旁的 sqldrivers/libqsqlite.so（启用了
// SQLITE_ENABLE_SESSION）优先生效。必须在任何 QSqlDatabase 操作前调用。
static void prependAppDirToLibraryPaths() {
    QStringList paths;
    paths.append(QCoreApplication::applicationDirPath());
    for (const QString& p : QCoreApplication::libraryPaths())
        if (!paths.contains(p))
            paths.append(p);
    QCoreApplication::setLibraryPaths(paths);
}

// 解析命令行：返回 workspace 目录；通过 out 参数返回是否 selftest。
static QString parseArgs(const QStringList& args, bool* selftest) {
    *selftest = false;
    QString ws;
    for (int i = 1; i < args.size(); ++i) {
        const QString& a = args.at(i);
        if (a == QStringLiteral("--selftest")) {
            *selftest = true;
        } else if (a == QStringLiteral("--ws") && i + 1 < args.size()) {
            ws = args.at(++i);
        } else if (!a.startsWith(QStringLiteral("--"))) {
            ws = a;  // 位置参数也接受为 workspace
        }
    }
    if (ws.isEmpty())
        ws = QDir::tempPath() + QStringLiteral("/sync-suite-ws");
    return ws;
}

// ── 无界面自检：headless 运行两个场景的核心逻辑，验证数据正确收敛/写回 ───────
static int runSelfTest(const QString& ws) {
    std::cout << "============================================================\n"
              << " sync-suite --selftest（无界面验证两场景核心逻辑）\n"
              << " workspace = " << ws.toStdString() << "\n"
              << "============================================================\n";

    // —— 场景1：完整多节点同步编排，跑到全域收敛 ——
    std::cout << "\n[selftest] 场景1：多节点指定库同步 ...\n";
    Scenario1Runner runner(ws + QStringLiteral("/scenario1"));
    // DirectConnection：在 runner 线程内直接打印（无需事件循环）。
    QObject::connect(
        &runner, &Scenario1Runner::logLine, &runner,
        [](const QString& line, int sev) {
            const char* tag = sev >= 2 ? "ERR " : sev == 1 ? "WARN" : "INFO";
            std::cout << "  [" << tag << "] " << line.toStdString() << "\n";
        },
        Qt::DirectConnection);

    // 用 QueuedConnection 接收收敛网格，真实走一遍「工作线程 emit → 主线程槽」的
    // 跨线程封送路径（与 GUI 的 onGrid 完全一致）。若 QVector<QStringList> 未注册元类型，
    // 这次投递会被 Qt 丢弃、gridArrived 保持 false——以此回归校验 GUI 网格空白的修复。
    bool gridArrived = false;
    QObject::connect(
        &runner, &Scenario1Runner::gridReady, qApp,
        [&](const QStringList& headers, const QVector<QStringList>& rows, const QString& caption) {
            gridArrived = true;
            std::cout << "  [GRID] " << caption.toStdString() << "（列=" << headers.size()
                      << " 行=" << rows.size() << "）\n";
            std::cout << "         " << headers.join(QStringLiteral(" | ")).toStdString() << "\n";
            for (const QStringList& r : rows)
                std::cout << "         " << r.join(QStringLiteral(" | ")).toStdString() << "\n";
        },
        Qt::QueuedConnection);

    runner.start();
    runner.wait();
    // 排空主线程事件队列，触发上面的队列槽（验证 QVector<QStringList> 封送已修复）。
    QCoreApplication::processEvents();
    const bool s1ok = runner.succeeded() && gridArrived;
    std::cout << "[selftest] 场景1 结果：" << (runner.succeeded() ? "收敛成功 ✅" : "失败 ❌")
              << "；收敛网格队列投递：" << (gridArrived ? "已送达 ✅" : "未送达 ❌") << "\n";

    // —— 场景2：比对会话 diff → 列级 stage → save 写回，校验 B 库更新 ——
    std::cout << "\n[selftest] 场景2：类 Beyond Compare 比对与列级同步 ...\n";
    Scenario2Model model(ws + QStringLiteral("/scenario2"));
    // 把 UDP 快照往返的「请求→响应→比对」时序打到控制台（GUI 走日志区，此处走 stdout）。
    model.setLogSink(
        [](const QString& line) { std::cout << "  [场景2] " << line.toStdString() << "\n"; });
    QString err;
    bool s2ok = model.setup(&err);
    if (!s2ok) {
        std::cout << "[selftest] 场景2 setup 失败：" << err.toStdString() << "\n";
    } else {
        s2ok = model.runHeadlessSelfTest(&err);
        std::cout << "[selftest] 场景2 结果："
                  << (s2ok ? "比对/列级同步/写回成功 ✅"
                           : (QStringLiteral("失败 ❌ ") + err).toStdString())
                  << "\n";
    }

    const bool ok = s1ok && s2ok;
    std::cout << "\n============================================================\n"
              << " selftest 总结：" << (ok ? "全部通过 ✅" : "存在失败 ❌") << "\n"
              << "============================================================\n";
    return ok ? 0 : 1;
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("sync-suite"));

    prependAppDirToLibraryPaths();

    bool selftest = false;
    const QString ws = parseArgs(app.arguments(), &selftest);
    QDir().mkpath(ws);

    if (selftest)
        return runSelfTest(ws);

    MainWindow w(ws);
    w.resize(1180, 760);
    w.show();
    return app.exec();
}
