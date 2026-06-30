#pragma once
#include "dbridge/Export.h"
#include "dbridge/sync/SyncConfig.h"
#include "dbridge/sync/SyncSelection.h"
#include "dbridge/sync/SyncTypes.h"

#include <memory>

// ============================================================================
// ISyncEngine.h — 增量同步子系统的对外总入口（引擎接口）
// ============================================================================
//
// 【这个文件是什么】
//   同步子系统暴露给应用层的唯一抽象接口。调用方只认 ISyncEngine，不依赖任何实现
//   细节（实现是 src/sync/SyncEngine，它把请求转交给后台 SyncWorker 线程执行）。
//   通过工厂函数 createSyncEngine(bridge) 获得实例，用 unique_ptr 独占持有。
//
// 【它在整个架构中的位置】
//   ISyncEngine 是“增量同步”这半边系统的门面，与“Excel↔SQLite 批量 ETL”那半边的
//   门面 DataBridge 平级。二者共享同一个底层 SQLite 库：同步活动期间，引擎会调用
//   DataBridge::setSyncActive(true,...) 把“绕过同步的直写导入”门控住（前台/后台互斥），
//   确保所有需要传播的改动都经由 session 录制器、能被打包广播。
//
// 【典型生命周期（建立直觉）】
//   auto eng = createSyncEngine(bridge);
//   eng->initialize(cfg, &err);     // ①一次性：校验配置、attach session、起 worker
//   eng->write(mutations, &err);    // ⑩业务写入（务必走这里，而非裸 SQL）
//   eng->sync(&err);                // ②手动触发一轮收发（drain inbox/outbox）
//   ... 轮询 state()/progress()/logs()/errors() 观察进展 ...
//   auto r = eng->result();         // ⑧取最近一次操作的战报
//   eng.reset();                    // 析构：停 worker、解除直写门控、释放上下文
//
// 【方法的“前台 vs 后台”语义（贯穿全接口的关键模型）】
//   · 后台：worker 线程持续自驱地扫 inbox→apply→回 ACK→按需广播，无需调用方介入。
//   · 前台：sync()/syncSelected() 是用户显式发起的操作，受 ForegroundGate 互斥——
//     同一时刻至多一个前台操作在跑（再来一个返回 E_BUSY）。stop() 只取消当前前台
//     操作，不停后台循环。
//
// 注释风格参照 IBatchTransfer.h：方法编号 ①~⑩ + 中文逐方法详注。
// ============================================================================

namespace dbridge {
class DataBridge;
}

namespace dbridge::sync {

// ISyncEngine —— 同步引擎对外接口（纯虚）。线程语义：所有方法可从调用线程直接调用；
// 各只读 getter（④~⑧）返回的是“加锁拷贝的快照”，可在任意线程安全调用。
class DBRIDGE_EXPORT ISyncEngine {
   public:
    virtual ~ISyncEngine() = default;

    // ① initialize —— 初始化引擎（一次性，不可重复）。
    //   做什么：校验 SyncConfig；把 SQLite 路径解析为 OS 文件身份并获取/创建该库共享的
    //           SyncContext；启动 SyncWorker 后台线程并等待其完成 attach session（变更捕获）
    //           等初始化；随后接管直写门控（setSyncActive(true)）。
    //   为什么：同步依赖 SQLite session 扩展捕获变更，必须在写入前 attach；共享 SyncContext
    //           保证“同一物理库只有一个写者”。
    //   参数：config 同步配置（须 isValid()）；err 失败原因输出（可空）。
    //   返回：成功 true；失败 false 并写 err。
    //   错误模式：重复初始化 / 配置非法 / 文件身份解析失败 /
    //            E_SYNC_SESSION_UNAVAILABLE（驱动未启用 session 扩展）/
    //            E_SYNC_UNSUPPORTED_SCHEMA / worker 初始化超时。
    //   副作用：起后台线程、登记全局 SyncContext registry、阻塞直写导入。
    virtual bool initialize(const SyncConfig& config, QString* err = nullptr) = 0;

