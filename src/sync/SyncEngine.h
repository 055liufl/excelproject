#pragma once
// ============================================================================
// SyncEngine.h — ISyncEngine 的实现门面（前台请求 → 后台 SyncWorker 的转发器）声明
// ============================================================================
//
// 【这个文件是什么】
//   ISyncEngine（见 include/dbridge/sync/ISyncEngine.h）的唯一实现类的声明。
//   SyncEngine 本身【不动数据库】——它是一层很薄的“门面（facade）/编排者”：
//     · 把 initialize / sync / syncSelected / write / stop 等前台请求，转交给真正
//       持有写连接、在后台线程串行执行写操作的 SyncWorker；
//     · 通过 Qt 信号槽订阅 worker 的 progressUpdated / errorOccurred，把异步进展
//       归集进自己加锁保护的“快照”（progress_/result_/logs_/errors_），供
//       state()/progress()/logs()/errors()/result() 这些只读 getter 线程安全地返回；
//     · 管理前台门控（ForegroundGate）的获取与释放，保证“同一时刻至多一个前台操作”。
//
// 【在引擎架构中的位置】
//   应用层 ── 只见 ISyncEngine 抽象
//       │ createSyncEngine(bridge) 返回的就是本类（以 unique_ptr 持有）
//   SyncEngine（本类，前台门面）
//       │ 持有：SyncContext（每库一份的共享上下文）+ SyncWorker（后台单写线程）
//       ▼
//   SyncWorker（后台线程，独占写连接，真正捕获/应用/广播变更）
//
// 【关键协作者】
//   · DataBridge& bridge_：批量 ETL 那半边的门面。初始化时本类调用
//     bridge_.setSyncActive(true) 把“绕过同步的直写导入”门控住（前台/后台互斥）。
//   · SyncContext ctx_：按物理库（dev+inode）唯一的共享上下文，登记在
//     SyncContextRegistry。本类把转发用的回调（importFn/workerWriteFn/
//     workerCaptureWriteFn/rescanFn）写入 ctx_，供 BatchTransfer/ComparisonSession 复用。
//   · SyncWorker worker_：所有“会改库”的活的真正执行者。
//   · ForegroundGate（在 ctx_->gate）：前台互斥闸门。
//
// 【线程模型】
//   前台方法在调用线程运行；只读 getter 与 worker 信号回调都经 snapMutex_ 加锁访问
//   共享快照，故 getter 可在任意线程安全调用。详细逐方法语义见 ISyncEngine.h 的 ①~⑩。
// ============================================================================
#include "dbridge/sync/ISyncEngine.h"
#include "dbridge/sync/SyncConfig.h"
#include "dbridge/sync/SyncTypes.h"

#include <QMutex>

#include "SyncContext.h"
#include "SyncWorker.h"
#include <memory>

namespace dbridge {
class DataBridge;  // 前置声明：本类只持有其引用，避免在头文件强依赖完整定义。
}

namespace dbridge::sync {

// SyncEngine —— ISyncEngine 的实现门面。把前台请求转交后台 SyncWorker，并把 worker
// 的异步进展/错误归集为线程安全的只读快照。逐方法对外语义见 ISyncEngine.h（①~⑩）。
class SyncEngine : public ISyncEngine {
   public:
    // 构造：仅保存 DataBridge 引用，不做任何重活（真正初始化在 initialize()）。
    explicit SyncEngine(DataBridge& bridge);
    // 析构：解除直写门控、清空 ctx_ 上的转发回调、停止并等待 worker、释放 SyncContext。
    ~SyncEngine() override;

    // ── ISyncEngine 接口实现（详尽语义见 ISyncEngine.h 同名方法）──────────────
    bool initialize(const SyncConfig& config, QString* err = nullptr) override;  // ①
    bool sync(QString* err = nullptr) override;                                  // ②
    bool stop(QString* err = nullptr) override;                                  // ③

    SyncState state() const override;           // ④
    SyncProgress progress() const override;     // ⑤
    QList<SyncLogEntry> logs() const override;  // ⑥
    QList<SyncError> errors() const override;   // ⑦
    SyncResult result() const override;         // ⑧

    bool syncSelected(const SyncSelection& selection, QString* err = nullptr) override;  // ⑨
    bool write(const QList<RowMutation>& mutations, QString* err = nullptr) override;    // ⑩

   private:
    // ── 私有辅助（均操作 snapMutex_ 保护的快照，或编排门控/状态）─────────────────
    // 追加一条日志到环形缓冲 logs_（限长，超出丢弃最早）。
    void appendLog(Severity sev, const QString& phase, const QString& msg);
    // 追加一条错误到环形缓冲 errors_（限长）。
    void appendError(const SyncError& e);
    // 设置前台状态机当前状态与百分比（pct=-1 表示“不确定/不更新百分比”）。
    void setProgress(SyncState st, int pct = -1);
    // worker progressUpdated 信号的处理：更新快照；到达终态时填 result_，并按需还闸。
    void onWorkerProgress(SyncProgress p);
    // worker errorOccurred 信号的处理：归集错误/日志；若错误足够严重且当前正处于前台
    // 操作进行态（Exporting/Capturing），把状态切到 Failed 并释放门控。
    void onWorkerError(SyncError e);
    // 若 state 为终态（Completed/Failed/Stopped）则释放前台门控（ctx_->gate）。
    void releaseGateIfTerminal(SyncState state);

    // ── 数据成员 ─────────────────────────────────────────────────────────────
    DataBridge& bridge_;  // ETL 门面（用于 setSyncActive 门控直写导入）。
    std::unique_ptr<SyncConfig> configPtr_;  // 同步配置，延迟到 initialize() 才填充。
    std::shared_ptr<SyncContext> ctx_;  // 本库共享上下文（含 gate 与各转发回调），来自 registry。
    std::unique_ptr<SyncWorker> worker_;  // 后台单写线程（真正执行所有写/收发）。
    bool initialized_ = false;            // 是否已成功 initialize()（防重复初始化）。
    QString canonicalKey_;  // ctx_ 在 registry 的键（dev+inode），析构 release() 用。

    // 以下五项构成“对外可观测快照”，统一由 snapMutex_ 保护，使只读 getter 可跨线程安全调用。
    mutable QMutex snapMutex_;  // mutable：const getter 也能加锁。
    SyncProgress progress_;     // 当前进度/状态快照。
    SyncResult result_;         // 最近一次已完成操作的最终战报。
    QList<SyncLogEntry> logs_;  // 日志环形缓冲（限长）。
    QList<SyncError> errors_;   // 错误环形缓冲（限长）。
};

}  // namespace dbridge::sync
