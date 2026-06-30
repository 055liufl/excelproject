#pragma once

// ── 场景1 编排器（后台线程，真实多节点 UDP 同步） ───────────────────────────
//
// 在独立线程中完整跑通设计图场景1：
//   拓扑：中心节点A(rank100) ⇄ 子节点B(70) / 子节点C(50) / 子节点D(30)，同属一个域；
//   存在一个「指定数据库」(designated.db) 作为全域权威数据源。
//
// 演示的四个阶段（均使用真实 ISyncEngine + UDP 文件传输层，无任何简化）：
//   Phase 1 指定库基线下发：中心A 连接指定库导入数据 → 广播 → B/C/D 追平到指定库。
//   Phase 2 并发冲突·以指定为准：子节点B 离线把张三薪资改小，同时中心A 从更新后的
//           指定库导入新权威值；B 上行推送 → 中心A 检测到并发冲突 → 按 rank 仲裁，
//           指定库/中心A 胜出（以指定数据为准）；随后广播使全域收敛到指定值。
//   Phase 3 子节点重连指定库自我纠正：子节点C 离线改错 → 重新连接指定库重导入 →
//           本地被指定值覆盖（节点处亦以指定数据为准）→ 上行保持一致。
//   Phase 4 收敛校验：对比 指定库 / A / B / C / D，确认全域收敛。
//
// 通过信号把日志、阶段、收敛网格、结束状态投递到 GUI 线程（Qt 队列连接）。

#include <QString>
#include <QStringList>
#include <QThread>
#include <QVector>

class Scenario1Runner : public QThread {
    Q_OBJECT
   public:
    explicit Scenario1Runner(const QString& ws, QObject* parent = nullptr);

    bool succeeded() const {
        return succeeded_;
    }

   signals:
    // sev: 0=INFO 1=WARN 2=ERROR。
    void logLine(const QString& line, int sev);
    void phaseChanged(const QString& title);
    // 收敛网格：headers 为列名，rows 每行一个 QStringList，caption 为标题。
    void gridReady(const QStringList& headers, const QVector<QStringList>& rows,
                   const QString& caption);
    void runFinished(bool ok);

   protected:
    void run() override;

   private:
    void log(const QString& s, int sev = 0) {
        emit logLine(s, sev);
    }

    QString ws_;
    bool succeeded_ = false;
};
