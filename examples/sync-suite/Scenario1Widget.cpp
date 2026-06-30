// ============================================================================
// Scenario1Widget.cpp — 场景1 界面控件的实现（UI 搭建 + runner 信号 → 界面更新）
// ============================================================================
//
// 本文件实现 Scenario1Widget.h 声明的构造与五个槽。两大块：
//   1) 构造函数：自顶向下用 QVBoxLayout/QHBoxLayout 拼出整页（说明 → 拓扑图 → 运行条
//      → 日志窗 → 收敛网格），并把「运行」按钮的 clicked 信号接到 onRun。
//   2) 五个槽：onRun 启动后台线程；onLogLine/onPhase/onGrid/onFinished 把后台线程
//      跨线程发回的进度增量刷到界面。所有槽都在 GUI 线程跑（队列连接），可直接改控件。
//
// 阅读提示：界面代码大量用 QStringLiteral（编译期常量字符串，零分配）与内联样式表
//   （setStyleSheet），属演示程序的常规写法；真正值得关注的是 onRun 里「线程生命周期
//   管理」与 onGrid 里「收敛列着色」两处。
// ============================================================================
#include "Scenario1Widget.h"

#include <QBrush>
#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextCursor>
#include <QTextEdit>
#include <QVBoxLayout>

#include "Scenario1Runner.h"

// ── 构造：自顶向下搭建整页 UI ────────────────────────────────────────────────
// 做什么：用一个竖直主布局 root 依次堆放五块区域；把按钮点击接到 onRun。
// 为什么：QWidget 的惯例做法——构造时一次性建好子控件并交给布局管理；子控件以 this
//   为父对象，随本控件析构自动释放（无需手工 delete）。
// 参数：ws 工作目录（仅保存进 ws_，运行时传给 runner）；parent 走 Qt 父子树。
// 副作用：创建并持有若干子控件。线程：GUI 线程（构造发生在主窗口创建本页时）。
Scenario1Widget::Scenario1Widget(const QString& ws, QWidget* parent) : QWidget(parent), ws_(ws) {
    auto* root = new QVBoxLayout(this);  // 主竖直布局；以 this 为父 → 自动设为本控件布局

    // —— 说明 —— 富文本说明区，概述场景1 的四步演示剧情（基线下发/并发冲突/重连纠正/收敛）
    //   setTextFormat(RichText) 让其中的 <b>/<i> 标签被解释为粗斜体；setWordWrap 自动折行。
    auto* desc = new QLabel(this);
    desc->setTextFormat(Qt::RichText);
    desc->setWordWrap(true);
    desc->setText(
        QStringLiteral("<b>场景1 · 多节点指定库同步</b>（真实 ISyncEngine + UDP 文件传输层）<br/>"
                       "中心节点A ⇄ 子节点B / C / D 同属一个域；<b>指定数据库</b>为全域权威源。"
                       "演示：① 指定库基线下发全域；② 子节点离线误改与指定库<b>并发冲突</b> → "
                       "按 rank 仲裁，<i>以指定数据为准</i>；③ 子节点重连指定库重导入自我纠正；"
                       "④ 全域收敛校验。"));
    root->addWidget(desc);

    // —— 拓扑图示（等宽字体）—— 用 ASCII 字符画出「指定库→中心节点A→子节点B/C/D」的
    //   星型拓扑。必须用等宽字体（FixedFont）渲染，否则方框/连线会因字宽不一而错位。
    auto* topo = new QLabel(this);
    topo->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    topo->setText(
        QStringLiteral("        ┌──────────────┐\n"
                       "        │   指定数据库   │  （权威源，以其数据为准）\n"
                       "        └───────┬──────┘\n"
                       "                │ 连接 / 导入\n"
                       "        ┌───────▼──────┐\n"
                       "   ┌────┤   中心节点A    ├────┐\n"
                       "   │    └───────┬──────┘    │   （双向同步）\n"
                       "   ▼            ▼           ▼\n"
                       "┌──────┐   ┌──────┐    ┌──────┐\n"
                       "│子节点B│   │子节点C│    │子节点D│\n"
                       "└──────┘   └──────┘    └──────┘"));
    topo->setStyleSheet(QStringLiteral(
        "background:#fbfdff;border:1px solid #d7dde5;border-radius:6px;padding:6px;"));
    root->addWidget(topo);

    // —— 运行条 —— 一行水平布局：左侧「运行」按钮，右侧阶段标签，再用弹簧把它们顶向左。
    auto* bar = new QHBoxLayout();
    btnRun_ = new QPushButton(QStringLiteral("▶ 运行场景1演示"), this);
    btnRun_->setStyleSheet(QStringLiteral("font-weight:bold;padding:4px 14px;"));
    // 把按钮的 clicked 信号接到本类 onRun 槽——这是整页唯一的用户触发入口。
    connect(btnRun_, &QPushButton::clicked, this, &Scenario1Widget::onRun);
    bar->addWidget(btnRun_);
    phase_ = new QLabel(QStringLiteral("（就绪）"), this);  // 阶段标签初始为「就绪」
    phase_->setStyleSheet(QStringLiteral("color:#1565c0;font-weight:bold;"));
    bar->addWidget(phase_);
    bar->addStretch(1);  // 弹簧：占满剩余横向空间，使按钮+标签靠左对齐
    root->addLayout(bar);

    // —— 实时日志 —— 只读、等宽、深色背景的终端风日志窗。addWidget 第二参 1 = 伸展因子，
    //   让日志窗吃掉竖直方向的多余空间（成为页面主体）。
    logView_ = new QTextEdit(this);
    logView_->setReadOnly(true);  // 只显示、禁编辑
    logView_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    logView_->setStyleSheet(QStringLiteral("background:#1e1e1e;color:#dcdcdc;"));
    root->addWidget(logView_, 1);

    // —— 收敛网格 —— 标题 + 只读网格，展示各节点最终是否与权威源收敛。
    gridCaption_ = new QLabel(QStringLiteral("收敛结果（运行后显示）"), this);
    gridCaption_->setStyleSheet(QStringLiteral("font-weight:bold;"));
    root->addWidget(gridCaption_);
    grid_ = new QTableWidget(this);
    grid_->setEditTriggers(QAbstractItemView::NoEditTriggers);  // 禁止用户编辑单元格
    grid_->verticalHeader()->setVisible(false);  // 隐藏左侧行号列（无意义）
    grid_->setMaximumHeight(160);                // 限高，避免网格抢占日志窗空间
    root->addWidget(grid_);
}