    // ② sync —— 手动触发一轮“收发”：扫 inbox 应用入站 + 打包 outbox 广播出站（drain）。
    //   语义：前台操作，受 ForegroundGate 互斥（已有前台操作在跑则返回 E_BUSY）。
    //   关键点：若本轮确有负载发出，则在广播前先 startAckWait() 武装 ACK 截止时钟，
    //           门控会一直持有到“收齐 ACK（Completed）”或“超时（Failed）”；若无任何负载
    //           可发，则立即按 Completed 收束并释放门控。
    //   返回：成功受理 true（注意：true 只代表已成功发起，最终结果需看 result()/state()）。
    //   错误模式：未初始化 / E_BUSY / E_SYNC_TRANSPORT（drain 失败）/ E_SYNC_ACK_TIMEOUT。
    virtual bool sync(QString* err = nullptr) = 0;

    // ③ stop —— 协作式中止“当前前台操作”（sync/syncSelected）。
    //   重要：只取消当前前台操作的 ACK 等待并释放门控；绝不停后台 worker 循环
    //   （inbox 扫描/apply/ACK/广播照常继续）。未初始化时直接返回 true（无操作）。
    virtual bool stop(QString* err = nullptr) = 0;

    // ④ state —— 取前台状态机当前状态的快照（线程安全）。
    virtual SyncState state() const = 0;

    // ⑤ progress —— 取进度快照（state/percent/已打包·已应用字节·变更数·冲突数；线程安全）。
    virtual SyncProgress progress() const = 0;

    // ⑥ logs —— 取日志环形缓冲的拷贝（限长保存，超出丢弃最早；线程安全）。
    virtual QList<SyncLogEntry> logs() const = 0;

    // ⑦ errors —— 取错误环形缓冲的拷贝（限长保存；线程安全）。
    virtual QList<SyncError> errors() const = 0;

    // ⑧ result —— 取“最近一次已完成操作”的最终战报（成败/终态/收发计数/各对端画像）。
    virtual SyncResult result() const = 0;

    // ⑨ syncSelected —— 上行“选择性推送”（FR-17）：只把指定的一批行推给对端。
    //   做什么：在调用线程对 schema catalog 拍快照（安全），随后把
    //           SelectionResolver→FkClosureBuilder→ChunkStreamer→OutboxWriter 整条链
    //           入队到 worker 执行（解析选择集→补全外键闭包→分片→写 outbox）。
    //   语义：前台操作，受门控互斥；空/非法选择集是“受理前”错误，不占门控即返回
    //         E_SYNC_SELECTION_EMPTY。门控持有到 ACK（Completed）或超时（Failed）。
    //   参数：selection 选择集（见 SyncSelection.h，PK 集合 + 是否带外键闭包/剪枝一致项）。
    //   错误模式：未初始化 / E_SYNC_SELECTION_EMPTY / 取 catalog 快照失败 / E_BUSY /
    //            E_SYNC_FK_CLOSURE_MISSING / E_SYNC_SELECTION_TOO_LARGE / E_SYNC_ACK_TIMEOUT。
    virtual bool syncSelected(const SyncSelection& selection, QString* err = nullptr) = 0;

    // ⑩ write —— “会被 session 捕获”的写入（业务改库的正确姿势）。
    //   做什么：把一批 RowMutation 经由 session 录制器在 worker 的写线程上同步执行，
    //           使每一行改动都被 SQLite changeset 记录，从而能在下次 sync() 时打包进
    //           outbox 并传播给对端。
    //   为什么：务必用本方法而非裸 SQL 直写——直写在同步活动期会被门控阻止
    //           （E_SYNC_WRITE_BLOCKED），即便写进去也不会被捕获、不会同步给别人。
    //   线程：内部转发到 worker 单写线程串行执行；本调用同步等待其结果。
    //   返回：全部成功 true；任一失败 false 并写 err（如 E_SYNC_APPLY_CONSTRAINT/FK）。
    virtual bool write(const QList<RowMutation>& mutations, QString* err = nullptr) = 0;
};

// 工厂函数：基于已打开的 DataBridge 创建一个同步引擎实例。
// 用 unique_ptr 返回 → 调用方独占所有权，析构即自动停 worker、释放上下文。
DBRIDGE_EXPORT std::unique_ptr<ISyncEngine> createSyncEngine(DataBridge& bridge);

}  // namespace dbridge::sync
