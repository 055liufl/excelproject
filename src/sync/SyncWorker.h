#pragma once
// ============================================================================
// SyncWorker.h — 同步引擎的“后台工作线程”声明（整个同步子系统的心脏）
// ============================================================================
//
// 【这个文件是什么】
//   SyncWorker 是一条 QThread 派生的单写者（single-writer）后台线程。它持有
//   SQLite 的“写连接”，把所有会改库的操作串行化到一个任务队列里逐个执行；同时
//   在主循环中周期性地：扫描 inbox（收）→ 应用变更 → 打包 outbox（发）→ 处理 ACK
//   → 评估对端健康度。SyncEngine（前台门面）只负责把请求投递进来并监听信号，
//   真正“动数据库”的活全部发生在这一条线程上。
//
// 【为什么要有单写者线程（设计动机）】
//   1) SQLite 在 WAL 模式下允许多读单写。把所有写集中到一条线程，天然避免了
//      多线程写竞争与“database is locked”。
//   2) SQLite session/changeset 捕获依赖于“写连接 + PREUPDATE 钩子”。该写连接
//      与其句柄（sqlite3*）必须始终归属创建它的线程——所以连接在 run() 内部创建，
//      并通过 wconnPtr_/hPtr_ 暴露给排进队列、在本线程上执行的任务闭包使用。
//   3) 串行执行让“分配 origin_seq → 捕获 changeset → 写 changelog → 提交事务”
//      这一串操作不会被打断，保证了序列号连续、不变量成立。
//
// 【在同步管线中的位置】
//   本地写入：DataBridge 导入 / ComparisonSession 保存
//        → submitImportSync / submitCaptureWriteSync（投递到本线程，session 捕获）
//        → 写 __sync_changelog（分配本地 origin_seq）
//        → broadcast()：编码 changeset「artifact」写入 outbox，经传输层发往各 peer
//   远端接收：传输层把对端 artifact 落到 inbox
//        → scanInbox() 发现 → processArtifact() 解码 → 按种类分发：
//          · Changeset        → processChangesetArtifact（按外键拓扑序 + rank 仲裁应用）
//          · SelectionPush     → processSelectionPushArtifact（按 chunk 顺序应用）
//          · BaselineRequest/Response → 冷启动/补洞 全量基线
//        → 应用成功后调度 ACK 回送发起方；ledger 标记 consumed（幂等）
//
// 【核心概念速查】
//   · origin / origin_seq：变更的“来源节点 + 该来源单调递增序号”。每条 changelog
//     都带 (origin, origin_seq)，对端据此判断“有没有漏收中间的”（gap）。
//   · stream_epoch：本进程一次启动的纪元（毫秒时间戳）。重启换 epoch，使 ACK/
//     applied_vector 的命名空间不跨进程混淆。
//   · local_seq：本地 changelog 的物理自增主键（与 origin_seq 是不同命名空间！
//     发送水位线、广播 FIFO 顺序用 local_seq；漏收判定、ACK 用 (origin, origin_seq)）。
//   · rank 仲裁：每个 origin 有唯一 rank，冲突时高 rank 胜（见 ConflictArbiter）。
//   · anti-echo（防回送）：绝不把某 peer 自己来源的变更再发回给它（见 RoutingTable）。
//   · ACK 窗口：前台 sync() 发起后，worker 记录“必须被哪些 (peer,origin,epoch,seq)
//     确认才算完成”，全部 ACK 到齐才把前台状态切到 Completed；超时则 E_SYNC_ACK_TIMEOUT。
//   · artifact（工件）：一次传输的最小文件单元（changeset / selection-push chunk /
//     baseline / ack），落在 outbox/inbox 目录，文件名自带路由信息。
//   · __sync_* 元数据表：changelog、applied_vector、outbound_ack、inbox_ledger、
//     push_progress 等，全部建在被同步的同一个 SQLite 库里。
//
// 【线程模型（务必牢记）】
//   · run()：唯一执行体，独占写连接 wconn 与句柄 h。所有 process*/broadcast*/
//     scanInbox 等私有方法都只在本线程被调用。
//   · 跨线程入口：enqueue / submitWriteSync / submitImportSync / submitCaptureWriteSync /
//     enqueueDrain / enqueueSelectionPush —— 由“调用方线程”调用，把工作打包成闭包
//     压入 taskQueue_，再由 run() 取出执行；同步版用 std::promise/future 等结果。
//   · 队列保护：queueMutex_ + queueCond_。stopRequested_ 也在锁内。
//   · ackWaiting_ / ackDeadlineMs_ 用 std::atomic：startAckWait() 在调用方线程置位，
//     run() 主循环读取，避免为这两个标志再加锁。
//
// 【协作者（皆为单一职责的小组件，本类负责编排）】
//   capture：SessionRecorder（开/封 session）、ChangelogStore（__sync_changelog）
//   apply  ：CapturedWriteTemplate（三分支写模板）、ChangesetApplier、
//            AppliedVectorStore（每来源已应用水位）、RowWinnerStore（行级冲突胜者）
//   schema ：SchemaGuard（结构指纹校验）、TableStateStore、QuarantineStore（隔离待重放）
//   transport：OutboxWriter / InboxWatcher / InboxLedger（幂等收发）/ AckChannel（批量 ACK）
//   conflict：RoutingTable（anti-echo 路由）/ ConflictArbiter（rank 仲裁）/ RebaseEngine（变基）
//   anchor ：OutboundAckStore（每 peer/origin 的 ACK 水位、发送水位）
//   peer   ：DeadPeerEvictor（滞后/失联判定与驱逐）
//   selection：SelectionResolver / FkClosureBuilder / ChunkStreamer / FrozenManifest
//   baseline：BaselineManager（全量导出/导入）
//   payload：PayloadCodec（artifact 的编解码）
// ============================================================================
#include "dbridge/DataBridge.h"
#include "dbridge/sync/SyncConfig.h"
#include "dbridge/sync/SyncSelection.h"
#include "dbridge/sync/SyncTypes.h"