// ── appendLog —— 把一段 HTML 追加到日志窗末尾并保持滚动到底 ───────────────────
// 做什么：取文本光标 → 移到文末 → 插入 HTML → 另起一段 → 确保光标可见（滚到底）。
// 为什么用光标而非 append()：append 会按纯文本/默认块处理；这里要插入带颜色 <span> 的
//   HTML 片段，用 QTextCursor::insertHtml 才能正确渲染颜色。insertBlock() 等价于换行成段。
// 线程：仅由四个槽在 GUI 线程调用，故直接操作 logView_ 安全。
void Scenario1Widget::appendLog(const QString& html) {
    QTextCursor c = logView_->textCursor();
    c.movePosition(QTextCursor::End);  // 光标移到文档末尾
    c.insertHtml(html);                // 插入这段带样式的 HTML
    c.insertBlock();                   // 起新段落（视觉上换行）
    logView_->setTextCursor(c);        // 回写光标
    logView_->ensureCursorVisible();   // 自动滚动，使最新一行始终可见
}

// ── onRun —— 「运行」按钮槽：创建并启动后台 runner，进入运行态 ────────────────
// 做什么：① 若已有 runner 在跑则直接返回（防重入）；② 禁用按钮、改文案、清空旧日志/网格；
//   ③ new 一个 Scenario1Runner，连好它的四个进度信号到本类对应槽，再连两条「生命周期」
//   信号实现自动回收；④ start() 启动线程。
// 为什么这样管理线程生命周期（关键）：
//   · finished → deleteLater：QThread 跑完后不能立刻 delete（它可能还在收尾），用
//     deleteLater 把删除投递到事件循环安全时机；
//   · destroyed → 把 runner_ 置回 nullptr：runner 真正销毁后，复位「空闲」标志，
//     使下次点击能再次创建（也让 onRun 顶部的重入判断恢复有效）。
// 线程：本槽在 GUI 线程执行；runner 启动后在自己的线程跑同步。
void Scenario1Widget::onRun() {
    if (runner_)
        return;  // 已在运行：忽略重复点击（防重入，避免并发起多个同步）
    btnRun_->setEnabled(false);  // 运行期间禁用按钮
    btnRun_->setText(QStringLiteral("运行中 ..."));
    logView_->clear();      // 清空上一轮日志
    grid_->setRowCount(0);  // 清空上一轮网格
    gridCaption_->setText(QStringLiteral("收敛结果（运行中 ...）"));

    runner_ = new Scenario1Runner(ws_, this);  // 以 this 为父；ws_ 指定工作目录
    // 四条「进度」信号 → 本类槽（跨线程，默认队列连接，槽在 GUI 线程执行）。
    connect(runner_, &Scenario1Runner::logLine, this, &Scenario1Widget::onLogLine);
    connect(runner_, &Scenario1Runner::phaseChanged, this, &Scenario1Widget::onPhase);
    connect(runner_, &Scenario1Runner::gridReady, this, &Scenario1Widget::onGrid);
    connect(runner_, &Scenario1Runner::runFinished, this, &Scenario1Widget::onFinished);
    // 线程结束后自动回收：finished 触发 deleteLater（安全延迟删除）；
    connect(runner_, &QThread::finished, runner_, &QObject::deleteLater);
    // 对象真正销毁后把 runner_ 复位为 nullptr（恢复「空闲」，允许再次运行）。
    connect(runner_, &QObject::destroyed, this, [this] { runner_ = nullptr; });
    runner_->start();  // 启动后台线程：进入 Scenario1Runner::run() 真正跑同步
}

