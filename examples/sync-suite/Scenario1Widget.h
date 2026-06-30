#pragma once

// ============================================================================
// Scenario1Widget.h — sync-suite 演示程序「场景1」的界面控件
// ============================================================================
//
// 【这个文件是什么】
//   一个 QWidget 子类，构成 sync-suite GUI 示例中「场景1：多节点指定库同步」这一页
//   的全部界面：顶部文字说明 + ASCII 拓扑图 + 运行按钮 + 阶段标签 + 实时日志窗 +
//   收敛结果网格。它【只管界面】，真正跑同步的活由后台线程 Scenario1Runner 完成。
//
// 【为什么这么拆（界面 / 计算分离）】
//   真实的多节点 UDP 同步是耗时的阻塞流程（建多个节点、发收报文、等 ACK、校验收敛），
//   若直接在 GUI 线程跑会冻结界面。因此把它放进后台线程 Scenario1Runner（继承
//   QThread），本控件仅做两件事：① 点「运行」时创建并启动 runner；② 用槽函数接收
//   runner 通过 Qt 信号「跨线程」发回的进度（日志行/阶段名/结果网格/完成标志），
//   增量刷到界面上。信号槽的跨线程队列连接保证了刷新发生在 GUI 线程，线程安全。
//
// 【数据流：谁调用它、它调用谁】
//   主窗口（sync-suite 的 MainWindow / Tab 容器）创建本控件并塞进某个标签页；
//   本控件 → 创建 Scenario1Runner（传入工作目录 ws_）→ 启动 → 监听其四个信号：
//     logLine(行,严重度) / phaseChanged(阶段名) / gridReady(表头,行,标题) /
//     runFinished(是否全域收敛)。runner 线程结束后经 deleteLater 自动回收。
//
// 【线程模型】
//   本控件对象活在 GUI 线程；Scenario1Runner 活在它自己的线程。二者只通过信号槽通信
//   （默认 Qt::AutoConnection → 跨线程时退化为队列连接），所有槽都在 GUI 线程执行，
//   故槽内可安全直接操作 widget。runner_ 指针的写入也都在 GUI 线程，无需额外加锁。
//
// 注释为教学用途，详解每个槽「在响应什么、把什么刷到界面」。
// ============================================================================

// ── 场景1 界面：拓扑图示 + 运行按钮 + 实时日志 + 收敛网格 ─────────────────────
//
// 点击「运行」后在后台线程（Scenario1Runner）执行真实多节点 UDP 同步，
// 并通过信号把日志、阶段、最终收敛网格实时刷新到界面。

#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

// 前置声明：只用到指针/引用，无需在头文件包含其完整定义（缩短编译依赖、加快编译）。
class Scenario1Runner;  // 后台同步执行线程（真正干活者，定义在 Scenario1Runner.h）
class QTextEdit;        // 实时日志显示控件
class QTableWidget;     // 收敛结果网格
class QPushButton;      // 「运行」按钮
class QLabel;           // 文字说明 / 阶段标签 / 网格标题

// ── Scenario1Widget —— 场景1 的容器控件（界面层，无业务逻辑）──────────────────
// 【做什么】搭建并持有场景1 页面的所有子控件，把后台 runner 的信号翻译成界面更新。
// 【为什么独立成类】把「场景1 这一页」封装成一个自洽的 QWidget，主窗口拿来即用、
//   可单独复用，也便于与场景2 等并列摆进标签页。
// 【状态/线程】对象在 GUI 线程；持有一个可选的后台 runner_。Q_OBJECT 宏启用元对象
//   系统（信号槽/moc），故 .cpp 末尾需 #include 对应的 .moc。
class Scenario1Widget : public QWidget {
    Q_OBJECT
   public:
    // 构造：搭建整页 UI。ws 是本次演示使用的工作目录（各节点的 SQLite 文件、传输落地
    //   目录等都建在此目录下），原样转交给 Scenario1Runner。parent 走 Qt 父子内存树。
    explicit Scenario1Widget(const QString& ws, QWidget* parent = nullptr);

   private slots:
    // —— 以下五个槽：四个响应 runner 信号、一个响应按钮点击。均在 GUI 线程执行 ——
    void onRun();  // 「运行」按钮被点击：创建并启动后台 runner，禁用按钮、清空旧输出
    // 收到一行日志：sev 为严重度（0 普通 / 1 警告 / 2 错误），据此着色后追加到日志窗。
    void onLogLine(const QString& line, int sev);
    // 进入新阶段：把阶段名显示在 phase_ 标签，并以醒目分隔样式插入日志，便于阅读分段。
    void onPhase(const QString& title);
    // 收到一张结果网格：headers 为列标题、rows 为各行单元格、caption 为网格标题。
    //   末列为「收敛与否」列，按内容是否以「✓」开头着绿/红背景。
    void onGrid(const QStringList& headers, const QVector<QStringList>& rows,
                const QString& caption);
    // 演示结束：ok 表示全域是否收敛。恢复按钮、更新阶段标签为成功/警告终态。
    void onFinished(bool ok);

   private:
    // 把一段 HTML 追加到日志窗末尾并滚到底（供上面几个槽复用的小工具）。
    void appendLog(const QString& html);

    QString ws_;                         // 工作目录（传给 runner）
    Scenario1Runner* runner_ = nullptr;  // 当前后台 runner；nullptr 表示空闲（未在运行）

    // —— 子控件指针（生命周期由 Qt 父子树托管，本类析构时随 QWidget 一并释放）——
    QPushButton* btnRun_ = nullptr;  // 运行/重新运行按钮
    QLabel* phase_ = nullptr;        // 当前阶段标签
    QTextEdit* logView_ = nullptr;   // 实时日志窗（深色终端风）
    QTableWidget* grid_ = nullptr;   // 收敛结果网格
    QLabel* gridCaption_ = nullptr;  // 网格上方的标题标签
};