#include <QMutex>
#include <QSemaphore>
#include <QSet>
#include <QThread>
#include <QWaitCondition>

#include "anchor/OutboundAckStore.h"
#include "apply/AppliedVectorStore.h"
#include "apply/CapturedWriteTemplate.h"
#include "apply/ChangesetApplier.h"
#include "apply/RowWinnerStore.h"
#include "capture/ChangelogStore.h"
#include "capture/SessionRecorder.h"
#include "payload/PayloadCodec.h"
#include "profile/ProfileSpec.h"
#include "schema/QuarantineStore.h"
#include "schema/SchemaCatalog.h"
#include "schema/SchemaGuard.h"
#include "schema/TableStateStore.h"
#include "selection/ConsistencyCache.h"
#include "transport/AckChannel.h"
#include "transport/InboxLedger.h"
#include "transport/InboxWatcher.h"
#include "transport/OutboxWriter.h"
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <sqlite3.h>

namespace dbridge::sync {

// 前置声明：以下组件由“并行开发的其它 agent”实现，这里只用到指针，故不必 include。
// （前向声明可降低头文件耦合、加快编译。其定义在 .cpp 中通过对应头被引入。）
class RoutingTable;     // anti-echo 路由：决定某条变更是否该发给某 peer
class ConflictArbiter;  // 冲突仲裁：按 origin rank + seq 决定胜者
class RebaseEngine;     // 变基：把待发 changeset 重放到已应用的 rebase buffer 之上
class DeadPeerEvictor;  // 失联对端驱逐：按滞后 seq/字节/时长三维度判定 Lagging/Dead
class InboundTableGate;  // 入站表门控：比对暂存期间对相关表的入站 artifact 进行延迟（defer）

// SyncWorker —— 单写者后台线程。
// 【职责】串行处理写任务队列；周期性扫描 inbox 工件、应用变更；把本地 changelog
//   打包成 outbox 工件广播给各 peer；处理 ACK；评估对端健康度。
// 【生命周期】构造时仅分配各组件（不碰数据库）；start() 启动线程，run() 内创建写连接
//   并完成全部 store 的初始化（成功/失败通过 initSemaphore_ + initError_ 通知）；
//   requestStop() 请求停止，run() 排空队列后退出；析构里 requestStop()+wait()。
// 【关键不变量】
//   · 写连接 wconn 及其句柄 h 自始至终只被 run() 所在线程触碰（C-03 修复的核心）。
//   · localOriginSeq_ 单调递增且“连续无洞”——事务回滚必须 rollbackOriginSeq() 退回，
//     否则对端会把跳过的 seq 误判为 gap。
// 【线程模型】见文件头。公共 enqueue*/submit* 方法是跨线程入口；其余私有方法只在 run() 线程跑。
// 写连接在 run() 内部创建，因此它永远归属 worker 线程自身（不会发生跨线程使用 QSqlDatabase）。
class SyncWorker : public QThread {
    Q_OBJECT
   public:
    // 构造：保存配置并预创建各 store/组件（但不打开数据库）。
    // inboundGate 可由外部注入（比对会话用同一个门控实例）；为空则内部自建一个。
    explicit SyncWorker(SyncConfig config, std::shared_ptr<InboundTableGate> inboundGate = nullptr);
    // 析构：请求停止并最多等待 5s 让线程优雅退出（见 .cpp）。
    ~SyncWorker() override;