// ── onLogLine —— 收到一行日志：按严重度着色后追加到日志窗 ──────────────────────
// 做什么：sev≥2(错误)→红，==1(警告)→黄，否则(普通)→蓝；包成带 color 的 <span> 追加。
// 为什么 toHtmlEscaped()：日志文本可能含 < > & 等字符，转义后才不会被当成 HTML 标签
//   而破坏渲染（也防注入）。线程：GUI 线程（队列连接投递而来）。
void Scenario1Widget::onLogLine(const QString& line, int sev) {
    const QString color = sev >= 2   ? QStringLiteral("#ff6b6b")   // 错误：红
                          : sev == 1 ? QStringLiteral("#ffd166")   // 警告：黄
                                     : QStringLiteral("#9cdcfe");  // 普通：蓝
    appendLog(QStringLiteral("<span style='color:%1'>%2</span>").arg(color, line.toHtmlEscaped()));
}

// ── onPhase —— 进入新阶段：更新阶段标签 + 在日志里插入醒目分隔条 ────────────────
// 做什么：把阶段名写到 phase_ 标签；并以青绿粗体「━━ 阶段名 ━━」插入日志，分隔各阶段输出。
void Scenario1Widget::onPhase(const QString& title) {
    phase_->setText(title);
    appendLog(QStringLiteral("<span style='color:#4ec9b0;font-weight:bold'>━━ %1 ━━</span>")
                  .arg(title.toHtmlEscaped()));
}

// ── onGrid —— 收到收敛结果网格：填表 + 给「收敛与否」列着色 ────────────────────
// 做什么：设标题、列数、表头、行数，逐单元格填值；对「最后一列」（约定为收敛结论列）
//   按其文字是否以「✓」开头着绿（收敛）/红（未收敛）背景，让结果一眼可辨。
// 关键点：
//   · 内层循环用 `j < r.size() && j < headers.size()` 双重保护，防止行列长度不一时越界。
//   · QTableWidgetItem 用 new 创建后交给 setItem，所有权转移给 QTableWidget（自动释放）。
//   · 末尾 Stretch：让各列等宽铺满网格宽度。
// 线程：GUI 线程。
void Scenario1Widget::onGrid(const QStringList& headers, const QVector<QStringList>& rows,
                             const QString& caption) {
    gridCaption_->setText(caption);
    grid_->setColumnCount(headers.size());
    grid_->setHorizontalHeaderLabels(headers);
    grid_->setRowCount(rows.size());
    for (int i = 0; i < rows.size(); ++i) {
        const QStringList& r = rows.at(i);
        for (int j = 0; j < r.size() && j < headers.size(); ++j) {  // 双重边界保护
            auto* it = new QTableWidgetItem(r.at(j));
            if (j == headers.size() - 1) {  // 收敛列着色（约定最后一列为收敛结论列）
                const bool ok = r.at(j).startsWith(QStringLiteral("✓"));  // 「✓」前缀 = 收敛
                it->setBackground(QBrush(ok ? QColor(0xd6, 0xf5, 0xd6)    // 浅绿 = 收敛
                                            : QColor(0xff, 0xd6, 0xd6)));  // 浅红 = 未收敛
            }
            grid_->setItem(i, j, it);  // 所有权转给网格
        }
    }
    grid_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);  // 各列等宽铺满
}

// ── onFinished —— 演示结束：恢复按钮、更新终态标签 ────────────────────────────
// 做什么：重新启用按钮并改文案为「重新运行」；据 ok 把阶段标签设为成功/警告终态文案。
// 注意：本槽不需要 delete runner——onRun 里已连好 finished→deleteLater 与
//   destroyed→runner_=nullptr，回收与状态复位会自动发生。线程：GUI 线程。
void Scenario1Widget::onFinished(bool ok) {
    btnRun_->setEnabled(true);
    btnRun_->setText(QStringLiteral("▶ 重新运行场景1演示"));
    phase_->setText(ok ? QStringLiteral("✅ 演示完成（全域已收敛）")
                       : QStringLiteral("⚠ 演示结束（存在未收敛项，见日志）"));
}
