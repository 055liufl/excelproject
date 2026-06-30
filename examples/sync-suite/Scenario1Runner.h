#pragma once

// ============================================================================
// Scenario1Runner.h — sync-suite 演示「场景1」的后台编排线程（声明）
// ============================================================================
//
// 【这个文件是什么】
//   一个继承 QThread 的「场景编排器」：它在自己的后台线程里，端到端地把同步设计文档
//   的「场景1」完整跑一遍——真实地起四个同步节点、真实地用 UDP 文件传输层互发工件、
//   真实地制造并发冲突再观察收敛。它是 GUI 演示程序的「剧本/导演」，GUI 只负责显示
//   它通过信号播报出来的日志、阶段标题、收敛结果。
//
// 【为什么单独开一条线程】
//   整个剧本要反复 sleep 等待对端应用变更/回 ACK（涉及真实文件 I/O 与网络往返），耗时
//   数秒到十几秒。若在 GUI 线程跑会把界面卡死。故继承 QThread，把重活放进 run()，再用
//   Qt 信号（跨线程默认队列连接）把进展安全地投递回 GUI 线程刷新。
//
// 【演示的拓扑】
//   中心节点 A(rank100) ⇄ 子节点 B(70) / 子节点 C(50) / 子节点 D(30)，同属一个域；
//   另有一个「指定数据库」designated.db 充当全域唯一权威数据源（不是同步节点，只是数据来源）。
//   rank 即 originPriority：数字越大优先级越高，冲突时高 rank 的来源胜出。
//
// 【四个阶段（均用真实 ISyncEngine + 真实 UDP 传输，无任何打桩简化）】
//   Phase 1 指定库基线下发：中心 A 从指定库导入全部权威数据 → 广播 → B/C/D 追平。
//   Phase 2 并发冲突·以指定为准：子节点 B 离线把张三薪资改小，同时中心 A 从更新后的
//           指定库导入新权威值；B 上行推送 → 中心 A 检测到并发冲突 → 按 rank 仲裁，
//           指定库/中心 A（rank100）胜出；随后广播使全域收敛到指定值。
//   Phase 3 子节点重连指定库自我纠正：子节点 C 离线改错 → 重连指定库重导入 →
//           本地被指定值覆盖 → 上行保持一致。
//   Phase 4 收敛校验：对比 指定库 / A / B / C / D 的若干字段，确认全域收敛。
//
// 【线程与协作模型】
//   · run() 在本线程执行整套剧本；过程中只通过 emit 信号与外界通信，不直接碰 GUI。
//   · 四类信号（logLine/phaseChanged/gridReady/runFinished）被 GUI 以队列连接接收，
//     故跨线程安全。其中 gridReady 的 QVector<QStringList> 参数需在构造里注册元类型
//     （见 .cpp 构造函数注释），否则队列连接会静默丢弃该调用。
//   · 协作者：DataBridge（每节点一个，开物理库）、ISyncEngine（每节点一个同步引擎）、
//     UdpFileTransport（每节点一个传输层，见 udp_transport.h）。
// ============================================================================

#include <QString>
#include <QStringList>
#include <QThread>
#include <QVector>

// Scenario1Runner —— 在后台线程跑「场景1」完整剧本的编排器；进展经信号投递给 GUI。
class Scenario1Runner : public QThread {
    Q_OBJECT
   public:
    // 构造：记下工作目录 ws（剧本会在其下建各节点的库与 outbox/inbox/quarantine 目录），
    //   并注册 gridReady 所需的元类型。不在此启动线程——调用方显式 start() 才进入 run()。
    explicit Scenario1Runner(const QString& ws, QObject* parent = nullptr);

    // 剧本是否整体成功（= Phase 4 全域收敛）。线程结束后由 GUI 读取，作最终结论。
    bool succeeded() const {
        return succeeded_;
    }

   signals:
    // 播报一行日志。sev: 0=INFO 1=WARN 2=ERROR（GUI 据此着色）。
    void logLine(const QString& line, int sev);
    // 进入新阶段时播报阶段标题（GUI 用于更新阶段指示）。
    void phaseChanged(const QString& title);
    // 收敛网格：headers 为列名，rows 每行一个 QStringList（一个探测字段在各节点的取值），
    //   caption 为整表标题。GUI 据此渲染最终的「各节点值对照表」。
    void gridReady(const QStringList& headers, const QVector<QStringList>& rows,
                   const QString& caption);
    // 剧本结束信号：ok = 是否成功收敛。GUI 据此收尾（解锁按钮、显示总结等）。
    void runFinished(bool ok);

   protected:
    // QThread 入口：start() 后在新线程里被调用，承载整套四阶段剧本。
    void run() override;

   private:
    // 便捷封装：发一行日志（默认 INFO 级）。仅是对 emit logLine 的简写。
    void log(const QString& s, int sev = 0) {
        emit logLine(s, sev);
    }

    QString ws_;              // 工作目录（各节点库与同步收发目录的根）
    bool succeeded_ = false;  // 剧本最终是否成功收敛（Phase 4 写入）
};