    // 写任务类型：一个无参无返回的可调用对象（闭包），将在 worker 线程上执行。
    using WriteTask = std::function<void()>;

    // 把一个写任务压入队列（线程安全）。任务最终在 worker 线程上运行。
    // 仅入队并唤醒主循环，立即返回——不等待任务执行。
    void enqueue(WriteTask task);
    // 同步执行一个“拿到写连接才能干的活”并等待其结果（最多阻塞 60s）。
    // task 签名 (QSqlDatabase&, QString*)->bool。若当前就在 worker 线程上则直接执行，
    // 否则用 promise/future 投递到队列再等回。返回 task 的成功与否；失败时填 *err。
    bool submitWriteSync(const std::function<bool(QSqlDatabase&, QString*)>& task,
                         QString* err = nullptr);
    // 请求立即重扫一次 inbox（把 scanInbox 作为任务入队）。worker 未运行时直接返回。
    void requestRescan();

    // 请求停止：置 stopRequested_ 并唤醒主循环；主循环会先排空剩余任务再退出。
    void requestStop();

    // 由调用 start() 的那条线程调用：阻塞直到 worker 初始化完成（成功 release 信号量），
    // 或 timeoutMs 到期。返回 true 表示初始化成功。底层是对 initSemaphore_ 的 tryAcquire。
    bool waitForInit(int timeoutMs = 10000);

    // 初始化失败原因。仅当 waitForInit() 返回 false、或 init 期间通过此字段记录错误时非空。
    QString initError() const;

    // I-04：在 worker 线程上执行一次 Excel 导入（走 wconn + session 捕获），同步等待完成。
    // 安全性：调用方线程阻塞等待，实际写入在 worker 线程发生——绝不跨线程用同一个连接。
    // profile/catalog 是“在调用方线程上拷贝出来的快照”，worker 永不触碰 DataBridge::db_
    // 或其可变的 catalog（C-03 修复：杜绝跨线程访问主库对象）。
    // 返回 ImportResult；导入捕获的变更会写入 changelog，随后被广播给各 peer。
    ImportResult submitImportSync(const ImportOptions& opts, const QString& xlsxPath,
                                  const detail::ProfileSpec& profile,
                                  const detail::SchemaCatalog& catalog);

    // I-19：告诉 worker“前台 sync() 正在等待 ACK”。武装 ACK 等待计时：置 ackWaiting_=true，
    // 并把 ackDeadlineMs_ 设为 now+ackMaxDelayMs。若到点仍无对端 ACK，主循环发 E_SYNC_ACK_TIMEOUT。
    // 由调用方线程调用（ackWaiting_/ackDeadlineMs_ 是 atomic，无需加锁）。
    void startAckWait();
    // C-1 修复：若武装了 ACK 等待但实际没发出任何 payload（无需等待），取消之，免得前台永久挂起。
    void cancelAckWait();

    // C-05 修复：把一批 RowMutation 经 CapturedWriteTemplate（session 捕获 + changelog 封存）
    // 在 worker 线程上落库。这样“比对会话的保存”就与“本地导入”拥有完全一致的捕获+广播语义——
    // 即其改动同样会进 changelog 并被广播给 peer（区别于绕过捕获的直写）。同步等待，最多 60s。
    bool submitCaptureWriteSync(const QList<RowMutation>& mutations, const QStringList& syncTables,
                                QString* err = nullptr);

    // C-02：在 worker 线程上排入并执行一次“立即抽干”循环（扫描 inbox + 广播），同步等待结果。
    // 用于前台 sync()：把刚产生的本地变更尽快发出去，并据此构建 ACK 窗口。返回是否真正写出了工件。
    bool enqueueDrain(QString* err = nullptr);

    // C-01：在 worker 线程上排入一次“选择性推送”。
    // catalog 是调用方线程预先拍好的快照（跨线程安全）。本方法立即返回；
    // 进度/错误通过 progressUpdated/errorOccurred 信号异步上报。
    void enqueueSelectionPush(const SyncSelection& selection, const detail::SchemaCatalog& catalog);

   signals:
    // 进度更新（前台状态机/百分比等）。由 worker 线程 emit，Qt 跨线程信号会投递到连接所在线程。
    void progressUpdated(dbridge::sync::SyncProgress p);
    // 错误/警告上报（错误码见 Errors.h；severity 区分 Warning/Error/Fatal）。
    void errorOccurred(dbridge::sync::SyncError e);

   protected:
    // QThread 入口：线程体。创建写连接 → 建表/迁移 → 初始化各 store → 主事件循环。仅此一处执行。
    void run() override;

   private:
    // ── 主循环各阶段 ──────────────────────────────────────────────────────────
    void processPendingTasks();  // 取出并执行队列里的全部写任务
    void scanInbox();  // 扫描 inbox：发现工件→processArtifact；超时未补的 gap→基线回退
    bool broadcast(QString* outErr = nullptr);  // 向所有未驱逐 peer 各发一轮，再 flush ACK 通道

    // ── inbox 工件处理（按种类分发）──────────────────────────────────────────
    bool processArtifact(const QString& path);  // 读文件→ledger 幂等判定→解码→按 kind 分派
    bool processChangesetArtifact(const DecodeResult& dec,
                                  const QString& artifactName);  // 普通 changeset
    bool processSelectionPushArtifact(const DecodeResult& dec,
                                      const QString& artifactName,  // 选择性推送 chunk
                                      const QString& checksum);
    bool processBaselineRequestArtifact(const DecodeResult& dec,
                                        const QString& artifactName);  // 对端请求基线→导出回应
    bool processBaselineResponseArtifact(const DecodeResult& dec,
                                         const QString& artifactName);  // 收到基线→全量套用
    bool processAckArtifact(const QString& path, const QString& artifactName);  // 处理 ACK 工件

    // ── 广播辅助 ─────────────────────────────────────────────────────────────
    // 向单个 peer 发一轮 changelog。
    // C-01 修复：ackedEntries 收集本次成功写出的每个工件的 (peer, origin, epoch, maxOriginSeq)，
    // 这样 enqueueDrain 能据此构建“覆盖所有被广播 origin（而不仅本地 origin）”的完整 ACK 窗口。
    bool broadcastTopeer(const QString& peer, QString* outErr = nullptr,
                         QList<PendingAckEntry>* ackedEntries = nullptr);
    qint64 computePeerAckedSeq(const QString& peer);  // 查某 peer 对“本节点 origin”的已确认 seq
    qint64 nextLocalOriginSeq();  // 预递增并返回下一个本地 origin_seq
    // H-01 修复：事务中止时把 localOriginSeq_ 退回 prevSeq，避免 seq 出现空洞——
    // 否则 AppliedVectorStore::check() 会把这个洞误报成 gap。
    void rollbackOriginSeq(qint64 prevSeq);
    bool isPeerEvicted(const QString& peer);  // 该 peer 是否已被标记 pending_baseline（驱逐）
    qint64 peerLastAckMs(const QString& peer);  // 该 peer 最近一次 ACK 的时间戳（ms）
    qint64 peerLagBytes(const QString& peer,
                        qint64 afterLocalSeq);  // 该 peer 落后的字节量（用于滞后评估）
    void evaluatePeers();  // 逐 peer 评估滞后/失联，必要时驱逐或告警
    bool runBaselineFallbackFor(
        const QString& artifactName);  // 对“超时仍补不齐的 gap 工件”发起基线请求
    // M-06 修复：重放隔离区中“因 schema 升级或基线套用后变得可应用”的 payload。
    // 必须在 worker 线程、wconnPtr_ 有效时调用。
    void drainQuarantine();

    // ── 数据成员 ─────────────────────────────────────────────────────────────
    SyncConfig config_;  // 不可变的同步配置（节点身份、目录、各类阈值/间隔）

    // 指向 run() 内的局部变量（写连接与其句柄）；仅在 run() 执行期间有效。
    // init 成功后才入队的任务闭包可安全使用它们，因为 run() 返回前会把队列排空。
    QSqlDatabase* wconnPtr_ = nullptr;  // 写连接指针（worker 线程独占）
    sqlite3* hPtr_ = nullptr;  // 上述连接的底层 sqlite3* 句柄（session/changeset 用）

    // 初始化同步原语（I-02 修复）：run() 完成 init（成功或失败）后 release，
    // waitForInit() 在调用方线程 tryAcquire 等待。
    QSemaphore initSemaphore_{0};
    QString initError_;  // 初始化失败原因（成功为空）

    QMutex queueMutex_;           // 保护 taskQueue_ 与 stopRequested_
    QWaitCondition queueCond_;    // 主循环在此等待“有新任务 / 停止 / 超时”
    QList<WriteTask> taskQueue_;  // 待执行的写任务队列（FIFO）
    bool stopRequested_ = false;  // 停止标志；置位后排空队列即退出

    // 跟踪类 store（全部建在被同步的 SQLite 库内的 __sync_* 表上）
    std::unique_ptr<AppliedVectorStore> av_;
    std::unique_ptr<RowWinnerStore> rw_;  // 行级冲突“当前胜者”记录（origin rank+seq）
    std::unique_ptr<TableStateStore>
        ts_;  // __sync_table_state：每表指纹/版本（供 DiffEngine 判定是否过期）
    std::unique_ptr<ChangelogStore> clog_;  // __sync_changelog：本地与转发的 changeset 流水
    std::unique_ptr<SessionRecorder> rec_;  // SQLite session：begin/seal 捕获行级变更
    std::unique_ptr<SchemaGuard> guard_;  // 结构指纹守卫：校验来包 schema 是否与本地一致
    std::unique_ptr<ChangesetApplier>
        applier_;  // changeset 应用器（被 CapturedWriteTemplate 使用）
    std::unique_ptr<CapturedWriteTemplate>
        tpl_;  // 三分支写模板：A=入站changeset B=选择推送 C=本地写
    std::unique_ptr<OutboxWriter> outbox_;  // 把编码后的工件原子写入 outbox 目录
    std::unique_ptr<InboxLedger>
        ledger_;  // __sync_inbox_ledger：工件 seen/consumed/corrupt 幂等台账
    std::unique_ptr<InboxWatcher> watcher_;  // inbox 同步扫描器（无事件循环，见 I-10 修复）
    std::unique_ptr<AckChannel> ackChan_;  // ACK 批量通道：积攒 ACK，到期或显式 flush 一次性写出
    std::unique_ptr<OutboundAckStore>
        ackStore_;  // __sync_outbound_ack：每 (peer,origin,epoch) 的 ACK/发送水位
    std::unique_ptr<PayloadCodec> codec_;  // 工件编解码（changeset/选择推送/基线/ACK）
    std::unique_ptr<RoutingTable> routing_;  // anti-echo 路由：是否把某变更发给某 peer
    std::unique_ptr<ConflictArbiter> arbiter_;  // 冲突仲裁：rank 映射 + 高 rank 胜
    std::unique_ptr<RebaseEngine> rebaser_;  // 变基：广播前把 changeset 重放到 rebase buffer 之上
    std::unique_ptr<QuarantineStore>
        quarantine_;  // 隔离区：schema 不匹配的来包暂存，待可应用时重放
    std::unique_ptr<DeadPeerEvictor> evictor_;  // 失联对端判定与驱逐（软/硬阈值）
    std::shared_ptr<InboundTableGate>
        inboundGate_;  // 入站表门控（比对暂存期延迟相关表的应用），可外部共享

    qint64 streamEpoch_ = 0;  // 本次进程启动的 stream_epoch（毫秒时间戳）；重启即更换
    qint64 localOriginSeq_ = 0;  // 本节点下一个待分配的 origin_seq（init 时从 changelog MAX 恢复）
    // C-08：规范化后的同步表清单（配置为空时展开 = 所有用户表），保证 session 总能 attach 到东西。
    QStringList canonicalSyncTables_;

    // M-01 修复：选择性推送做“依赖剪枝”用的共享一致性缓存。
    // worker 启动时初始化；由基线套用、权威下行（center→edge）套用持续喂入。
    ConsistencyCache consistencyCache_;

    // I-16：变基缓冲区，键为 "origin/originSeq"；每次 apply_v2 后填充。
    // J-13：用“插入序列表”辅助实现正确的 LRU 淘汰（QHash 本身无序）。
    QHash<QString, QByteArray> rebaseBuffers_;
    QList<QString> rebaseBufferOrder_;  // 记录插入顺序，超容量时从最旧者开始淘汰

    // J-02：ACK 超时跟踪。用 atomic：startAckWait() 在“调用方线程”置位，worker 主循环读取，
    // 二者无需互斥锁即可安全共享。
    std::atomic<bool> ackWaiting_{false};  // 当前是否有前台 sync() 在等 ACK
    std::atomic<qint64> ackDeadlineMs_{
        0};  // ACK 超时绝对时刻（ms）；过点未到 ACK 则发 E_SYNC_ACK_TIMEOUT

    // C-04 修复：记录“在途选择性推送”的 pushId，使 processAckArtifact() 能忽略来自陈旧/无关 push 的
    // chunk ACK。仅在 worker 线程访问（enqueueSelectionPush 闭包里置位，processAckArtifact 读取——
    // 同一 worker 线程，无需额外加锁）。
    QString pendingPushId_;

    // C-01 修复：前台 sync() 被视为“完成”前必须全部被 ACK 的 (peer, origin, epoch, targetSeq)
    // 列表。 无活动 ACK 窗口时为空。仅在 worker 线程访问（enqueueDrain 里构建，processAckArtifact
    // 里消费）。
    QList<PendingAckEntry> pendingAckWindow_;

    // 正在“在途请求基线”的 gap 工件名集合（去抖：同一工件不重复发基线请求）。
    QSet<QString> baselineRequestsInFlight_;
};

}  // namespace dbridge::sync
