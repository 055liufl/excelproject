// ============================================================================
// SyncWorker.cpp — 同步引擎后台工作线程的实现（同步子系统的心脏）
// ============================================================================
//
// 【本文件实现什么】
//   SyncWorker 声明见同名头文件。这里实现它的全部行为：
//     · 线程体 run()：创建写连接、建表/迁移、初始化所有 store、跑主事件循环。
//     · 收（inbox）：scanInbox / processArtifact / process*Artifact 系列。
//     · 发（outbox）：broadcast / broadcastTopeer。
//     · ACK：processAckArtifact / startAckWait / cancelAckWait / ACK 窗口。
//     · 同步入口：submitImportSync / submitCaptureWriteSync / enqueueDrain /
//       enqueueSelectionPush（跨线程投递 + promise/future 等结果）。
//     · 健康度：evaluatePeers / 各 peerLag* 辅助；基线回退 runBaselineFallbackFor。
//
// 【阅读建议（按数据流）】
//   1) 先读 run()：理解线程从“拿连接”到“进主循环”的全过程，以及主循环如何在
//      “任务 / 收 / 发 / ACK 超时”之间分配每一轮。
//   2) 再读 processArtifact + 四个 process*Artifact：理解“收到一个工件后发生什么”。
//   3) 然后读 broadcastTopeer：理解 anti-echo 路由、发送水位线、变基与 ACK 窗口记账。
//   4) 最后读 submitImportSync / enqueueSelectionPush：理解本地写入如何被 session 捕获、
//      选择性推送如何分片成 chunk 工件。
//
// 【贯穿全文件的不变量与术语】
//   · localOriginSeq_ 必须连续：任何回滚事务的路径都要 rollbackOriginSeq() 退回，
//     否则对端把空洞误判为 gap → 触发不必要的基线回退。
//   · ledger 幂等：工件一旦 markConsumed 永不重处理；崩溃可重放且结果一致（“先持久 ACK
//     再 markConsumed”的次序就是为此服务，见 processArtifact 的 H-01 修复注释）。
//   · 所有 process*/broadcast*/scanInbox 都只在 worker 线程跑（wconnPtr_/hPtr_ 才有效）。
// ============================================================================
#include "sync/SyncWorker.h"

#include "dbridge/Errors.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>

#include "service/ImportService.h"
#include "sql/SqlBuilder.h"
#include "sync/SyncContext.h"
#include "sync/SyncDDL.h"
#include "sync/WriteTxn.h"
#include "sync/baseline/BaselineManager.h"
#include "sync/capture/SqliteHandle.h"
#include "sync/conflict/ConflictArbiter.h"
#include "sync/conflict/RebaseEngine.h"
#include "sync/conflict/RoutingTable.h"
#include "sync/diff/InboundTableGate.h"
#include "sync/peer/DeadPeerEvictor.h"
#include "sync/schema/SchemaEligibility.h"
#include "sync/selection/ChunkStreamer.h"
#include "sync/selection/ConsistencyCache.h"
#include "sync/selection/FkClosureBuilder.h"
#include "sync/selection/FrozenManifest.h"
#include "sync/selection/SelectionResolver.h"
#include <future>
#include <limits>

namespace dbridge::sync {

namespace {
// payloadTables —— 算出一个解码后的工件“涉及哪些表”。
// 【为什么需要】InboundTableGate（入站表门控）要据此判断：当前是否正处于某个比对
//   暂存窗口、且这些表被门控冻结——若是，则该工件应被延迟（defer），稍后再处理。
// 【两种工件的取表方式不同】
//   · 选择性推送（SelectionPush）：表名已显式列在 frozenEntries[*].table 中，直接收集。
//   · 普通 changeset：表名编码在二进制 changeset blob 里，需用 SQLite 的 changeset 迭代器
//     逐行读出每行所属表名。
// 【参数】dec：已解码工件。【返回】去重后的表名集合（失败/空则返回空集，调用方按“不延迟”处理）。
QSet<QString> payloadTables(const DecodeResult& dec) {
    QSet<QString> tables;
    if (dec.kind == PayloadKind::SelectionPush) {
        // 选择性推送：表名直接来自冻结清单条目。
        for (const FrozenEntry& entry : dec.selection.frozenEntries) {
            if (!entry.table.isEmpty())
                tables.insert(entry.table);
        }
        return tables;
    }

    // 普通 changeset：开一个 SQLite changeset 迭代器遍历其中每一行变更。
    // const_cast 是因为 sqlite3changeset_start 的 C 接口要 void*（非 const），
    // 但我们只读不改 blob 内容，故该转换是安全的。
    sqlite3_changeset_iter* it = nullptr;
    if (sqlite3changeset_start(
            &it, dec.changeset.size(),
            const_cast<void*>(static_cast<const void*>(dec.changeset.constData()))) != SQLITE_OK)
        return tables;  // 无法解析（如空 blob）：返回空集，等价于“无需门控延迟”

    // 逐行推进：每行通过 sqlite3changeset_op 取出该行所属表名（以及列数/操作/间接标志）。
    while (sqlite3changeset_next(it) == SQLITE_ROW) {
        const char* tableName = nullptr;
        int columns = 0;
        int op = 0;        // SQLITE_INSERT/UPDATE/DELETE（此处不关心，只取表名）
        int indirect = 0;  // 是否“间接变更”（由触发器等引起，此处亦不关心）
        if (sqlite3changeset_op(it, &tableName, &columns, &op, &indirect) == SQLITE_OK &&
            tableName) {
            tables.insert(QString::fromUtf8(tableName));
        }
    }
    sqlite3changeset_finalize(it);  // 必须 finalize 释放迭代器
    return tables;
}
}  // namespace

// 构造函数：仅“准备”所有协作组件，绝不触碰数据库。
// 设计动机：写连接必须由 worker 线程自己创建并持有（见 run()），因此这里只能做
//   “与连接无关”的对象构造与参数注入；真正的建表/初始化推迟到 run()。
SyncWorker::SyncWorker(SyncConfig config, std::shared_ptr<InboundTableGate> inboundGate)
    : config_(std::move(config)), inboundGate_(std::move(inboundGate)) {
    av_ = std::make_unique<AppliedVectorStore>();
    rw_ = std::make_unique<RowWinnerStore>();
    ts_ = std::make_unique<TableStateStore>();
    clog_ = std::make_unique<ChangelogStore>();
    rec_ = std::make_unique<SessionRecorder>();
    // M-02 fix: pass verifySchemaFingerprint flag from config.
    guard_ = std::make_unique<SchemaGuard>(config_.verifySchemaFingerprint());
    applier_ = std::make_unique<ChangesetApplier>();
    outbox_ = std::make_unique<OutboxWriter>(config_.outboxDir());
    ledger_ = std::make_unique<InboxLedger>();
    ackChan_ = std::make_unique<AckChannel>(*outbox_, config_.nodeId(), config_.ackMaxDelayMs());
    ackStore_ = std::make_unique<OutboundAckStore>();
    codec_ = std::make_unique<PayloadCodec>();
    routing_ = std::make_unique<RoutingTable>();
    arbiter_ = std::make_unique<ConflictArbiter>();
    rebaser_ = std::make_unique<RebaseEngine>();
    quarantine_ = std::make_unique<QuarantineStore>();
    evictor_ = std::make_unique<DeadPeerEvictor>();
    // 把三维滞后阈值（seq/字节/时长，各有软/硬两档）一次性灌给驱逐器。
    // 软档 → 仅告警 W_SYNC_PEER_LAGGING；硬档 → 判定 Dead 并驱逐（要求重新基线）。
    evictor_->configure(config_.peerLagSoftSeq(), config_.peerLagHardSeq(),
                        config_.peerLagSoftBytes(), config_.peerLagHardBytes(),
                        config_.peerLagSoftMs(), config_.peerLagHardMs());
    if (!inboundGate_)
        inboundGate_ = std::make_shared<InboundTableGate>();  // 未注入则自建一个独立门控
    // 注意：InboxWatcher 不在这里创建——它要在 run() 内创建，从而“活在 worker 线程上”。
}

// 析构：请求停止并等待线程退出（最多 5s）。
// 设计动机：若线程未及时退出而对象先析构，run() 里使用的成员将悬空——故必须先 join。
SyncWorker::~SyncWorker() {
    requestStop();  // 置停止标志并唤醒主循环
    wait(5000);     // QThread::wait：阻塞等待 run() 返回（超时上限 5s）
}

// 入队一个写任务（线程安全）：加锁追加，并唤醒一个等待者（主循环）。
// wakeOne 足矣——只有 run() 一条线程在 queueCond_ 上等待。
// enqueue —— 跨线程入口：把一个写任务压入队列并唤醒主循环（不等待执行）。
// 做什么：加锁追加到 taskQueue_，wakeOne 唤醒在 queueCond_ 上等待的 run() 主循环。
// 为什么 wakeOne 而非 wakeAll：只有一条 worker 线程在等，唤醒一个即可，省一次无谓唤醒。
// 线程：任意调用方线程（queueMutex_ 保护队列）。副作用：入队。复杂度：O(1)。
void SyncWorker::enqueue(WriteTask task) {
    QMutexLocker lk(&queueMutex_);
    taskQueue_.append(std::move(task));
    queueCond_.wakeOne();
}

// submitWriteSync —— 同步执行一个「需要写连接」的任务并等待其结果（最多阻塞 60s）。
// 做什么：若当前已在 worker 线程则直接执行(避免自我死锁)；否则用 promise/future 把任务
//   投递到队列、阻塞等回结果。任务签名 (QSqlDatabase&, QString*)->bool。
// 为什么要判「是否已在 worker 线程」：若在 worker 线程里又投递并 future.wait，会等待自己
//   执行队列——死锁。故同线程时直接调用。
// 参数：task 待执行的写操作；err 失败原因回填。返回：task 的成败；超时返回 false。
// 线程：任意调用方线程。错误模式：worker 未就绪 / 60s 超时 → false。
bool SyncWorker::submitWriteSync(const std::function<bool(QSqlDatabase&, QString*)>& task,
                                 QString* err) {
    if (!isRunning() || !wconnPtr_) {
        if (err)
            *err = QStringLiteral("SyncWorker not ready");
        return false;
    }
    if (QThread::currentThread() == this) {
        return task(*wconnPtr_, err);  // 已在 worker 线程：直接执行，避免投递给自己造成死锁
    }

    auto sharedPromise = std::make_shared<std::promise<QPair<bool, QString>>>();
    std::future<QPair<bool, QString>> future = sharedPromise->get_future();
    enqueue([this, task, sp = sharedPromise]() {
        QString taskErr;
        bool ok = false;
        if (wconnPtr_) {
            ok = task(*wconnPtr_, &taskErr);
        } else {
            taskErr = QStringLiteral("wconn not available in worker");
        }
        sp->set_value(qMakePair(ok, taskErr));
    });
    if (future.wait_for(std::chrono::seconds(60)) == std::future_status::timeout) {
        if (err)
            *err = QStringLiteral("submitWriteSync timed out after 60s");
        return false;
    }
    const auto result = future.get();
    if (!result.first && err)
        *err = result.second;
    return result.first;
}

// requestRescan —— 请求立即重扫一次 inbox（把 scanInbox 当作任务入队）。
// 做什么：worker 在运行时，入队一个调用 scanInbox 的闭包；未运行则直接返回。
// 为什么走入队而非直接调：scanInbox 必须在 worker 线程、用其独占写连接执行；调用方
//   线程不能直接碰 wconn。线程：任意调用方线程。复杂度：O(1)（仅入队）。
void SyncWorker::requestRescan() {
    if (!isRunning())
        return;
    enqueue([this]() { scanInbox(); });
}

// requestStop —— 请求停止：置停止标志并唤醒主循环。
// 做什么：加锁置 stopRequested_=true，wakeAll 唤醒主循环。主循环醒来后会先把队列里剩余
//   任务排空、再退出（协作式停止，不强杀）。为什么 wakeAll：确保即使主循环正阻塞在
//   queueCond_ 上也能立刻醒来响应停止。线程：任意调用方线程。复杂度：O(1)。
void SyncWorker::requestStop() {
    QMutexLocker lk(&queueMutex_);
    stopRequested_ = true;
    queueCond_.wakeAll();
}

// waitForInit —— 阻塞等待 worker 初始化完成（成功 / 超时）。
// 做什么：tryAcquire 信号量——run() 完成初始化(无论成败)后会 release 一次，本调用据此返回。
// 为什么用信号量：跨线程「等待一次性事件」的标准做法；超时可控，避免无限等待卡死调用方。
// 参数：timeoutMs 最长等待毫秒。返回：true=已初始化(信号量已 release)；false=超时。
// 线程：调用 start() 的那条线程。复杂度：O(1)（带超时的获取）。
bool SyncWorker::waitForInit(int timeoutMs) {
    return initSemaphore_.tryAcquire(1, timeoutMs);
}

// initError —— 返回初始化失败原因（成功时为空串）。线程：通常在 waitForInit 返回 false 后
//   由调用方线程读取（此时 run() 已写完 initError_ 并 release 信号量，存在 happens-before）。
QString SyncWorker::initError() const {
    return initError_;
}

// run —— QThread 线程体：整条 worker 线程的唯一执行体（贯穿其全生命周期）。
// 做什么（三大阶段）：
//   ① 起连接：在【本线程内】创建 SQLite 写连接 wconn(I-02：必须本线程创建以归属本线程)，
//      开 WAL + foreign_keys，取底层 sqlite3* 句柄；任一步失败 → 写 initError_ + release
//      信号量 + 清理后返回。
//   ② 初始化：建/迁移全部 __sync_* 表、依次 init 各 store、从 changelog MAX 恢复
//      localOriginSeq_、attach session 等；完成后(无论成败)把 wconnPtr_/hPtr_ 暴露给闭包
//      并 release initSemaphore_，唤醒 waitForInit。
//   ③ 主循环：在 queueCond_ 上「等任务/等停止/带超时」循环；每轮 processPendingTasks(执行
//      写任务) → scanInbox(收) → broadcast(发) → 检查 ACK 超时。stopRequested_ 后排空队列退出。
// 为什么写连接在此创建：SQLite session 捕获要求「连接 + 句柄」始终归属创建线程；放在 run()
//   内即天然保证 worker 线程独占，永不跨线程使用 QSqlDatabase(C-03 不变量的根基)。
// 线程：worker 线程自身(本函数即该线程)。复杂度：随生命周期运行，受任务量与轮询间隔支配。
void SyncWorker::run() {
    // --- 阶段①：在 worker 线程内创建 SQLite 写连接（I-02 修复）---
    // 【为什么必须在这里(run 内部)创建】QSqlDatabase 及其底层 sqlite3* 句柄都不能跨线程使用；
    //   SQLite 的 session/changeset 捕获又必须绑定这条写连接。把创建动作放进 run()，就让这条
    //   连接从诞生到销毁始终活在 worker 线程上，从根本上杜绝“跨线程使用同一连接”（C-03 不变量）。
    // 连接名用 UUID 保证全局唯一：Qt 以连接名索引 QSqlDatabase，重名会互相覆盖。
    QString connName =
        QStringLiteral("dbridge_sw_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSqlDatabase wconn = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
    wconn.setDatabaseName(config_.sqlitePath());
    // 忙等待 5s：多进程/多连接争锁时，SQLite 会自旋重试而非立刻报 "database is locked"。
    wconn.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    // 打开失败：写下错误、release 信号量唤醒 waitForInit（否则调用方会一直等到超时），
    // 再清理连接后退出。
    // 下方每一处 init 失败路径都重复这套“写 initError_ + release + 清理 + return”的收尾——务必成对，
    // 漏掉 release 会让 waitForInit 卡满 timeout，漏掉 removeDatabase 会泄漏 Qt 连接表条目。
    if (!wconn.open()) {
        initError_ = QStringLiteral("cannot open db: ") + wconn.lastError().text();
        initSemaphore_.release();
        QSqlDatabase::removeDatabase(connName);
        return;
    }

    // 连接级 PRAGMA：
    //   · WAL（预写日志）：允许“多读单写”并发——正是本设计把写集中到单线程、读可并行的前提。
    //   · foreign_keys=ON：应用入站 changeset 时按外键约束校验，配合拓扑序应用避免悬挂引用。
    {
        QSqlQuery q(wconn);
        q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
        q.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
    }

    // 取底层 sqlite3* 句柄：session/changeset 的 C API 只认原生句柄，QSqlDatabase 封装拿不到。
    // SqliteHandle::of 从 Qt 驱动内部挖出该句柄；拿不到说明驱动不是 QSQLITE 或版本异常。
    sqlite3* h = SqliteHandle::of(wconn);
    if (!h) {
        initError_ = QStringLiteral("cannot get sqlite3* handle");
        initSemaphore_.release();
        wconn.close();
        QSqlDatabase::removeDatabase(connName);
        return;
    }
    // 探测 session 扩展是否可用：本库的整套变更捕获都依赖它，不可用则同步根本无法开展。
    // 注：Qt 5.12 的 QSQLITE 插件加载存在陷阱（见项目记忆 project_qsqlite_session.md），
    // 若此处报 E_SYNC_SESSION_UNAVAILABLE，多半是加载了不含 session 的 SQLite 库。
    if (!SqliteHandle::sessionAvailable(h)) {
        initError_ = QStringLiteral("E_SYNC_SESSION_UNAVAILABLE");
        initSemaphore_.release();
        wconn.close();
        QSqlDatabase::removeDatabase(connName);
        return;
    }

    // 执行 DDL：建齐所有 __sync_* 元数据表（changelog / applied_vector / inbox_ledger /
    // outbound_ack / push_progress 等），它们都建在被同步的这同一个 SQLite 库里。
    // allCreateStatements() 里夹带的注释行（以 "-- " 开头）跳过，不当作 SQL 执行。
    for (const QString& stmt : ddl::allCreateStatements()) {
        if (stmt.startsWith(QStringLiteral("-- ")))
            continue;  // skip comment-only lines
        QSqlQuery q(wconn);
        if (!q.exec(stmt)) {
            initError_ = QStringLiteral("DDL: ") + q.lastError().text();
            initSemaphore_.release();
            wconn.close();
            QSqlDatabase::removeDatabase(connName);
            return;
        }
    }

    // M-04 修复：施加“只增不改”的 schema 迁移（幂等；单条失败不致命，故不检查返回值）。
    //   用于给旧库补上后续版本新增的列/表，让不同历史版本的库都能升到当前 __sync_* 结构。
    ddl::applyMigrations(wconn);

    // C-08 修复：把“空的同步表清单”展开成“全部用户表”。
    // 【为什么必须展开】SQLite session 若 attach 不到任何表，捕获会退化成“抓全库”或直接失败；
    //   把空配置显式展开成具体表清单，保证 session 总能 attach 到确定的一批表（见 rec_->begin）。
    // canonicalSyncTables_ 此后就是本 worker 全生命周期内“权威的被同步表清单”，
    //   广播过滤(p.syncTables)、指纹计算、门控判断等处处引用它。
    QString expandErr;
    canonicalSyncTables_ =
        SchemaEligibility::expandSyncTables(wconn, config_.syncTables(), &expandErr);
    if (canonicalSyncTables_.isEmpty() && !config_.syncTables().isEmpty()) {
        // 显式给了表清单但展开失败（如自省出错）：退化为原始配置清单，至少不丢用户意图。
        canonicalSyncTables_ = config_.syncTables();
    }

    // schema 资格校验：逐表检查是否满足同步前提（如必须有主键、不含不支持的类型/约束）。
    // 不合格的表收集进 rejected；有任一不合格即判 E_SYNC_UNSUPPORTED_SCHEMA 并中止初始化——
    // 宁可启动即失败，也不要带着无法正确同步的表跑起来后再出难查的数据错乱。
    QStringList rejected;
    QString eligErr;
    if (!SchemaEligibility::verify(wconn, canonicalSyncTables_, &rejected, &eligErr)) {
        initError_ = QStringLiteral("E_SYNC_UNSUPPORTED_SCHEMA: ") + eligErr;
        if (!rejected.isEmpty())
            initError_ += QStringLiteral("; rejected: ") + rejected.join(QLatin1Char(','));
        initSemaphore_.release();
        wconn.close();
        QSqlDatabase::removeDatabase(connName);
        return;
    }

    // H-03 fix: exercise the session API against the actual DB handle so that symbol
    // mismatches or disabled PREUPDATE hooks are caught at init time, not at first write.
    {
        QString sessionErr;
        if (!SqliteHandle::exerciseSession(h, canonicalSyncTables_, &sessionErr)) {
            initError_ = QStringLiteral("E_SYNC_SESSION_UNAVAILABLE: session self-check failed: ") +
                         sessionErr;
            initSemaphore_.release();
            wconn.close();
            QSqlDatabase::removeDatabase(connName);
            return;
        }
    }

    // 把写连接与句柄的地址暴露给成员指针，供“排进队列、在本线程执行的任务闭包”使用。
    // 【生命周期铁律】wconn 是 run() 的局部变量，wconnPtr_/hPtr_ 仅在 run() 执行期间有效；
    //   run() 返回前会先把队列排空并把这两个指针置空（见文件末尾 teardown），故闭包读到它们
    //   非空时一定处于 run() 生命周期内、且就在 worker 线程上——绝不会悬空或跨线程。
    wconnPtr_ = &wconn;
    hPtr_ = h;

    // --- 阶段②：在 worker 线程上一次性初始化各 store（全部把表建在本库的 __sync_* 上）---
    // streamEpoch_ = 本次进程启动纪元（毫秒时间戳）。重启即换新纪元，使 ACK / applied_vector
    //   的 (origin, epoch, seq) 命名空间不跨进程混淆（旧纪元的残留 ACK 不会误配到新纪元）。
    streamEpoch_ = QDateTime::currentMSecsSinceEpoch();

    // 下面每个 store 的 init 都遵循同一失败收尾：写 initError_ → release 信号量 → 置空指针 →
    // 关连接 → removeDatabase → return。任一 store 初始化失败都会导致整条 worker 启动失败。
    QString initErr;
    if (!av_->init(wconn, &initErr)) {
        initError_ = initErr;
        initSemaphore_.release();
        wconnPtr_ = nullptr;
        hPtr_ = nullptr;
        wconn.close();
        QSqlDatabase::removeDatabase(connName);
        return;
    }
    if (!rw_->init(wconn, &initErr)) {
        initError_ = initErr;
        initSemaphore_.release();
        wconnPtr_ = nullptr;
        hPtr_ = nullptr;
        wconn.close();
        QSqlDatabase::removeDatabase(connName);
        return;
    }
    if (!ts_->init(wconn, &initErr)) {
        initError_ = initErr;
        initSemaphore_.release();
        wconnPtr_ = nullptr;
        hPtr_ = nullptr;
        wconn.close();
        QSqlDatabase::removeDatabase(connName);
        return;
    }
    if (!clog_->init(wconn, &initErr)) {
        initError_ = initErr;
        initSemaphore_.release();
        wconnPtr_ = nullptr;
        hPtr_ = nullptr;
        wconn.close();
        QSqlDatabase::removeDatabase(connName);
        return;
    }
    // 从 changelog 恢复 localOriginSeq_ 水位：取本节点(origin=nodeId)已写过的最大 origin_seq。
    // 【为什么必须恢复】origin_seq 要“跨进程重启仍单调连续”。若重启后从 0 重新计，就会重复
    //   分配旧序号，对端会把冲突的 seq 视为错乱。COALESCE(...,0) 让首次运行(无记录)从 0 起，
    //   于是 nextLocalOriginSeq() 首个返回 1。
    {
        QSqlQuery q(wconn);
        q.prepare(
            QStringLiteral("SELECT COALESCE(MAX(origin_seq), 0) "
                           "FROM __sync_changelog WHERE origin = ?"));
        q.addBindValue(config_.nodeId());
        if (q.exec() && q.next())
            localOriginSeq_ = q.value(0).toLongLong();
    }
    if (!ledger_->init(wconn, &initErr)) {
        initError_ = initErr;
        initSemaphore_.release();
        wconnPtr_ = nullptr;
        hPtr_ = nullptr;
        wconn.close();
        QSqlDatabase::removeDatabase(connName);
        return;
    }
    if (!ackStore_->init(wconn, &initErr)) {
        initError_ = initErr;
        initSemaphore_.release();
        wconnPtr_ = nullptr;
        hPtr_ = nullptr;
        wconn.close();
        QSqlDatabase::removeDatabase(connName);
        return;
    }
    if (!quarantine_->init(wconn, &initErr)) {
        initError_ = initErr;
        initSemaphore_.release();
        wconnPtr_ = nullptr;
        hPtr_ = nullptr;
        wconn.close();
        QSqlDatabase::removeDatabase(connName);
        return;
    }

    // 初始化“一致性缓存”（M-01 修复）：记录“本地某行已与权威端(center/基线)一致”的指纹。
    // 【它有什么用】选择性推送做“依赖剪枝”时，若某外键依赖行已被证明与中心端一致，就不必
    //   再把它塞进推送闭包，省带宽。基线套用、权威下行(center→edge)会持续往这里“盖章”喂数据。
    // 初始化失败按非致命处理：退化成空缓存——只是没有剪枝收益，功能仍完全正确，故不中止启动。
    {
        QString cacheErr;
        if (!consistencyCache_.init(wconn, config_.consistencyCacheDurable(), &cacheErr)) {
            // Non-fatal: degrade to empty cache (no pruning benefit, but no functional breakage).
            // Log but don't abort initialization.
        }
    }

    // 计算本地 schema 指纹并交给 SchemaGuard 记为“本地基准”。
    // 指纹 = 对被同步表结构的摘要；收到入站包时 guard_->verifyPayload 拿包里的指纹与它比对，
    //   不一致说明两端结构不同步 → 该包进隔离区等本地 schema 升级后再重放
    //   （见 processChangesetArtifact）。
    QString schemaFp = SchemaGuard::computeFingerprint(wconn, canonicalSyncTables_);
    guard_->setLocal(config_.schemaVersion(), schemaFp);

    // 配置路由与仲裁：
    //   routing_：告知“我是谁 + 有哪些 peer”，据此做 anti-echo（绝不把某来源的变更发回其来源）。
    //   arbiter_：装入全体 origin 的 rank 映射，冲突时“高 rank 胜”的裁决依据。
    routing_->configure(config_.nodeId(), config_.peerNodes());
    arbiter_->setRankMap(config_.allRanks());

    // 各 store 就绪后，才构造 CapturedWriteTemplate（“捕获写模板”）。它统一封装三类写入的
    //   “事务 + session 捕获 + 冲突仲裁 + 封存 changelog”流程，是所有会改库操作的收口：
    //   分支 A=应用入站 changeset；分支 B=应用选择性推送；分支 C=本地写（导入/比对保存）。
    //   注意它要吃到写连接 wconn 与句柄 h，所以只能在这里（run 内、句柄有效时）构造。
    tpl_ = std::make_unique<CapturedWriteTemplate>(wconn, h, *av_, *rw_, *ts_, *clog_, *rec_,
                                                   *guard_, *applier_, config_.nodeId(),
                                                   streamEpoch_, schemaFp, config_.schemaVersion());

    // 在本线程创建 InboxWatcher。
    // I-10 fix: InboxWatcher no longer uses QFileSystemWatcher/QTimer (which require an event
    // loop).  It now exposes a synchronous scan() method called explicitly in scanInbox().
    watcher_ = std::make_unique<InboxWatcher>(config_.inboxDir(), wconn, *ledger_);

    // L-01 fix: publish the canonical sync table list to the shared SyncContext so other
    // modules (ComparisonSession, BatchTransfer, diagnostics) can read the same expanded set.
    // H-13 fix: also publish the active stream epoch so the ComparisonSession factory reads
    // __sync_table_state at the correct epoch (instead of a 0 placeholder).
    // H-01 fix: persist contextUuid to __sync_context_meta so it survives process restarts.
    // M-03 fix: use the resolved main-library path from the open connection (PRAGMA database_list)
    // instead of the raw config path, so URI/relative/alias paths find the same SyncContext.
    // 把本 worker 算出的“权威运行参数”回写进共享的 SyncContext，供同库其它模块统一读取：
    //   · canonicalSyncTables_：展开后的同步表清单（ComparisonSession/BatchTransfer 要读同一份）；
    //   · streamEpoch_：当前纪元（H-13：让比对会话按正确纪元读 __sync_table_state，而非占位 0）；
    //   · contextUuid：持久化到 __sync_context_meta，使其跨进程重启后仍是同一身份（H-01）。
    // M-03：SyncContext 的键必须是“同一个 OS 文件”的身份。用打开连接实际解析出的 main 库路径
    //   （PRAGMA database_list）而非原始配置串，才能让 URI 路径/相对路径/SQLite 别名
    //   都归一到同一 ctx。
    {
        // 从已打开的写连接解出真正的 main 库文件路径（归一化为绝对路径）。
        QString resolvedPath = config_.sqlitePath();
        {
            QSqlQuery pragmaQ(wconn);
            if (pragmaQ.exec(QStringLiteral("PRAGMA database_list"))) {
                while (pragmaQ.next()) {
                    // database_list 每行：seq(0) / name(1) / file(2)。只认 name=="main" 的主库。
                    if (pragmaQ.value(1).toString() == QLatin1String("main")) {
                        const QString p = pragmaQ.value(2).toString();
                        if (!p.isEmpty())
                            resolvedPath = QFileInfo(p).absoluteFilePath();
                        break;
                    }
                }
            }
        }
        auto ctx = SyncContextRegistry::instance().getExisting(resolvedPath);
        if (ctx) {
            if (!canonicalSyncTables_.isEmpty())
                ctx->canonicalSyncTables = canonicalSyncTables_;
            ctx->streamEpoch = streamEpoch_;

            // H-01 fix: read-or-write context UUID — adopt the DB-stored value on restart.
            {
                QString uuidErr;
                if (!SyncContextRegistry::ensureContextUuid(wconn, &ctx->contextUuid, &uuidErr)) {
                    initError_ = QStringLiteral("E_SYNC_CONTEXT_UUID_DB_ERROR: ") + uuidErr;
                    initSemaphore_.release();
                    wconnPtr_ = nullptr;
                    hPtr_ = nullptr;
                    wconn.close();
                    QSqlDatabase::removeDatabase(connName);
                    return;
                }
            }
        }
    }

    // 启动时清一次隔离区（H-16 修复）：把此前因“本地 schema 版本过低”而被隔离、但按当前
    //   schema 版本已可应用的 payload 捞出来重放（典型场景：重启后本地 schema 已升级到位，或
    //   刚套过基线）。drainReady() 取出成熟条目；重放仍失败者会被重新隔离。
    // 【为什么只在 init 清一次、而非每次 scan】worker 的 schema 版本在其整个生命周期内固定不变，
    //   运行期反复清只会把“仍不兼容”的条目反复搅动却无进展；真正会“解锁”隔离条目的时机是
    //   schema 升级(需重启)或套用基线(那时 processBaselineResponseArtifact 会另行
    //   drainQuarantine)。
    {
        const auto readyPayloads = quarantine_->drainReady(wconn, config_.schemaVersion());
        for (const auto& entry : readyPayloads) {
            const qint64 qid = entry.first;
            const QByteArray& payload = entry.second;
            DecodeResult qdec;
            QString qerr;
            if (codec_->decode(payload, &qdec, &qerr) && qdec.kind == PayloadKind::Changeset) {
                if (processChangesetArtifact(qdec, QString()))
                    quarantine_->markReplayed(wconn, qid);
            }
        }
    }

    // 初始化全部成功：release 信号量，唤醒在 waitForInit() 上阻塞的 initialize() 调用方。
    // 走到这里，wconnPtr_/hPtr_ 与所有 store 均已就绪，主循环可以安全开跑。
    initSemaphore_.release();

    qint64 lastBroadcastMs = 0;  // 上次后台广播的时刻，用于按 broadcastIntervalMs 节流

    // --- 阶段③：主事件循环（worker 线程的常驻循环，直到收到停止请求）---
    // 每一轮做四件事：等活/超时 → 执行队列写任务 → 扫 inbox（收） → 到点则 broadcast（发） →
    //   检查前台 ACK 是否超时。注意本循环用 QWaitCondition 而非 Qt 事件循环，故 QTimer/
    //   QFileSystemWatcher 之类依赖事件循环的机制在这里不生效（这正是 I-10 改用同步 scan 的原因）。
    while (true) {
        // 等待“有新任务 / 收到停止 / 定时到点”三者之一。为避免睡过头错过定时工作，需算出
        // “最紧”的等待上限（下面三个截止点取最小）。
        {
            QMutexLocker lk(&queueMutex_);
            if (taskQueue_.isEmpty() && !stopRequested_) {
                // Compute the tightest wait interval so we never sleep past:
                //   (a) broadcastIntervalMs — background broadcast cadence
                //   (b) ACK deadline — E_SYNC_ACK_TIMEOUT must fire on time
                //   (c) ACK channel deadline — pending ACKs must be flushed within ackMaxDelayMs
                // M-03 fix: also incorporate ackChan_->nextDeadlineMs() so ACK flush is not
                // delayed by the full broadcastIntervalMs when ACKs are already pending.
                // (a) 后台广播节奏：默认按 broadcastIntervalMs 周期发一轮。这是等待上限的基准值。
                qint64 waitMs = config_.broadcastIntervalMs();
                const qint64 nowForWait = QDateTime::currentMSecsSinceEpoch();
                // (b) 前台 sync() 的 ACK 超时：若正在等 ACK，则不能睡过 ackDeadlineMs_，否则
                //     E_SYNC_ACK_TIMEOUT 无法准时触发。remaining<=0 说明已到点，置 1ms 立即醒。
                if (ackWaiting_.load()) {
                    const qint64 remaining = ackDeadlineMs_.load() - nowForWait;
                    if (remaining > 0 && remaining < waitMs)
                        waitMs = remaining;
                    else if (remaining <= 0)
                        waitMs = 1;  // already expired; wake immediately
                }
                // (c) ACK 通道 flush 截止（M-03 修复）：AckChannel 会攒 ACK 到 ackMaxDelayMs
                // 再批量写出；
                //     若不把它纳入等待上限，攒着的 ACK 可能被拖到整整一个 broadcastIntervalMs
                //     才发出， 让对端迟迟收不到确认。max() 表示当前没有待 flush 的
                //     ACK，无需据此缩短等待。
                if (ackChan_) {
                    const qint64 ackDeadline = ackChan_->nextDeadlineMs();
                    if (ackDeadline != std::numeric_limits<qint64>::max()) {
                        const qint64 remaining = ackDeadline - nowForWait;
                        if (remaining > 0 && remaining < waitMs)
                            waitMs = remaining;
                        else if (remaining <= 0)
                            waitMs = 1;
                    }
                }
                // 带超时地在条件变量上等待：期间 enqueue/requestStop 的 wakeOne/wakeAll
                // 会提前唤醒； 无人唤醒则最迟 waitMs 后自行醒来去做定时工作。wait 会临时释放
                // queueMutex_，返回时重持。
                queueCond_.wait(&queueMutex_, static_cast<ulong>(waitMs > 0 ? waitMs : 1));
            }
            // 协作式停止：仅当“已请求停止且队列已排空”时才退出——保证收尾前把剩余写任务做完，
            // 不丢已投递的工作（requestStop 只置标志，真正退出由主循环自己判定）。
            if (stopRequested_ && taskQueue_.isEmpty())
                break;
        }

        processPendingTasks();  // ① 执行本轮排队的全部写任务（导入/捕获写/drain 等）
        scanInbox();  // ② 收：扫 inbox 应用入站工件、对超时 gap 触发基线回退

        // ③ 发：到达广播周期才广播一轮（节流，避免空转刷小文件）。
        qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (nowMs - lastBroadcastMs >= config_.broadcastIntervalMs()) {
            broadcast();
            lastBroadcastMs = nowMs;
        }

        // I-19: ACK timeout — emit E_SYNC_ACK_TIMEOUT if foreground sync() is waiting
        // and no ACK has arrived within ackMaxDelayMs.
        if (ackWaiting_ && nowMs >= ackDeadlineMs_) {
            ackWaiting_ = false;
            emit errorOccurred({QLatin1String(err::E_SYNC_ACK_TIMEOUT), Severity::Error,
                                QStringLiteral("sync"), config_.nodeId(),
                                QStringLiteral("ACK not received within ackMaxDelayMs=") +
                                    QString::number(config_.ackMaxDelayMs())});
        }
    }

    // --- 阶段④：退出前收尾（跳出主循环后，仍在 worker 线程上执行）---
    // I-10：不再需要 watcher_->stop()——已改为同步 scan()，没有 QTimer/QFileSystemWatcher 要拆。
    // 退出前把 AckChannel 里最后攒着的 ACK 强制 flush 出去，避免刚应用完的入站变更没来得及回确认。
    QString ackErr;
    if (!ackChan_->flush(*codec_, &ackErr)) {
        emit errorOccurred({err::E_SYNC_TRANSPORT, Severity::Warning, QStringLiteral("ack"),
                            config_.nodeId(), ackErr});
    }

    // 销毁次序讲究：先毁掉持有 wconn/h 的对象与暴露指针，最后才关连接、移除连接。
    //   tpl_/watcher_ 内部握着 wconn 与句柄，必须先 reset；
    //   wconnPtr_/hPtr_ 置空，让任何“万一残留”的闭包读到空指针即安全跳过（不会用到已关连接）。
    tpl_.reset();
    watcher_.reset();
    wconnPtr_ = nullptr;
    hPtr_ = nullptr;
    wconn.close();
    // 关键：把局部 wconn 置为空 QSqlDatabase，释放这最后一个引用，使 removeDatabase 时引用计数
    //   降为 1（仅 Qt 连接表自身持有），从而不触发 "connection still in use" 警告。
    wconn = QSqlDatabase();  // release reference so removeDatabase sees refcount=1, no warning
    QSqlDatabase::removeDatabase(connName);
}

// processPendingTasks —— 取出并执行队列里当前的全部写任务。
// 做什么：加锁把 taskQueue_ 整体 swap 到局部 tasks(锁内只做交换，锁外执行)，再逐个调用。
// 为什么 swap 后在锁外执行：任务可能耗时(写库)，若持锁执行会阻塞 enqueue 投递新任务；
//   swap 出来后立即放锁，把执行放到锁外，最大化并发投递吞吐。
// 线程：worker(主循环每轮调用)。副作用：执行各任务的全部副作用。复杂度：O(任务数 × 各任务)。
void SyncWorker::processPendingTasks() {
    QList<WriteTask> tasks;
    {
        QMutexLocker lk(&queueMutex_);
        tasks.swap(taskQueue_);  // 锁内只交换，立即放锁
    }
    for (auto& task : tasks) {
        task();  // 锁外执行，不阻塞其它线程投递
    }
}

// scanInbox —— 扫描 inbox：发现新工件并逐个处理，对超时未补齐的 gap 触发基线回退。
// 做什么：① 用 watcher_->scan(同步扫描，I-10：不依赖事件循环)找出新 .ready 工件；
//   ② 并入 ledger 里「已 seen 但尚未处理」的工件(防丢)；③ 逐个 processArtifact；
//   ④ 取「超时仍未推进」的工件，仅对其中的 changeset 类(H-03：只有它代表严格 seq 空洞)
//   发 E_SYNC_GAP 并 runBaselineFallbackFor 兜底，选择性推送 chunk/门控延迟工件各有自己的
//   重试语义，不走 changeset gap 处理。
// 为什么用同步 scan 而非信号：worker 主循环是 QWaitCondition::wait 而非事件循环，QTimer/
//   QFileSystemWatcher 信号根本不会触发(I-10 修复)。
// 线程：worker(主循环每轮调用)。前置：wconnPtr_ 有效。副作用：可能应用变更、发 gap 告警、
//   发起基线请求。复杂度：O(目录条目数 + 待处理工件数)。
void SyncWorker::scanInbox() {
    if (!wconnPtr_)
        return;

    // I-10 fix: use synchronous scan() instead of signal-driven InboxWatcher.
    // Direct scan (no event loop needed).
    QStringList found = watcher_->scan(*wconnPtr_);

    // Also add ledger pending-seen artifacts not yet seen by the directory scan.
    QStringList pending = ledger_->pendingSeen(*wconnPtr_);
    for (const QString& name : pending) {
        QString full = config_.inboxDir() + QDir::separator() + name;
        if (!found.contains(full))
            found.append(full);
    }

    for (const QString& path : found)
        processArtifact(path);

    // M-1 fix: gap timeout is now configurable via SyncConfig (default 30 s).
    QStringList stale = ledger_->stalePending(*wconnPtr_, config_.gapTimeoutMs());
    // H-03 fix: only changeset artifacts represent strict sequence gaps that require
    // E_SYNC_GAP and baseline fallback.  Selection push chunks waiting for predecessor
    // chunks and gate-deferred artifacts have their own timeout/retry semantics and
    // must not trigger changeset gap handling.
    QStringList staleChangeset;
    for (const QString& sn : stale) {
        if (sn.contains(QLatin1String("__changeset__")))
            staleChangeset.append(sn);
    }
    if (!staleChangeset.isEmpty()) {
        emit errorOccurred(
            {err::E_SYNC_GAP, Severity::Error, QStringLiteral("scanInbox"), QString(),
             QStringLiteral("Changeset gap unresolved after %1ms; %2 artifact(s) pending. "
                            "Baseline fallback required.")
                 .arg(config_.gapTimeoutMs())
                 .arg(staleChangeset.size())});
        for (const QString& artifactName : staleChangeset)
            runBaselineFallbackFor(artifactName);
    }
}

// processArtifact —— 处理单个 inbox 工件：幂等判定 → 解码 → 按种类分派。
// 做什么：① 查 ledger 幂等：已 consumed/corrupt 的直接跳过(不重复处理)；② 读文件、解码；
//   ③ 按 dec.kind 分派到 process{Changeset,SelectionPush,BaselineRequest,BaselineResponse,
//   Ack}Artifact；④ 处理成功后 markConsumed(配合「先持久 ACK 再 markConsumed」的次序，
//   保证崩溃可重放且结果一致)。ACK 工件(.ack)也入 ledger，避免无限重处理(M-07)。
// 为什么幂等是核心：传输层不保证 exactly-once，同一工件可能被多次发现；ledger 让「应用一次」
//   成为不变量——这是整个收方向正确性的基石。
// 参数：path 工件完整路径。返回：是否成功处理(并已 markConsumed)。
// 线程：worker。副作用：应用变更/回 ACK/写 ledger。复杂度：O(工件大小 + 应用成本)。
bool SyncWorker::processArtifact(const QString& path) {
    QFileInfo fi(path);
    QString name = fi.fileName();

    // M-07 fix: ACK artifacts also enter the inbox ledger to avoid infinite re-processing.
    if (name.endsWith(QStringLiteral(".ack"))) {
        LedgerStatus ackSt = ledger_->status(*wconnPtr_, name);
        if (ackSt == LedgerStatus::Consumed)
            return true;  // already processed
        ledger_->markSeen(*wconnPtr_, name, nullptr);
        const bool ok = processAckArtifact(path, name);
        if (ok) {
            ledger_->markConsumed(*wconnPtr_, name, nullptr);
        } else {
            // M-01 fix: mark corrupt so this ACK artifact is not re-processed on subsequent scans.
            ledger_->markCorrupt(*wconnPtr_, name, nullptr);
            emit errorOccurred({err::E_SYNC_PAYLOAD_CORRUPT, Severity::Warning,
                                QStringLiteral("ack"), QString(),
                                QStringLiteral("ACK artifact could not be parsed: ") + name});
        }
        return ok;
    }

    // Check ledger
    LedgerStatus st = ledger_->status(*wconnPtr_, name);
    if (st == LedgerStatus::Consumed || st == LedgerStatus::Corrupt)
        return true;

    // Read artifact
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        // M-02 fix: the .ready marker exists but the payload file is missing or unreadable.
        // This is a transport-layer failure, not a sequence gap — mark corrupt so stalePending()
        // does not include it and trigger a spurious E_SYNC_GAP / baseline fallback.
        ledger_->markCorrupt(*wconnPtr_, name, nullptr);
        emit errorOccurred({err::E_SYNC_TRANSPORT, Severity::Warning, QStringLiteral("inbox"),
                            QString(),
                            QStringLiteral("Cannot open artifact (transport error): ") + path});
        return false;
    }
    QByteArray data = f.readAll();
    f.close();

    // Decode
    DecodeResult dec;
    QString decErr;
    if (!codec_->decode(data, &dec, &decErr)) {
        QString markErr;
        ledger_->markCorrupt(*wconnPtr_, name, &markErr);
        // J-08: Move corrupt artifact to quarantineDir so it's not re-scanned.
        if (!config_.quarantineDir().isEmpty()) {
            QDir qDir(config_.quarantineDir());
            qDir.mkpath(QStringLiteral("."));
            QFile::copy(path, qDir.filePath(name));
            QFile::remove(path);
        }
        emit errorOccurred(
            {err::E_SYNC_PAYLOAD_CORRUPT, Severity::Error, "inbox", dec.header.origin, decErr});
        return false;
    }

    ledger_->markSeen(*wconnPtr_, name, nullptr);
    if (inboundGate_ && inboundGate_->shouldDefer(payloadTables(dec))) {
        return false;
    }

    bool ok = false;
    if (dec.kind == PayloadKind::Changeset)
        ok = processChangesetArtifact(dec, name);
    else if (dec.kind == PayloadKind::SelectionPush) {
        const QString checksum =
            QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
        ok = processSelectionPushArtifact(dec, name, checksum);
    } else if (dec.kind == PayloadKind::BaselineRequest) {
        ok = processBaselineRequestArtifact(dec, name);
    } else if (dec.kind == PayloadKind::BaselineResponse) {
        ok = processBaselineResponseArtifact(dec, name);
    }

    if (ok) {
        if (dec.kind == PayloadKind::Changeset) {
            // H-01 fix: schedule + flush ACK BEFORE marking consumed so a crash between
            // apply-commit and markConsumed leaves the artifact in 'seen' state and causes
            // an idempotent re-apply + re-ACK on restart, rather than silently losing the ACK.
            // C-05 fix: ACK the physical sender (senderPeer), not the business origin.
            ChangesetAck ack;
            ack.origin = dec.header.origin;
            ack.streamEpoch = dec.header.streamEpoch;
            ack.appliedSeq = dec.header.originSeq;
            ack.toPeer =
                dec.header.senderPeer.isEmpty() ? dec.header.origin : dec.header.senderPeer;
            QString ackErr;
            if (!ackChan_->scheduleChangesetAck(ack, *codec_, &ackErr)) {
                emit errorOccurred({err::E_SYNC_TRANSPORT, Severity::Warning, QStringLiteral("ack"),
                                    dec.header.origin, ackErr});
            } else {
                // Force flush so ACK file is durable on disk before we persist 'consumed'.
                QString flushErr;
                if (!ackChan_->flush(*codec_, &flushErr)) {
                    emit errorOccurred({err::E_SYNC_TRANSPORT, Severity::Warning,
                                        QStringLiteral("ack"), dec.header.origin, flushErr});
                }
            }
        }
        ledger_->markConsumed(*wconnPtr_, name, nullptr);
    }
    return ok;
}

// processChangesetArtifact —— 应用一个普通 changeset 工件（收方向最常走的路径）。
// 做什么：校验 schema 兼容(不兼容则进隔离区等升级)→ 经 InboundTableGate 判断是否需延迟
//   (比对暂存期)→ 按外键拓扑序 + rank 冲突仲裁，用 CapturedWriteTemplate 的「入站分支」
//   应用 changeset → 推进该 origin 的 AppliedVector 水位 → 调度回送 ACK 给发起方。
// 关键次序(H-01)：先 schedule + flush ACK，再 markConsumed——若在「apply 提交」与
//   「markConsumed」之间崩溃，工件停留在 'seen'，重启后会幂等重放 + 重发 ACK，而不会静默
//   丢掉 ACK(否则发送方永远等不到确认)。
// 参数：dec 已解码工件(name 形参未用)。返回：是否成功应用。
// 线程：worker。错误模式：schema 不符 → 隔离；门控命中 → 延迟；应用失败 → 回滚 + 退 seq。
bool SyncWorker::processChangesetArtifact(const DecodeResult& dec, const QString& /*name*/) {
    const PayloadHeader& hdr = dec.header;

    // Schema guard
    QString schemaErr;
    if (!guard_->verifyPayload(hdr.schemaVer, hdr.schemaFingerprint, &schemaErr)) {
        // C-5 fix: quarantine the FULL payload bytes (not just dec.changeset) so that
        // drainReady() can replay via codec_->decode() on the complete encoded artifact.
        // Previously this stored only the raw changeset which would fail codec decoding on replay.
        quarantine_->quarantine(*wconnPtr_, hdr.origin, hdr.originSeq, hdr.streamEpoch,
                                hdr.schemaVer, dec.rawPayload, nullptr);
        emit errorOccurred(
            {err::E_SYNC_SCHEMA_MISMATCH, Severity::Warning, "apply", hdr.origin, schemaErr});
        return false;
    }

    // Apply via CapturedWriteTemplate Branch A
    WriteParams p;
    p.kind = WriteKind::InboundChangeset;
    p.origin = hdr.origin;
    p.epoch = hdr.streamEpoch;
    p.seq = hdr.originSeq;
    p.schemaVer = hdr.schemaVer;
    p.schemaFp = hdr.schemaFingerprint;
    p.originRank = config_.originRank(hdr.origin);
    p.changesetBlob = dec.changeset;

    // H-01 fix: Edge nodes receiving a changeset from the center node treat it as
    // authoritative so the center always wins (center→edge convergence guarantee).
    const QString& senderPeer = hdr.senderPeer.isEmpty() ? hdr.origin : hdr.senderPeer;
    p.authoritative = (config_.role() == NodeRole::Edge && !config_.centerNodeId().isEmpty() &&
                       senderPeer == config_.centerNodeId());

    // H-02 fix: propagate the node's syncTables allow-list so filterCb rejects tables
    // outside the configured subset (previously syncTables was left empty = accept all).
    p.syncTables = canonicalSyncTables_;

    // M-01 fix: propagate the configured conflict policy so the apply path can honour
    // TargetWins/Manual instead of always defaulting to SourceWins.
    p.conflictPolicy = config_.conflictPolicy();

    WriteResult res = tpl_->execute(p);
    if (!res.ok) {
        // H-03 fix: GAP_PENDING is not a hard error — keep artifact in ledger as 'seen' so the
        // three-time-rescan logic can retry. The E_SYNC_GAP warning fires via stalePending() later.
        if (res.errorCode == QLatin1String("GAP_PENDING"))
            return false;  // caller must NOT markConsumed; ledger stays 'seen'
        emit errorOccurred(
            {res.errorCode, Severity::Error, QStringLiteral("apply"), hdr.origin, res.errorMsg});
        return false;
    }

    // H-03(table_state): surface stale warning so callers know DiffEngine data may be stale.
    if (!res.tableStateStaleSince.isEmpty()) {
        emit errorOccurred(
            {err::W_SYNC_UNTRACKED_CHANGE, Severity::Warning, QStringLiteral("table_state"),
             hdr.origin, QStringLiteral("table_state update failed: ") + res.tableStateStaleSince});
    }

    // I-16/J-13: Store rebase buffer; use insertion-ordered list for correct LRU eviction.
    if (!res.applyOutcome.rebaseBuffer.isEmpty()) {
        QString key = hdr.origin + QLatin1Char('/') + QString::number(hdr.originSeq);
        if (!rebaseBuffers_.contains(key)) {
            rebaseBufferOrder_.append(key);
        }
        rebaseBuffers_.insert(key, res.applyOutcome.rebaseBuffer);
        // Evict oldest entry when over capacity
        constexpr int kMaxRebaseBuffers = 500;
        while (rebaseBuffers_.size() > kMaxRebaseBuffers && !rebaseBufferOrder_.isEmpty()) {
            QString oldest = rebaseBufferOrder_.takeFirst();
            rebaseBuffers_.remove(oldest);
        }
    }

    // J-02/I-19: Inbound apply is NOT a typed ACK — don't clear ackWaiting_ here.
    // ACK-wait is only cleared by processAckArtifact (typed ACK) or timeout.

    // Update local origin seq if this is our own changeset echoed back
    if (hdr.origin == config_.nodeId() && hdr.originSeq > localOriginSeq_)
        localOriginSeq_ = hdr.originSeq;

    return true;
}

// processSelectionPushArtifact —— 应用一个「选择性推送」chunk 工件。
// 做什么：校验本 chunk 属于期望的 push 且按序(前驱 chunk 已到)→ 用 CapturedWriteTemplate 的
//   「选择推送分支」按冻结清单(拓扑序)套用行数据 → 更新 __sync_push_progress(记录该 push
//   已收到/应用到第几片，全部 done 才放行该 push 的 changelog 广播)→ 回送 PushChunkAck
//   (带 checksum，告知发送方此片是否完好应用，支撑断点续传与重传)。
// 为什么单独成路径：选择性推送是「按需推一个数据子集 + 其外键闭包」，需分片传输与逐片确认，
//   语义不同于全量 changeset(其按 origin_seq 连续；这里按 chunkSeq 连续)。
// 参数：dec 解码工件；checksum 本片校验和(回 ACK 用)。返回：是否成功应用本片。
// 线程：worker。错误模式：乱序/陌生 push → 延迟或忽略；应用失败 → 回 ok=false 的 ACK。
bool SyncWorker::processSelectionPushArtifact(const DecodeResult& dec, const QString& /*name*/,
                                              const QString& checksum) {
    const PayloadHeader& hdr = dec.header;
    const SelectionPushBody& body = dec.selection;
    const QString pushId = !hdr.pushId.isEmpty() ? hdr.pushId : body.pushId;
    const int chunkSeq = hdr.chunkSeq != 0 ? hdr.chunkSeq : body.chunkSeq;
    const int totalChunks = hdr.totalChunks > 0 ? hdr.totalChunks : body.totalChunks;

    // M-05 fix: ensure __sync_push_progress row exists before any chunk processing.
    // Insert on first chunk arrival (ON CONFLICT DO NOTHING is idempotent for subsequent chunks).
    if (!pushId.isEmpty()) {
        QSqlQuery ins(*wconnPtr_);
        // H-04 fix: only reset status to 'streaming' when NOT already done/failed.
        // Duplicate chunk delivery must not revert a completed push back to streaming
        // (which would stall the broadcast barrier forever).
        ins.prepare(
            QStringLiteral("INSERT INTO __sync_push_progress "
                           "(push_id, origin, peer, total_chunks, schema_ver, status, updated_ms) "
                           "VALUES (?, ?, ?, ?, ?, 'streaming', ?) "
                           "ON CONFLICT(push_id) DO UPDATE SET "
                           "  total_chunks=excluded.total_chunks, "
                           "  schema_ver=excluded.schema_ver, updated_ms=excluded.updated_ms, "
                           "  status=CASE WHEN status IN ('done','failed') THEN status "
                           "             ELSE 'streaming' END"));
        ins.addBindValue(pushId);
        ins.addBindValue(hdr.origin);
        ins.addBindValue(config_.nodeId());
        ins.addBindValue(totalChunks > 0 ? totalChunks : 1);
        ins.addBindValue(hdr.schemaVer);
        ins.addBindValue(QDateTime::currentMSecsSinceEpoch());
        ins.exec();  // non-fatal if table doesn't exist yet (pre-DDL edge case)
    }

    // M-02 fix: verify that subsequent chunks for an existing push_id come from the
    // same origin to prevent cross-origin/epoch push_id collision (though UUIDs make this
    // vanishingly rare, the protocol layer should enforce it explicitly).
    if (!pushId.isEmpty()) {
        QSqlQuery ck(*wconnPtr_);
        ck.prepare(QStringLiteral("SELECT origin FROM __sync_push_progress WHERE push_id=?"));
        ck.addBindValue(pushId);
        if (ck.exec() && ck.next()) {
            const QString storedOrigin = ck.value(0).toString();
            if (!storedOrigin.isEmpty() && storedOrigin != hdr.origin) {
                emit errorOccurred({err::E_SYNC_TRANSPORT, Severity::Error,
                                    QStringLiteral("selection_push"), hdr.origin,
                                    QStringLiteral("push_id %1 already registered for origin %2"
                                                   ", rejecting chunk from %3")
                                        .arg(pushId, storedOrigin, hdr.origin)});
                return false;
            }
        }
    }

    // J-04: Reject the entire push if the sender's schema version has moved.
    if (hdr.schemaVer != config_.schemaVersion()) {
        QSqlQuery q(*wconnPtr_);
        q.prepare(
            QStringLiteral("UPDATE __sync_push_progress SET status='failed', "
                           "failed_code='E_SYNC_PUSH_SCHEMA_MOVED', updated_ms=? "
                           "WHERE push_id=?"));
        q.addBindValue(QDateTime::currentMSecsSinceEpoch());
        q.addBindValue(pushId);
        q.exec();
        emit errorOccurred({err::E_SYNC_PUSH_SCHEMA_MOVED, Severity::Error,
                            QStringLiteral("selection_push"), hdr.origin,
                            QString(QStringLiteral("push_id=%1 schema_ver=%2 local=%3"))
                                .arg(pushId)
                                .arg(hdr.schemaVer)
                                .arg(config_.schemaVersion())});
        return false;
    }

    // H-15 fix: enforce in-order chunk application. ChunkStreamer emits chunks in topological
    // order (parent chunks before child chunks); applying chunk N before 0..N-1 would risk
    // FK-dangling rows and break the "half not externalised" guarantee. Apply chunk N only when
    // all chunks 0..N-1 are already applied; otherwise keep the artifact pending (ledger stays
    // 'seen') and retry on the next inbox scan once the predecessor chunks arrive.
    if (chunkSeq > 0 && !pushId.isEmpty()) {
        QSqlQuery cq(*wconnPtr_);
        cq.prepare(
            QStringLiteral("SELECT COUNT(*) FROM __sync_push_chunk_progress "
                           "WHERE push_id=? AND status='applied' AND chunk_seq < ?"));
        cq.addBindValue(pushId);
        cq.addBindValue(chunkSeq);
        if (cq.exec() && cq.next()) {
            const int appliedBefore = cq.value(0).toInt();
            if (appliedBefore < chunkSeq)
                return false;  // predecessor chunk(s) not yet applied — keep pending, retry later
        }
    }

    // 从选择性推送体构造出待写入的 RowMutation 列表。
    // J-05：每行要带上主键列(pkColumns)，UpsertExecutor 才能拼出正确的 ON CONFLICT(...) 子句
    //   （即“按主键 upsert”）。主键列从 PRAGMA table_info 读，并按表缓存，避免每行重复查 PRAGMA。
    QHash<QString, QStringList> pkColsCache;
    auto getPkCols = [&](const QString& table) -> QStringList {
        if (pkColsCache.contains(table))
            return pkColsCache[table];
        QSqlQuery pq(*wconnPtr_);
        // H-3 fix: use quoteIdent for table name in PRAGMA.
        pq.prepare(QStringLiteral("PRAGMA table_info(") + detail::SqlBuilder::quoteIdent(table) +
                   QLatin1Char(')'));
        QStringList pks;
        if (pq.exec()) {
            while (pq.next()) {
                if (pq.value("pk").toInt() > 0)
                    pks << pq.value("name").toString();
            }
        }
        pkColsCache[table] = pks;
        return pks;
    };

    QList<RowMutation> mutations;
    for (int i = 0; i < body.rows.size(); ++i) {
        // 第 i 行的数据来自 body.rows[i]，其“属于哪张表 + 是选中行还是依赖行”来自并行的
        // frozenEntries[i]（冻结清单，发送方按拓扑序冻结，父行在前）。
        const QVariantMap& rowMap = body.rows[i];
        RowMutation m;
        m.table = (i < body.frozenEntries.size()) ? body.frozenEntries[i].table : QString();
        m.columns = rowMap.keys();
        m.values = rowMap.values();
        m.pkColumns = getPkCols(m.table);
        // 按记录种类决定 upsert 策略：
        //   "selected"（用户显式选中，权威数据）→ DoUpdate：命中主键则覆盖更新；
        //   "dependency"（仅为满足外键闭包而带上的父行）→ DoNothing：已存在就别动它，
        //     避免依赖行反向覆盖接收端可能更新的数据（只补齐“缺失的父行”即可）。
        if (i < body.frozenEntries.size() &&
            body.frozenEntries[i].recordKind == QLatin1String("dependency"))
            m.mode = UpsertMode::DoNothing;
        else
            m.mode = UpsertMode::DoUpdate;
        mutations.append(m);
    }

    WriteParams p;
    p.kind = WriteKind::InboundSelectionPush;
    // Record the applying node as the origin so that the seq counter stays contiguous under
    // the local nodeId.  Using the push initiator's origin (hdr.origin) here would advance
    // the shared seq counter with a foreign-origin entry, causing the next local-origin entry
    // to skip a seq number.  Peers that receive this forwarded changeset would then see a false
    // GAP because the "missing" seq=N was never broadcast (anti-echo blocks echo back to
    // initiator). The applying node has the authority to claim this entry as its own broadcast —
    // that is equivalent to saying "center has applied and vouches for this data".
    p.origin = config_.nodeId();
    p.epoch = streamEpoch_;
    // save prevSeq so we can roll back if execute() fails (transaction rollback
    // leaves localOriginSeq_ advanced, which would create a gap seen as false GAP by peers).
    const qint64 prevSeqPush = localOriginSeq_;
    p.seq = nextLocalOriginSeq();
    p.schemaVer = hdr.schemaVer;
    p.schemaFp = hdr.schemaFingerprint;
    p.originRank = config_.originRank(config_.nodeId());
    p.pushId = pushId;
    p.chunkSeq = chunkSeq;
    p.checksum = checksum;
    p.mutations = mutations;
    p.syncTables = canonicalSyncTables_;

    WriteResult res = tpl_->execute(p);
    if (!res.ok) {
        // H-01 fix: transaction was rolled back inside execute(); restore seq counter.
        rollbackOriginSeq(prevSeqPush);
        emit errorOccurred(
            {res.errorCode, Severity::Error, "selection_push", hdr.origin, res.errorMsg});
        return false;
    }
    // H-01 fix: sealInto() returns success with localChangelogSeq==0 when changeset is empty
    // (no rows were actually modified). In that case the originSeq we allocated is unused, so
    // roll it back to keep origin_seq contiguous.
    if (res.localChangelogSeq == 0) {
        rollbackOriginSeq(prevSeqPush);
    }
    if (!res.tableStateStaleSince.isEmpty()) {
        emit errorOccurred(
            {err::W_SYNC_UNTRACKED_CHANGE, Severity::Warning, QStringLiteral("table_state"),
             hdr.origin, QStringLiteral("table_state update failed: ") + res.tableStateStaleSince});
    }
    PushChunkAck ack;
    ack.pushId = pushId;
    ack.chunkSeq = chunkSeq;
    ack.totalChunks = totalChunks > 0 ? totalChunks : 1;
    ack.checksum = checksum;
    ack.ok = true;
    ack.toPeer = hdr.origin;  // H-04 fix: ACK routes back to the push origin
    QString ackErr;
    if (!ackChan_->schedulePushChunkAck(ack, *codec_, &ackErr)) {
        emit errorOccurred(
            {err::E_SYNC_TRANSPORT, Severity::Warning, QStringLiteral("ack"), hdr.origin, ackErr});
    }
    return true;
}

// processBaselineRequestArtifact —— 响应对端的「基线请求」：导出全量、回 BaselineResponse。
// C-01 fix: implement baseline request handler.
// When a peer asks for a baseline (because it has a gap it cannot self-heal), this node
// serializes the requested tables and writes a BaselineResponse artifact to the outbox.
// 【C-01 修复】实现基线请求处理：当某对端因「无法自愈的 gap」请求基线时，本节点把所请求
//   的各表序列化成全量快照，写出一个 BaselineResponse 工件到 outbox 发回给它。
// 做什么：解出请求方与所需表 → 经 BaselineManager 导出这些表的当前全量数据 → 编码为
//   BaselineResponse 工件 → 写 outbox 路由回请求方。这是「另一端发起回退、本端配合提供
//   全量基线」的服务端半边。
// 参数：dec 请求工件(artifactName 形参未用)。返回：是否成功导出并写出回应。
// 线程：worker。前置：wconnPtr_ 有效。副作用：写一个 BaselineResponse 工件。
bool SyncWorker::processBaselineRequestArtifact(const DecodeResult& dec,
                                                const QString& /*artifactName*/) {
    if (!wconnPtr_)
        return false;

    const BaselineRequestPayload& req = dec.baselineRequest;

    // Decide which tables to export: prefer the request's list, fall back to all sync tables.
    QStringList tables = req.requestedTables.isEmpty() ? canonicalSyncTables_ : req.requestedTables;

    BaselineManager bm;
    BaselineManager::BaselineArtifact art;
    QString exportErr;
    if (!bm.exportBaseline(*wconnPtr_, tables, &art, &exportErr, config_.nodeId(), streamEpoch_,
                           localOriginSeq_)) {
        emit errorOccurred({err::E_SYNC_BASELINE_FAILED, Severity::Error,
                            QStringLiteral("baseline_request"), dec.header.origin, exportErr});
        return false;
    }

    // C-01 fix: merge the source node's own local origin cut into the baseline.
    // queryOriginCuts() only reads __sync_applied_vector; the source node's own writes
    // never advance its own applied_vector, so the cut for (nodeId, streamEpoch_) is absent.
    // Without this cut, the receiver resets the source node's applied_seq to 0 and replays
    // old changesets. We merge it here (take max if the entry already exists from a
    // self-echo that did advance applied_vector).
    {
        const QString& selfOrigin = config_.nodeId();
        bool found = false;
        for (BaselineOriginCut& cut : art.originCuts) {
            if (cut.origin == selfOrigin && cut.streamEpoch == streamEpoch_) {
                if (localOriginSeq_ > cut.appliedSeq)
                    cut.appliedSeq = localOriginSeq_;
                found = true;
                break;
            }
        }
        if (!found) {
            BaselineOriginCut selfCut;
            selfCut.origin = selfOrigin;
            selfCut.streamEpoch = streamEpoch_;
            selfCut.appliedSeq = localOriginSeq_;
            art.originCuts.append(selfCut);
        }
    }

    BaselineResponsePayload resp;
    resp.origin = config_.nodeId();
    resp.requestOrigin = dec.header.origin;  // send back to requester
    resp.streamEpoch = streamEpoch_;
    resp.tables = tables;
    resp.fromSeq = req.fromSeq;
    resp.pendingArtifactName = req.pendingArtifactName;
    resp.baselineData = art.data;
    resp.sourceMaxSeq = art.sourceMaxSeq;
    resp.originCuts =
        art.originCuts;  // C-03 fix: per-origin (epoch, appliedSeq) from applied_vector

    PayloadHeader hdr;
    hdr.origin = config_.nodeId();
    // H-01 fix: baseline response does not participate in the changeset sequence space.
    // Setting originSeq = 0 prevents a gap in local changeset seq that would cause
    // subsequent inbound changesets to be mis-judged as gaps and trigger another fallback.
    hdr.originSeq = 0LL;
    hdr.streamEpoch = streamEpoch_;
    hdr.schemaVer = config_.schemaVersion();
    hdr.schemaFingerprint = guard_ ? guard_->fingerprint() : QString();
    hdr.routeTag = dec.header.origin;

    QByteArray payload = codec_->encodeBaselineResponse(hdr, resp);

    const QString artifactOut = QStringLiteral("blresp__") + config_.nodeId() +
                                QStringLiteral("__") + dec.header.origin + QStringLiteral("__") +
                                QString::number(QDateTime::currentMSecsSinceEpoch()) +
                                QStringLiteral(".payload");
    QString writeErr;
    if (!outbox_->write(artifactOut, payload, &writeErr)) {
        emit errorOccurred({err::E_SYNC_TRANSPORT, Severity::Error,
                            QStringLiteral("baseline_request"), dec.header.origin, writeErr});
        return false;
    }
    return true;
}

// processBaselineResponseArtifact —— 收到「基线回应」：全量套用并重置跟踪状态、重扫 inbox。
// C-01 fix: implement baseline response handler.
// Applies the received baseline data, resets tracking stores, and triggers an inbox rescan
// so any artifacts that were pending the baseline can now be processed.
// 【C-01 修复】实现基线回应处理：套用收到的基线全量数据，重置各跟踪 store，并触发一次
//   inbox 重扫，使那些「之前因等待基线而挂起」的工件现在得以被处理。
// 做什么：经 BaselineManager 全量导入对端发来的快照(覆盖本地相关表)→ 重置 AppliedVector/
//   RowWinner 等跟踪 store 到与基线一致的水位 → drainQuarantine 重放因此变得可应用的隔离包
//   → requestRescan 重扫 inbox(此前卡在 gap 的增量工件现可继续)。
// 为什么基线后要重置跟踪 store：基线是「跳过中间增量、直接对齐到某一致快照」，原有的逐 seq
//   水位已不再连续可信，必须随基线一并重置，否则后续会把基线之后的正常增量误判为 gap。
// 参数：dec 基线回应工件(artifactName 形参未用)。返回：是否成功套用。线程：worker。
// 前置：wconnPtr_/hPtr_ 有效。副作用：覆盖本地数据、重置水位、重扫 inbox。
bool SyncWorker::processBaselineResponseArtifact(const DecodeResult& dec,
                                                 const QString& /*artifactName*/) {
    if (!wconnPtr_ || !hPtr_)
        return false;

    const BaselineResponsePayload& resp = dec.baselineResponse;

    // Verify this response was sent to us.
    if (!resp.requestOrigin.isEmpty() && resp.requestOrigin != config_.nodeId())
        return true;  // not for this node — silently consume

    BaselineManager bm;
    BaselineManager::BaselineArtifact art;
    art.data = resp.baselineData;
    art.sourceMaxSeq = resp.sourceMaxSeq;
    art.originCuts =
        resp.originCuts;  // C-03 fix: per-origin (epoch, appliedSeq) from applied_vector

    qint64 newAnchorSeq = 0;
    QString applyErr;
    const QString schemaFp = guard_ ? guard_->fingerprint() : QString();
    // M-02 fix: pass the baseline origin's rank so applyBaseline can seed RowWinner entries.
    const int baselineRank = config_.originRank(resp.origin);
    // M-01 fix: use the shared consistencyCache_ (initialized in run()) so baseline apply
    // feeds the persistent cache that selection push reads for dependency pruning.
    if (!bm.applyBaseline(*wconnPtr_, hPtr_, art, *av_, *ts_, *rw_, consistencyCache_,
                          resp.streamEpoch, resp.origin, schemaFp, &newAnchorSeq, &applyErr,
                          baselineRank)) {
        emit errorOccurred({err::E_SYNC_BASELINE_FAILED, Severity::Error,
                            QStringLiteral("baseline_response"), dec.header.origin, applyErr});
        return false;
    }

    // M-01 fix: after baseline apply, stamp each sync-table row in consistencyCache_ so that
    // pruneConsistentDependencies() can skip rows that are already consistent with the center.
    // Fingerprint format mirrors FkClosureBuilder::rowFingerprint(): SHA1 of "col\0val\0..." pairs.
    if (!canonicalSyncTables_.isEmpty()) {
        for (const QString& tbl : canonicalSyncTables_) {
            // 先探测该表的主键列名。PRAGMA table_info 每行含：cid(0)/name(1)/type(2)/
            //   notnull(3)/dflt(4)/pk(5)；pk==1 即主键列。表名中的双引号按 SQL
            //   规则转义(""）以防注入/报错。
            const QString pkColStmt =
                QStringLiteral("PRAGMA table_info(\"") +
                QString(tbl).replace(QLatin1Char('"'), QLatin1String("\"\"")) +
                QStringLiteral("\")");
            QSqlQuery pkQ(*wconnPtr_);
            if (!pkQ.exec(pkColStmt))
                continue;
            QString pkCol;
            while (pkQ.next()) {
                if (pkQ.value(5).toInt() == 1) {
                    pkCol = pkQ.value(1).toString();
                    break;
                }
            }
            if (pkCol.isEmpty())
                continue;  // 无单列主键的表跳过（一致性缓存以“表+主键”为键，无主键无从盖章）

            QSqlQuery rowQ(*wconnPtr_);
            if (!rowQ.exec(QStringLiteral("SELECT * FROM \"") +
                           QString(tbl).replace(QLatin1Char('"'), QLatin1String("\"\"")) +
                           QStringLiteral("\"")))
                continue;

            // 逐行算“行指纹”并盖章进一致性缓存。指纹算法必须与 FkClosureBuilder::rowFingerprint()
            //   逐字节一致，否则推送端 isConsistent() 比对不上、剪枝失效：
            //   · 用 QVariantMap（按 key 排序）遍历列，保证列顺序与对端一致（M-01）；
            //   · 拼接格式为 "列名\0列值\0列名\0列值\0..." 后取 SHA1。
            const QSqlRecord rec = rowQ.record();
            while (rowQ.next()) {
                const QString pk = rowQ.value(pkCol).toString();
                QVariantMap rowMap;
                for (int ci = 0; ci < rec.count(); ++ci)
                    rowMap.insert(rec.fieldName(ci), rowQ.value(ci));
                QByteArray buf;
                for (auto it = rowMap.constBegin(); it != rowMap.constEnd(); ++it) {
                    buf += it.key().toUtf8();
                    buf += '\0';
                    buf += it.value().toString().toUtf8();
                    buf += '\0';
                }
                const QByteArray fp = QCryptographicHash::hash(buf, QCryptographicHash::Sha1);
                // 盖章：记录“(表,主键) 这行现已与权威端一致，指纹=fp”，供后续选择性推送剪枝依赖行。
                consistencyCache_.stampFromAuthoritative(*wconnPtr_, tbl, pk, fp);
            }
        }
    }

    // Remove from in-flight tracking so the next gap scan doesn't re-request.
    baselineRequestsInFlight_.remove(resp.pendingArtifactName);

    // M-06 fix: drain quarantine after baseline apply so payloads that were quarantined
    // due to schema version mismatch can now be replayed with the updated applied_vector.
    drainQuarantine();

    // Trigger rescan so any artifacts that were deferred waiting for this baseline are now applied.
    enqueue([this]() { scanInbox(); });

    emit errorOccurred(
        {err::W_SYNC_UNTRACKED_CHANGE, Severity::Warning, QStringLiteral("baseline"),
         dec.header.origin,
         QStringLiteral("Baseline from '%1' applied successfully; anchor seq=%2; rescanning inbox")
             .arg(dec.header.origin)
             .arg(newAnchorSeq)});
    return true;
}

// processAckArtifact —— 处理收到的 ACK 工件（推进发送方向的确认水位、消费 ACK 窗口）。
// 做什么：解出 ACK 类型——ChangesetAck 则推进 OutboundAckStore 中 (peer,origin,epoch) 的
//   acked_seq(MAX 推进，只增不减)，并检查是否令某个 pendingAckWindow_ 条目达成(全部达成则
//   前台 sync() 可切 Completed)；PushChunkAck 则推进对应 push 的 chunk 确认进度(忽略来自陈旧
//   /无关 push 的 chunk ACK——靠 pendingPushId_ 过滤，C-04)。
// 为什么 ACK 也要入 ledger(M-07)：ACK 工件同样可能被重复发现，入台账幂等去重，避免无限重处理。
// 参数：path ACK 文件路径；name 文件名(查/写 ledger 用)。返回：是否成功处理。
// 线程：worker。副作用：推进 acked_seq / push 进度，可能推动前台状态机走向完成。
bool SyncWorker::processAckArtifact(const QString& path, const QString& name) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    QByteArray data = f.readAll();
    f.close();

    // I-15 fix: parse the sending peer from the artifact file name.
    // ACK file name format: ack__{fromPeer}__{toPeer}__{ms}.ack
    // parts[0]="ack", parts[1]=fromPeer, parts[2]=toPeer, parts[3]=ms+".ack"
    QString peer;
    {
        QStringList parts = name.split(QStringLiteral("__"));
        if (parts.size() >= 3)
            peer = parts[1];  // fromPeer
    }

    // Try changeset ACK first
    ChangesetAck csAck;
    if (codec_->decodeChangesetAck(data, &csAck)) {
        // M-09 fix: the ACK payload is self-describing; ignore ACKs not addressed to this node
        // (e.g. mis-routed or renamed artifacts).
        if (!csAck.toPeer.isEmpty() && csAck.toPeer != config_.nodeId()) {
            emit errorOccurred({err::W_SYNC_UNTRACKED_CHANGE, Severity::Warning,
                                QStringLiteral("ack"), csAck.origin,
                                QStringLiteral("changeset ACK addressed to %1, not this node %2")
                                    .arg(csAck.toPeer, config_.nodeId())});
            return true;  // consumed (not ours), don't reprocess
        }
        // I-15 fix: pass peer (sender) as the first peer argument, not csAck.origin.
        if (!peer.isEmpty())
            ackStore_->updateAcked(*wconnPtr_, peer, csAck.origin, csAck.streamEpoch,
                                   csAck.appliedSeq, nullptr);
        // C-01 fix: only complete foreground sync() when ALL expected peers have ACKed their
        // respective (origin, epoch, targetSeq). Check each entry in pendingAckWindow_ against
        // the current acked_seq; complete only when every entry is satisfied.
        if (ackWaiting_.load() && !pendingAckWindow_.isEmpty()) {
            bool allAcked = true;
            for (const PendingAckEntry& entry : qAsConst(pendingAckWindow_)) {
                const qint64 acked =
                    ackStore_->ackedSeq(*wconnPtr_, entry.peer, entry.origin, entry.epoch);
                if (acked < entry.targetSeq) {
                    allAcked = false;
                    break;
                }
            }
            if (allAcked && ackWaiting_.exchange(false)) {
                pendingAckWindow_.clear();
                SyncProgress p;
                p.state = SyncState::Completed;
                p.percent = 100;
                emit progressUpdated(p);
            }
        } else if (ackWaiting_.load() && pendingAckWindow_.isEmpty()) {
            // No window was built (e.g. empty broadcast) — should not happen after C-01 fix,
            // but fall back to old behaviour to avoid permanent block.
            if (ackWaiting_.exchange(false)) {
                SyncProgress p;
                p.state = SyncState::Completed;
                p.percent = 100;
                emit progressUpdated(p);
            }
        }
        evaluatePeers();
        return true;
    }
    // C-05 fix: process PushChunkAck — record per-chunk status and check for push completion.
    PushChunkAck chunkAck;
    if (codec_->decodeChunkAck(data, &chunkAck)) {
        if (chunkAck.pushId.isEmpty())
            return false;
        // M-09 fix: ignore chunk ACKs not addressed to this node.
        if (!chunkAck.toPeer.isEmpty() && chunkAck.toPeer != config_.nodeId()) {
            emit errorOccurred({err::W_SYNC_UNTRACKED_CHANGE, Severity::Warning,
                                QStringLiteral("ack"), chunkAck.pushId,
                                QStringLiteral("chunk ACK addressed to %1, not this node %2")
                                    .arg(chunkAck.toPeer, config_.nodeId())});
            return true;
        }
        // C-04 fix: ignore chunk ACKs that belong to a different (stale) push operation.
        // This prevents an ACK from a previous enqueueSelectionPush() from completing the
        // ackWaiting_ that was armed for the current push.
        if (!pendingPushId_.isEmpty() && chunkAck.pushId != pendingPushId_) {
            return true;  // silently consume — the artifact is valid but for a different operation
        }

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

        // Record this chunk's ACK in push_chunk_progress.
        QSqlQuery markQ(*wconnPtr_);
        markQ.prepare(
            QStringLiteral("INSERT INTO __sync_push_chunk_progress "
                           "(push_id, chunk_seq, status, checksum, applied_ms) "
                           "VALUES (?, ?, 'applied', ?, ?) "
                           "ON CONFLICT(push_id, chunk_seq) DO UPDATE SET "
                           "  status='applied', checksum=excluded.checksum, "
                           "  applied_ms=excluded.applied_ms"));
        markQ.addBindValue(chunkAck.pushId);
        markQ.addBindValue(chunkAck.chunkSeq);
        markQ.addBindValue(chunkAck.checksum);
        markQ.addBindValue(nowMs);
        markQ.exec();

        // Check whether all chunks have been ACKed.
        if (chunkAck.totalChunks > 0) {
            QSqlQuery countQ(*wconnPtr_);
            countQ.prepare(
                QStringLiteral("SELECT COUNT(*) FROM __sync_push_chunk_progress "
                               "WHERE push_id=? AND status='applied'"));
            countQ.addBindValue(chunkAck.pushId);
            if (countQ.exec() && countQ.next()) {
                int ackedCount = countQ.value(0).toInt();
                if (ackedCount >= chunkAck.totalChunks) {
                    // All chunks ACKed — mark push_progress done and complete foreground op.
                    QSqlQuery doneQ(*wconnPtr_);
                    doneQ.prepare(
                        QStringLiteral("UPDATE __sync_push_progress "
                                       "SET status='done', updated_ms=? WHERE push_id=?"));
                    doneQ.addBindValue(nowMs);
                    doneQ.addBindValue(chunkAck.pushId);
                    doneQ.exec();

                    // C-04 fix: clear pendingPushId_ so future unrelated chunk ACKs don't
                    // match this operation (belt-and-suspenders alongside the pushId check above).
                    pendingPushId_.clear();
                    // Transition foreground state to Completed (design §5.4 / FR-11).
                    if (ackWaiting_.exchange(false)) {
                        SyncProgress p;
                        p.state = SyncState::Completed;
                        p.percent = 100;
                        emit progressUpdated(p);
                    }
                }
            }
        }
        evaluatePeers();
        return true;
    }
    return false;
}

// broadcast —— 向「所有未被驱逐的对端」各发一轮 changelog，再统一 flush ACK 通道。
// 做什么：遍历配置的 peerNodes，跳过已驱逐者，对其余每个调 broadcastTopeer；之后
//   把积攒的 ACK 一次性写出(ackChan_->flush)，最后 evaluatePeers 评估健康度。
// 为什么先逐 peer 发、再统一 flush ACK：ACK 通道是批量的——把多次应用产生的 ACK 攒起来
//   一次写出，减少小文件 IO。这是「后台广播循环」用的版本，不需要 ACK 窗口记账
//   （前台 sync() 走 enqueueDrain → 直接调 broadcastTopeer 并传 ackedEntries 收集窗口）。
// 参数：outErr 可选，回填首个遇到的错误（只记第一个，不覆盖）。
// 返回：true 表示本轮至少向某个对端写出了工件（有实际广播发生）。
// 线程：worker。副作用：写 outbox 工件、flush ACK、可能驱逐对端。
bool SyncWorker::broadcast(QString* outErr) {
    bool wroteAny = false;
    for (const QString& peer : config_.peerNodes()) {
        if (isPeerEvicted(peer))
            continue;  // 已驱逐（失联/滞后）的对端本轮跳过
        QString peerErr;
        if (broadcastTopeer(peer, &peerErr))
            wroteAny = true;
        if (!peerErr.isEmpty() && outErr && outErr->isEmpty())
            *outErr = peerErr;
    }
    // NOTE: enqueueDrain calls broadcastTopeer directly with ackedEntries; the broadcast()
    // helper used by the background loop does not need ACK window tracking.
    QString ackErr;
    if (!ackChan_->flush(*codec_, &ackErr)) {
        emit errorOccurred({err::E_SYNC_TRANSPORT, Severity::Warning, QStringLiteral("ack"),
                            config_.nodeId(), ackErr});
        if (outErr && outErr->isEmpty())
            *outErr = ackErr;
    }
    evaluatePeers();
    return wroteAny;
}

// broadcastTopeer —— 向「单个对端」发一轮 changelog（广播的核心实现）。
// 做什么（流程概览，细节见下方各 fix 注释）：
//   ① 用 minUnackedLocalSeq 求读取下界(afterLocalSeq)——确保未被 ACK 的变更可被重发；
//   ② 从 changelog 读这段范围的条目；
//   ③ 逐条按 anti-echo 路由(shouldRoute)过滤：不发对端自己来源的、已被它确认的变更；
//   ④ 跳过尚属「未完成的选择性推送」的条目（避免半成品被广播）；
//   ⑤ 对要发的 changeset 先 rebase 到本地冲突处理结果之上，再编码成工件写 outbox；
//   ⑥ 记账：把成功写出的 (peer,origin,epoch,maxOriginSeq) 收进 ackedEntries(若提供)，
//      供 enqueueDrain 构建完整 ACK 窗口；并推进发送水位 updateLastSent。
// 为什么读取下界用「未确认」而非「已发送」(C-02)：已发送但未确认的变更必须仍可被重读重发，
//   否则一旦丢包就永久漏发；last_sent 仅用于诊断/推进，不作为重发剪枝依据。
// 参数：peer 目标对端；outErr 可选错误回填；ackedEntries 可选，收集本轮 ACK 窗口条目。
// 返回：true=本轮确实向该对端写出了至少一个工件。
// 线程：worker。副作用：写 outbox 工件、推进 last_sent 水位、可能填充 ackedEntries。
bool SyncWorker::broadcastTopeer(const QString& peer, QString* outErr,
                                 QList<PendingAckEntry>* ackedEntries) {
    // C-02 fix: use minUnackedLocalSeq as the read lower-bound so that un-ACKed changesets
    // are always eligible for re-send.  lastSentLocalSeq is only advanced for diagnostics.
    // Falls back to -1 (read from beginning) when no ACK rows exist yet.
    qint64 afterLocalSeq = ackStore_->minUnackedLocalSeq(*wconnPtr_, peer, streamEpoch_);
    if (afterLocalSeq < -1)
        afterLocalSeq = -1;

    // Read pending entries from changelog
    QList<ChangelogStore::EntryFull> entries = clog_->readRangeAll(
        *wconnPtr_, peer, afterLocalSeq, static_cast<int>(config_.broadcastThreshold()));

    qint64 bytesSent = 0;
    qint64 lastSentLocal = afterLocalSeq;
    bool wroteAny = false;
    for (const auto& entry : entries) {
        // H-02 fix: shouldRoute must compare entry.originSeq against the per-(peer,origin)
        // acked_seq, NOT against afterLocalSeq which is a local_seq watermark in a different
        // sequence namespace.  Mixing them causes phantom misses and double-sends.
        // C-02 fix: use entry.streamEpoch (the epoch under which this origin's ACK was stored)
        // rather than streamEpoch_ (local node epoch). When forwarding a remote-origin changeset,
        // the receiver ACKs under entry.streamEpoch, so querying with streamEpoch_ returns -1
        // and causes infinite re-sends.
        const qint64 entryEpoch = entry.streamEpoch > 0 ? entry.streamEpoch : streamEpoch_;
        const qint64 peerOriginAcked =
            ackStore_->ackedSeq(*wconnPtr_, peer, entry.origin, entryEpoch);
        if (!routing_->shouldRoute(peer, entry.origin, entry.originSeq, peerOriginAcked))
            continue;

        // H-01 fix: skip changelog entries produced by a selection push that has not yet been fully
        // applied (status != 'done').  Only skip when the entry's own push_id is still pending —
        // not all pushes from the same origin (coarse filter bug).
        // Entries without a push_id (plain changesets) are never blocked.
        // Note: the previous guard "entry.origin != config_.nodeId()" is intentionally removed:
        // center nodes can be the originator of a selection push, and their own captured changesets
        // must also be held back until the entire push is marked done.
        if (!entry.pushId.isEmpty()) {
            QSqlQuery pushQ(*wconnPtr_);
            pushQ.prepare(
                QStringLiteral("SELECT COUNT(*) FROM __sync_push_progress "
                               "WHERE push_id = ? AND status != 'done' AND status != 'failed'"));
            pushQ.addBindValue(entry.pushId);
            if (pushQ.exec() && pushQ.next() && pushQ.value(0).toInt() > 0)
                continue;
        }

        // I-16: Rebase the changeset onto any stored rebase buffer before broadcast.
        QByteArray changesetToSend = entry.changeset;
        QString rebaseKey = entry.origin + QLatin1Char('/') + QString::number(entry.originSeq);
        if (rebaseBuffers_.contains(rebaseKey) && rebaser_) {
            QByteArray rebased;
            QString rebaseErr;
            if (rebaser_->rebase(rebaseBuffers_.value(rebaseKey), entry.changeset, &rebased,
                                 &rebaseErr)) {
                changesetToSend = rebased;
            } else {
                // M-03 fix: rebase failure is Error (not Warning) so SyncEngine's
                // onWorkerError transitions foreground state to Failed when applicable.
                emit errorOccurred({err::E_SYNC_REBASE_FAILED, Severity::Error,
                                    QStringLiteral("broadcast"), peer, rebaseErr});
                continue;  // skip this entry — don't send un-rebased
            }
        }

        // Encode and write to outbox
        PayloadHeader hdr;
        hdr.origin = entry.origin;
        hdr.originSeq = entry.originSeq;
        // C-04 fix: use the entry's own stream_epoch (not the local node's epoch) so that
        // forwarded remote-origin changesets preserve their original epoch namespace.
        // Self-authored entries have streamEpoch == streamEpoch_ by construction.
        hdr.streamEpoch = entry.streamEpoch > 0 ? entry.streamEpoch : streamEpoch_;
        hdr.schemaVer = config_.schemaVersion();
        hdr.schemaFingerprint = guard_ ? guard_->fingerprint() : QString();
        hdr.routeTag = peer;
        // C-05 fix: stamp the physical sender so the receiver can ACK back to us rather than
        // to the business-origin node (which may be a different node when we forward changesets).
        hdr.senderPeer = config_.nodeId();

        QByteArray payload = codec_->encodeChangeset(hdr, changesetToSend);
        // H-07 / H-08 fix: include target peer so different peers get distinct file names.
        QString artifactName =
            ddl::changesetArtifactName(entry.origin, hdr.streamEpoch, entry.originSeq, peer);
        QString writeErr;
        if (!outbox_->write(artifactName, payload, &writeErr)) {
            emit errorOccurred(
                {err::E_SYNC_TRANSPORT, Severity::Warning, "broadcast", peer, writeErr});
            if (outErr)
                *outErr = writeErr;
            break;
        }
        wroteAny = true;
        bytesSent += payload.size();
        lastSentLocal = qMax(lastSentLocal, entry.localSeq);
        // C-01 fix: record the (peer, origin, epoch, originSeq) tuple for ACK window construction.
        if (ackedEntries) {
            PendingAckEntry ae;
            ae.peer = peer;
            ae.origin = entry.origin;
            ae.epoch = hdr.streamEpoch;
            ae.targetSeq = entry.originSeq;
            ackedEntries->append(ae);
        }
        if (bytesSent >= config_.outboxMaxBytesPerPeer())
            break;
    }

    // Advance the send-watermark so the next broadcast starts where we left off.
    if (lastSentLocal > afterLocalSeq)
        ackStore_->updateLastSent(*wconnPtr_, peer, streamEpoch_, lastSentLocal, nullptr);
    return wroteAny;
}

// computePeerAckedSeq —— 查「某对端」对「本节点 origin」已确认到的 seq。
// 做什么：从 OutboundAckStore 读 (peer, 本节点id, 当前纪元) 的 acked_seq。
// 为什么按 peer 而非全局 MIN（I-15 fix）：判断「这个对端」是否已确认到某点时，必须用它
//   自己的水位；若误用全体最小值，会被最慢的对端拖累而误判其它对端尚未确认。
// 参数：peer 对端 id。返回：该对端已确认的本地 origin_seq（无记录则由底层返回 -1）。
// 线程：worker。复杂度：O(1)（主键命中）。
qint64 SyncWorker::computePeerAckedSeq(const QString& peer) {
    // I-15 fix: query the acked seq for this specific peer, not the global min.
    // 【I-15 修复】查的是「该特定对端」的已确认 seq，而非全体最小值。
    return ackStore_->ackedSeq(*wconnPtr_, peer, config_.nodeId(), streamEpoch_);
}

// isPeerEvicted —— 该对端是否已被「驱逐」（标记为基线挂起 pending_baseline=1）。
// 做什么：查 __sync_outbound_ack 中该 peer 任一行的 pending_baseline 是否为 1（取 MAX）。
// 为什么：被判定失联/严重滞后的对端会被置 pending_baseline，广播循环据此跳过它，避免
//   为一个不在线的对端反复打包发送、浪费 IO，也把它排除出 changelog 裁剪计算。
// 参数：peer 对端。返回：true=已驱逐（应跳过）；连接无效/查询失败/无记录 → false（不跳过）。
// 线程：worker。复杂度：O(该 peer 行数)。错误模式：查询失败按「未驱逐」保守处理。
bool SyncWorker::isPeerEvicted(const QString& peer) {
    if (!wconnPtr_)
        return false;
    QSqlQuery q(*wconnPtr_);
    q.prepare(
        QStringLiteral("SELECT COALESCE(MAX(pending_baseline), 0) "
                       "FROM __sync_outbound_ack WHERE peer = ?"));
    q.addBindValue(peer);
    if (!q.exec() || !q.next())
        return false;
    return q.value(0).toInt() != 0;
}

// peerLastAckMs —— 该对端最近一次 ACK 的时间戳（毫秒）。
// 做什么：取该 peer 名下所有「真实 origin 行」(排除 '__broadcast__' 哨兵行) 的
//   last_ack_ms 最大值。为什么排除哨兵行：哨兵行记的是「发送」时间而非「确认」时间，
//   把它算进来会高估对端的活跃度，导致失联判定失准。
// 参数：peer 对端。返回：最近 ACK 的 ms 时间戳；无任何确认记录 → 0（视为「从未确认」）。
// 用途：evaluatePeers 用「now - 此值」算对端静默时长，超阈值判为失联。
// 线程：worker。复杂度：O(该 peer 行数)。
qint64 SyncWorker::peerLastAckMs(const QString& peer) {
    QSqlQuery q(*wconnPtr_);
    q.prepare(
        QStringLiteral("SELECT COALESCE(MAX(last_ack_ms), 0) "
                       "FROM __sync_outbound_ack "
                       "WHERE peer = ? AND origin != '__broadcast__'"));  // 排除发送哨兵行
    q.addBindValue(peer);
    if (!q.exec() || !q.next())
        return 0;
    return q.value(0).toLongLong();
}

// peerLagBytes —— 该对端「落后」了多少字节（滞后评估的字节维度）。
// 做什么：累加 __sync_changelog 中「不是该 peer 自己产生的(origin != peer)、且 local_seq
//   大于 afterLocalSeq(它已发送/确认到的水位)」的所有变更的 byte_size。
// 为什么 origin != peer：anti-echo——对端自己产生的变更本就不会发回给它，不应计入它的
//   待收积压；只统计「它还欠收的、别处来的」变更字节量。
// 参数：peer 对端；afterLocalSeq 它的发送/确认水位（此值之后的才算积压）。
// 返回：积压字节数（0 表示已追平或查询失败）。用途：超过字节阈值即判定该对端滞后。
// 线程：worker。复杂度：O(满足条件的 changelog 行数)。
qint64 SyncWorker::peerLagBytes(const QString& peer, qint64 afterLocalSeq) {
    QSqlQuery q(*wconnPtr_);
    q.prepare(
        QStringLiteral("SELECT COALESCE(SUM(byte_size), 0) "
                       "FROM __sync_changelog "
                       "WHERE origin != ? AND local_seq > ?"));  // 排除对端自产 + 仅统计水位之后
    q.addBindValue(peer);
    q.addBindValue(afterLocalSeq);
    if (!q.exec() || !q.next())
        return 0;
    return q.value(0).toLongLong();
}

// evaluatePeers —— 逐对端评估健康度（滞后/失联），必要时驱逐或告警。
// 做什么：对每个对端，采集三维度指标——已发送水位与本地头之差(seq 滞后)、积压字节量
//   (peerLagBytes)、距最近一次 ACK 的时长(now - peerLastAckMs)——交给 DeadPeerEvictor
//   按软/硬阈值判定状态(健康/Lagging/Dead)，据此告警或把对端置 pending_baseline 驱逐。
// 为什么三维度：单看一项易误判（短暂大批量写会让字节积压瞬时飙高但对端其实在线）；
//   组合「序号滞后 + 字节积压 + 静默时长」才能稳健区分「暂时落后」与「真失联」。
// 何时调用：每轮 broadcast 末尾。前置：wconnPtr_ 与 evictor_ 均有效。
// 线程：worker。副作用：可能 emit 告警、可能驱逐对端(改 pending_baseline)。
void SyncWorker::evaluatePeers() {
    if (!wconnPtr_ || !evictor_)
        return;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 localHead = clog_->maxLocalSeq(*wconnPtr_);
    for (const QString& peer : config_.peerNodes()) {
        const bool alreadyEvicted = isPeerEvicted(peer);
        const qint64 lastSent = ackStore_->lastSentLocalSeq(*wconnPtr_, peer, streamEpoch_);

        DeadPeerEvictor::PeerState state;
        state.peer = peer;
        state.lastAckMs = peerLastAckMs(peer);
        state.lagSeq = qMax<qint64>(0, localHead - lastSent);
        state.lagBytes = peerLagBytes(peer, lastSent);
        state.evicted = alreadyEvicted;

        const auto level = evictor_->evaluate(state, nowMs);
        if (level == DeadPeerEvictor::AlertLevel::Dead && !alreadyEvicted) {
            QString evictErr;
            if (evictor_->evict(*wconnPtr_, peer, *ackStore_, &evictErr)) {
                emit errorOccurred({err::E_SYNC_PEER_DEAD, Severity::Error, QStringLiteral("peer"),
                                    peer,
                                    QStringLiteral("Peer evicted; pending baseline required")});
            } else {
                emit errorOccurred({err::E_SYNC_TRANSPORT, Severity::Warning,
                                    QStringLiteral("peer"), peer, evictErr});
            }
        } else if (level == DeadPeerEvictor::AlertLevel::Lagging) {
            emit errorOccurred({err::W_SYNC_PEER_LAGGING, Severity::Warning, QStringLiteral("peer"),
                                peer, QStringLiteral("Peer lag exceeds soft threshold")});
        }
    }
}

// runBaselineFallbackFor —— 对「超时仍补不齐的 gap 工件」发起基线请求（兜底全量对齐）。
// 做什么：读出这个迟迟无法应用(其前驱 seq 缺失)的工件、解码出它的 origin/epoch，向该 origin
//   发一个 BaselineRequest，请求对方导出全量基线，从而跳过那段补不上的增量、重新对齐。
// 为什么需要：增量同步靠连续 seq；一旦中间某段永久丢失(对方已裁剪/网络长期中断)，光等
//   增量永远补不上。基线回退是「认输并重来」的安全网：用一次全量快照重置到一致状态。
// 去抖：baselineRequestsInFlight_ 防止对同一工件反复发请求(同一 gap 只请求一次)。
// 参数：artifactName gap 工件文件名。返回：是否成功发起了基线请求。
// 线程：worker。前置：wconnPtr_/hPtr_ 有效。副作用：可能写出一个 BaselineRequest 工件。
bool SyncWorker::runBaselineFallbackFor(const QString& artifactName) {
    if (!wconnPtr_ || !hPtr_)
        return false;

    const QString path = config_.inboxDir() + QDir::separator() + artifactName;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray data = f.readAll();
    f.close();

    DecodeResult dec;
    QString decErr;
    if (!codec_->decode(data, &dec, &decErr) || dec.kind != PayloadKind::Changeset)
        return false;

    BaselineManager baseline;
    const qint64 applied = av_->current(*wconnPtr_, dec.header.origin, dec.header.streamEpoch);
    if (!baseline.shouldFallbackToBaseline(applied, dec.header.originSeq))
        return false;

    // C-1 fix: request a source-authoritative baseline over the transport channel.
    // If a request is already in-flight for this artifact, don't re-send (debounce).
    if (baselineRequestsInFlight_.contains(artifactName)) {
        // Already requested — artifact stays pending; wait for baseline response.
        return false;
    }

    // Generate a BaselineRequest artifact and write it to the outbox addressed to the origin node.
    // The response handler (processBaselineResponseArtifact) will apply the baseline and then
    // rescan the inbox so this pending artifact gets retried.
    BaselineRequestPayload req;
    req.origin = dec.header.origin;
    req.streamEpoch = dec.header.streamEpoch;
    req.fromSeq = applied;
    req.pendingArtifactName = artifactName;
    req.requestedTables = canonicalSyncTables_;

    PayloadHeader reqHdr;
    reqHdr.origin = config_.nodeId();
    // H-01 fix: baseline request does not participate in the changeset sequence space.
    // Setting originSeq = 0 prevents a gap in local changeset seq that would cause
    // subsequent inbound changesets to be mis-judged as gaps and trigger another fallback.
    reqHdr.originSeq = 0LL;
    reqHdr.streamEpoch = streamEpoch_;
    reqHdr.schemaVer = config_.schemaVersion();
    reqHdr.schemaFingerprint = guard_ ? guard_->fingerprint() : QString();
    reqHdr.routeTag = dec.header.origin;

    QByteArray reqPayload = codec_->encodeBaselineRequest(reqHdr, req);
    const QString reqName = QStringLiteral("blreq__") + config_.nodeId() + QStringLiteral("__") +
                            dec.header.origin + QStringLiteral("__") +
                            QString::number(QDateTime::currentMSecsSinceEpoch()) +
                            QStringLiteral(".payload");

    QString writeErr;
    if (!outbox_->write(reqName, reqPayload, &writeErr)) {
        // Cannot send request — quarantine the artifact as before to avoid infinite rescan.
        emit errorOccurred(
            {err::E_SYNC_GAP, Severity::Error, QStringLiteral("baseline"), dec.header.origin,
             QStringLiteral("Gap for origin=%1 seq=%2: cannot send BaselineRequest: %3")
                 .arg(dec.header.origin)
                 .arg(dec.header.originSeq)
                 .arg(writeErr)});
        if (!config_.quarantineDir().isEmpty()) {
            QDir qDir(config_.quarantineDir());
            qDir.mkpath(QStringLiteral("."));
            if (QFile::copy(path, qDir.filePath(artifactName)))
                QFile::remove(path);
        }
        ledger_->markConsumed(*wconnPtr_, artifactName, nullptr);
        return false;
    }

    // Request sent — mark artifact as in-flight and keep it in the pending ledger.
    baselineRequestsInFlight_.insert(artifactName);
    emit errorOccurred(
        {err::E_SYNC_GAP, Severity::Warning, QStringLiteral("baseline"), dec.header.origin,
         QStringLiteral("Gap for origin=%1 seq=%2: BaselineRequest sent, waiting for response")
             .arg(dec.header.origin)
             .arg(dec.header.originSeq)});
    return false;
}

// submitImportSync —— 在 worker 线程上跑一次 Excel 导入（session 捕获 + 同步等待）。
// I-04: Submit import to run on the worker thread using wconn + session capture.
// C-03 fix: accepts pre-snapshotted profile/catalog so the worker never touches
// DataBridge::db_ or its mutable members from the wrong thread.
// 【I-04】把导入提交到 worker 线程，用其写连接 wconn 配合 SQLite session 捕获变更。
// 【C-03 修复】参数 profile/catalog 是「调用方线程预先拍好的快照」——worker 全程不触碰
//   DataBridge::db_ 或其可变成员，杜绝跨线程使用同一连接/可变 catalog 的隐患。
// 做什么：把导入操作打包成闭包投递到 worker 队列并阻塞等结果(最多 60s)；闭包内用 wconn
//   执行导入，所有写改动被 session 捕获、封入 changelog，随后会被广播给各 peer。
// 为什么必须在 worker 线程：session/changeset 捕获绑定写连接，而该连接归 worker 线程独占；
//   且只有走 worker 才能让导入改动进入 changelog 被同步出去(区别于绕过捕获的直写)。
// 参数：opts 导入选项；xlsxPath 源文件；profile/catalog 跨线程安全的规格快照。
// 返回：ImportResult(含成功行数/错误)；超时则返回带 E_SYNC_INIT 错误的结果。
// 线程：调用方线程投递+阻塞，实际执行在 worker 线程。
ImportResult SyncWorker::submitImportSync(const ImportOptions& opts, const QString& xlsxPath,
                                          const detail::ProfileSpec& profile,
                                          const detail::SchemaCatalog& catalog) {
    if (!isRunning() || !wconnPtr_) {
        // Worker not started — return error; cannot fall back to main-thread db_ (C-03).
        ImportResult r;
        RowError e;
        e.code = QLatin1String(err::E_SYNC_INIT);
        e.message = QStringLiteral(
            "SyncWorker not ready; import rejected to avoid cross-thread db_ access");
        r.errors.append(e);
        return r;
    }

    auto sharedPromise = std::make_shared<std::promise<ImportResult>>();
    std::future<ImportResult> future = sharedPromise->get_future();

    // profile/catalog are value-copied into the lambda — worker never touches main-thread objects.
    enqueue([this, opts, xlsxPath, profile, catalog, sp = sharedPromise]() mutable {
        if (!wconnPtr_ || !hPtr_) {
            ImportResult r;
            RowError e;
            e.code = QLatin1String(err::E_SYNC_INIT);
            e.message = QStringLiteral("wconn not available in worker");
            r.errors.append(e);
            sp->set_value(r);
            return;
        }

        // CapturedWriteTemplate branch C: fresh session capture around the UPSERT writes.
        WriteTxn txn(*wconnPtr_);
        QString txnErr;
        if (!txn.begin(&txnErr)) {
            ImportResult r;
            RowError e;
            e.code = QLatin1String(err::E_SYNC_INIT);
            e.message = txnErr;
            r.errors.append(e);
            sp->set_value(r);
            return;
        }

        // C-02 fix: check rec_->begin() return value; if it fails, the import cannot be
        // session-captured and the changelog entry would be missing → reject the whole import.
        QString sessionErr;
        if (!rec_->begin(hPtr_, canonicalSyncTables_, &sessionErr)) {
            txn.rollback();
            ImportResult errResult;
            RowError e;
            e.code = QLatin1String(err::E_SYNC_INIT);
            e.message =
                QStringLiteral("session begin failed (changelog unavailable): %1").arg(sessionErr);
            errResult.errors.append(e);
            sp->set_value(errResult);
            return;
        }

        // C-05 fix: pass manageTransaction=false so ImportService does not open an inner
        // db.transaction() while WriteTxn holds the outer BEGIN IMMEDIATE.
        detail::ImportService svc;
        ImportResult result = svc.run(profile, catalog, xlsxPath, opts, *wconnPtr_,
                                      /*manageTransaction=*/false);

        if (result.ok) {
            qint64 localSeq = 0;
            QString sealErr;
            const QString fp = guard_ ? guard_->fingerprint() : QString();
            // H-01 fix: save prevSeq so any failure path can roll back the counter,
            // keeping origin_seq contiguous even when the transaction is rolled back.
            const qint64 prevSeqImport = localOriginSeq_;
            const qint64 originSeq = nextLocalOriginSeq();
            // C-02 fix: check sealInto() return value.
            QByteArray capturedChangeset;
            if (!rec_->sealInto(hPtr_, *clog_, *wconnPtr_, txn, config_.nodeId(), streamEpoch_,
                                config_.schemaVersion(), fp, 0, originSeq, &localSeq, &sealErr,
                                /*pushId=*/QString(), &capturedChangeset)) {
                rec_->abort();
                txn.rollback();
                rollbackOriginSeq(prevSeqImport);  // H-01 fix
                result.ok = false;
                RowError e;
                e.code = QLatin1String(err::E_SYNC_INIT);
                e.message = QStringLiteral("changelog seal failed: %1").arg(sealErr);
                result.errors.append(e);
                sp->set_value(result);
                return;
            }
            // H-01 fix: sealInto() returns success with localSeq==0 when changeset is empty
            // (no rows were captured). Roll back the unused originSeq to keep the counter
            // contiguous. The transaction is still committed (rows are written) but no
            // changelog entry was produced, which is consistent with a no-op import.
            if (localSeq == 0) {
                rollbackOriginSeq(prevSeqImport);
            }

            // M-01 fix: update __sync_table_state incrementally from the captured changeset
            // instead of a full resetFromBaseline() scan. This is O(changed rows) rather
            // than O(all sync table rows), which is much cheaper for large tables.
            if (!canonicalSyncTables_.isEmpty() && ts_) {
                QString tsErr;
                if (!capturedChangeset.isEmpty()) {
                    const QList<TableMutation> muts = CapturedWriteTemplate::extractMutationsStatic(
                        capturedChangeset, *wconnPtr_, canonicalSyncTables_);
                    if (!ts_->applyMutations(*wconnPtr_, muts, streamEpoch_, fp, originSeq,
                                             &tsErr)) {
                        txn.rollback();
                        rollbackOriginSeq(prevSeqImport);
                        result.ok = false;
                        RowError e;
                        e.code = QLatin1String(err::E_SYNC_INIT);
                        e.message =
                            QStringLiteral("table_state update failed after import: %1").arg(tsErr);
                        result.errors.append(e);
                        sp->set_value(result);
                        return;
                    }
                }
            }

            // C-02 fix: check txn.commit() return value.
            QString commitErr;
            if (!txn.commit(&commitErr)) {
                rollbackOriginSeq(prevSeqImport);  // H-01 fix: commit failed = txn rolled back
                result.ok = false;
                RowError e;
                e.code = QLatin1String(err::E_SYNC_INIT);
                e.message = QStringLiteral("transaction commit failed: %1").arg(commitErr);
                result.errors.append(e);
            }
        } else {
            rec_->abort();
            txn.rollback();
        }
        sp->set_value(result);
    });

    // Block until worker executes the task (with timeout).
    // The shared_ptr keeps the promise alive even if we return early.
    if (future.wait_for(std::chrono::seconds(60)) == std::future_status::timeout) {
        ImportResult r;
        RowError e;
        e.code = QLatin1String(err::E_SYNC_INIT);
        e.message = QStringLiteral("submitImportSync timed out after 60s");
        r.errors.append(e);
        return r;
    }
    return future.get();
}

// nextLocalOriginSeq —— 预递增并返回下一个本地 origin_seq。
// 做什么：前置自增 localOriginSeq_ 并返回新值（即「先 +1 再用」）。
// 为什么前置自增：origin_seq 必须从 1 开始、连续不重复；每次写入预分配一个。
// 关键不变量：返回后若事务最终回滚，【必须】调 rollbackOriginSeq() 退回，否则会留下空洞。
// 线程：worker（仅在写线程串行调用，故无锁即安全）。复杂度：O(1)。
qint64 SyncWorker::nextLocalOriginSeq() {
    return ++localOriginSeq_;
}

// rollbackOriginSeq —— 事务回滚后把 origin_seq 计数器退回。
// H-01 fix: restore localOriginSeq_ to prevSeq after a transaction rollback so that
// the next successful write receives a contiguous seq (no gap).
// 【H-01 修复】事务回滚后把 localOriginSeq_ 复位到 prevSeq（即本次写入前的值），
//   使下一次成功写入拿到连续的 seq、不留空洞。
// 为什么必须手动退回：DB 事务回滚只撤销「表数据」改动，并不会回退这个内存里的计数器；
//   若不退回，被预分配但未落库的 seq 就成了空洞，对端 AppliedVectorStore::check() 会把它
//   误判为 gap，进而触发不必要的基线回退。参数：prevSeq 写入前应恢复到的值。线程：worker。
void SyncWorker::rollbackOriginSeq(qint64 prevSeq) {
    localOriginSeq_ = prevSeq;
}

// drainQuarantine —— 重放隔离区中「现已可应用」的 payload（M-06 修复配套逻辑）。
// 做什么：按当前 schema 版本从 QuarantineStore 取出已成熟的条目，逐个解码；若是 changeset
//   就走正常应用路径 processChangesetArtifact，成功则 markReplayed 将其移出隔离区。
// 为什么：之前因本地 schema 版本过低而被隔离的「来自未来」的变更，在本地 schema 升级或
//   套用基线之后就变得可应用了——本函数把它们捞出来补放，避免这些变更永久滞留隔离区。
// 何时调用：schema 升级 / 基线套用之后。前置：必须在 worker 线程且 wconnPtr_ 有效。
// 线程：worker。副作用：可能应用若干变更并删除对应隔离行。复杂度：O(成熟条目数)。
void SyncWorker::drainQuarantine() {
    if (!wconnPtr_ || !quarantine_ || !codec_)
        return;
    const auto readyPayloads = quarantine_->drainReady(*wconnPtr_, config_.schemaVersion());
    for (const auto& entry : readyPayloads) {
        const qint64 qid = entry.first;
        const QByteArray& payload = entry.second;
        DecodeResult qdec;
        QString qerr;
        if (codec_->decode(payload, &qdec, &qerr) && qdec.kind == PayloadKind::Changeset) {
            if (processChangesetArtifact(qdec, QString()))
                quarantine_->markReplayed(*wconnPtr_, qid);
        }
    }
}

// submitCaptureWriteSync —— 把一批 RowMutation 经「捕获写模板」落库（比对会话保存用）。
// C-05 fix: routes RowMutations through CapturedWriteTemplate (session capture + changelog seal)
// instead of directly through UpsertExecutor. This ensures comparison-session saves produce
// changelog entries that are broadcast to peers, matching the semantics of a normal local write.
// 【C-05 修复】让这批改动走 CapturedWriteTemplate(session 捕获 + 封存 changelog)，而非直接
//   走 UpsertExecutor。这样「比对会话的保存」与「普通本地写」语义完全一致——其改动同样
//   进 changelog 并被广播给 peer，而不是绕过捕获、对端永远收不到。
// 做什么：投递闭包到 worker 线程并同步等待(最多 60s)；空 mutations 直接成功返回。
// 参数：mutations 待写的行变更；syncTables 涉及的同步表(供 session attach)；err 错误回填。
// 返回：是否成功落库。线程：调用方线程投递+阻塞，执行在 worker 线程。
bool SyncWorker::submitCaptureWriteSync(const QList<RowMutation>& mutations,
                                        const QStringList& syncTables, QString* err) {
    if (!isRunning() || !wconnPtr_) {
        if (err)
            *err = QStringLiteral("SyncWorker not ready");
        return false;
    }
    if (mutations.isEmpty())
        return true;

    auto sharedPromise = std::make_shared<std::promise<QPair<bool, QString>>>();
    std::future<QPair<bool, QString>> future = sharedPromise->get_future();

    enqueue([this, mutations, syncTables, sp = sharedPromise]() {
        if (!wconnPtr_ || !tpl_) {
            sp->set_value(qMakePair(false, QStringLiteral("wconn/tpl not available")));
            return;
        }

        WriteParams p;
        p.kind = WriteKind::LocalWrite;
        p.origin = config_.nodeId();
        p.epoch = streamEpoch_;
        // H-01 fix: save prevSeq so we can roll back if execute() rolls back the transaction.
        const qint64 prevSeqCapture = localOriginSeq_;
        p.seq = nextLocalOriginSeq();
        p.schemaVer = config_.schemaVersion();
        p.schemaFp = guard_ ? guard_->fingerprint() : QString();
        p.mutations = mutations;
        p.syncTables = syncTables.isEmpty() ? canonicalSyncTables_ : syncTables;

        WriteResult res = tpl_->execute(p);
        if (!res.ok) {
            rollbackOriginSeq(prevSeqCapture);  // H-01 fix
        } else if (res.localChangelogSeq == 0) {
            // H-01 fix: sealInto() returned success with seq==0 (empty changeset — no actual
            // row changes were captured by the session). Roll back the unused originSeq to
            // keep origin_seq contiguous and avoid spurious gaps seen by peers.
            rollbackOriginSeq(prevSeqCapture);
        }
        sp->set_value(qMakePair(res.ok, res.errorMsg.isEmpty() ? res.errorCode : res.errorMsg));
    });

    if (future.wait_for(std::chrono::seconds(60)) == std::future_status::timeout) {
        if (err)
            *err = QStringLiteral("submitCaptureWriteSync timed out after 60s");
        return false;
    }
    const auto result = future.get();
    if (!result.first && err)
        *err = result.second;
    return result.first;
}

// startAckWait —— 武装「前台 sync() 正在等 ACK」的计时。
// I-19: Signal worker that a foreground sync() is waiting for ACK.
// 【I-19】告诉 worker：有一次前台 sync() 正在等待对端 ACK。
// 做什么：置 ackWaiting_=true，并把超时绝对时刻设为「现在 + 配置的最大等待时长」。
// 为什么用 atomic 而非加锁：本函数在「调用方线程」执行，worker 主循环在另一线程读这两个
//   值；用 std::atomic 即可安全共享，省去一把锁。到点仍无 ACK，主循环发 E_SYNC_ACK_TIMEOUT。
// 线程：调用方线程。副作用：改两个 atomic 标志。复杂度：O(1)。
void SyncWorker::startAckWait() {
    ackWaiting_ = true;
    ackDeadlineMs_ = QDateTime::currentMSecsSinceEpoch() + config_.ackMaxDelayMs();
}

// cancelAckWait —— 撤销「等 ACK」状态（连同清空 ACK 窗口与在途 push 标记）。
// C-1 fix: cancel a pending ACK wait that was armed before enqueueDrain but no payload was sent.
// 【C-1 修复】撤销「已武装但实际没发出任何 payload」的 ACK 等待。
// 为什么需要：sync() 会先 startAckWait 再 enqueueDrain；若这次 drain 其实没写出任何工件
//   （无新变更可发），就没有对端会来 ACK，前台会永久挂起。此时主动取消，让 sync() 立即
//   按「已完成、无需等待」收尾。同时清空 pendingPushId_ / pendingAckWindow_，避免残留状态
//   污染下一次 sync。线程：worker（在 enqueueDrain 闭包内调用）。复杂度：O(1)。
void SyncWorker::cancelAckWait() {
    ackWaiting_ = false;
    ackDeadlineMs_ = 0;
    pendingPushId_.clear();
    pendingAckWindow_.clear();
}

// enqueueDrain —— 在 worker 线程上排入并执行一次「立即抽干」（扫 inbox + 广播），同步等结果。
// C-02: Enqueue an immediate drain cycle on the worker thread.
// 【C-02】在 worker 线程排入一次立即抽干循环。
// 做什么：把一个闭包压入队列并阻塞等其结果；闭包内 ① scanInbox 先应用任何待处理入站变更，
//   ② 对每个未驱逐对端直接调 broadcastTopeer(传 ackedEntries)，③ 用收集到的 (peer,origin,
//   epoch,maxOriginSeq) 构建覆盖「所有被广播 origin」的完整 ACK 窗口 pendingAckWindow_。
// 为什么前台 sync() 走这里而不走后台 broadcast()：sync() 需要把刚产生的本地变更尽快发出，
//   并据此知道「要等哪些 ACK 才算完成」——只有直接调 broadcastTopeer 才能拿到 ackedEntries
//   来构建精确的 ACK 窗口(C-01：必须覆盖转发的远端 origin，而不止本地 origin)。
// 参数：err 可选错误回填。返回：本次是否真的写出了工件（false → 无可发，sync 可立即完成）。
// 线程：调用方线程投递 + 阻塞等待，实际执行在 worker 线程。
bool SyncWorker::enqueueDrain(QString* err) {
    if (!isRunning()) {
        if (err)
            *err = QStringLiteral("SyncWorker is not running");
        return false;
    }
    auto sharedPromise = std::make_shared<std::promise<bool>>();
    std::future<bool> future = sharedPromise->get_future();
    enqueue([this, sp = sharedPromise]() {
        QString taskErr;
        scanInbox();  // apply any pending inbound payloads

        // C-01 fix: collect (peer, origin, epoch, maxOriginSeq) for every artifact written
        // during this drain so the ACK window covers ALL origins, not just the local one.
        // We call broadcastTopeer directly (bypassing broadcast()) so we can pass ackedEntries.
        QList<PendingAckEntry> broadcastedEntries;
        bool wrote = false;
        for (const QString& peer : config_.peerNodes()) {
            if (isPeerEvicted(peer))
                continue;
            QString peerErr;
            if (broadcastTopeer(peer, &peerErr, &broadcastedEntries))
                wrote = true;
            if (!peerErr.isEmpty() && taskErr.isEmpty())
                taskErr = peerErr;
        }
        // Flush ACK channel (same as broadcast() does).
        QString ackErr;
        if (!ackChan_->flush(*codec_, &ackErr)) {
            emit errorOccurred({err::E_SYNC_TRANSPORT, Severity::Warning, QStringLiteral("ack"),
                                config_.nodeId(), ackErr});
        }
        evaluatePeers();

        if (!taskErr.isEmpty()) {
            emit errorOccurred({err::E_SYNC_TRANSPORT, Severity::Error, QStringLiteral("sync"),
                                config_.nodeId(), taskErr});
        }

        // Build pendingAckWindow_ from the entries we actually broadcast this round.
        // Deduplicate by (peer, origin, epoch): keep the maximum originSeq seen per tuple.
        if (wrote) {
            QHash<QString, PendingAckEntry> windowMap;
            for (const PendingAckEntry& e : broadcastedEntries) {
                const QString key = e.peer + QLatin1Char('|') + e.origin + QLatin1Char('|') +
                                    QString::number(e.epoch);
                auto it = windowMap.find(key);
                if (it == windowMap.end()) {
                    windowMap.insert(key, e);
                } else if (e.targetSeq > it->targetSeq) {
                    it->targetSeq = e.targetSeq;
                }
            }
            pendingAckWindow_.clear();
            for (const PendingAckEntry& e : windowMap)
                pendingAckWindow_.append(e);

            if (pendingAckWindow_.isEmpty()) {
                // Nothing needs ACKing (e.g. all already acked) — cancel wait.
                ackWaiting_ = false;
                ackDeadlineMs_ = 0;
            }
        } else if (!wrote) {
            // wrote=false: no artifacts sent at all, no need to wait for ACKs.
            if (ackWaiting_.load()) {
                ackWaiting_ = false;
                ackDeadlineMs_ = 0;
            }
        }
        sp->set_value(wrote);
    });
    if (future.wait_for(std::chrono::seconds(60)) == std::future_status::timeout) {
        if (err)
            *err = QStringLiteral("enqueueDrain timed out after 60s");
        return false;
    }
    return future.get();
}

// enqueueSelectionPush —— 在 worker 线程排入一次「选择性推送」（异步，进度经信号上报）。
// C-01: Enqueue a selection push — SelectionResolver → FkClosureBuilder → ChunkStreamer
//        → PayloadCodec → OutboxWriter (design §5.5 / §7.3).
// 【C-01】选择性推送的完整流水：SelectionResolver(解析选中集) → FkClosureBuilder(补全外键
//   依赖闭包，保证父行随子行一起推) → ChunkStreamer(切片) → PayloadCodec(编码) →
//   OutboxWriter(写出工件)（对应设计文档 §5.5 / §7.3）。
// 做什么：把整套流程打包成闭包入队，本方法立即返回(不阻塞)；闭包内开一个短命只读连接做
//   快照读，逐表按拓扑序构建冻结清单与行数据、切成 chunk 工件发出。进度/错误通过
//   progressUpdated/errorOccurred 信号异步上报。
// 为什么用独立只读连接做快照：选择性推送要读「某一致时刻」的数据；用独立 rconn 读、并在
//   removeDatabase 前确保其完全析构(否则 Qt 报 "connection still in use")。
// catalog 是调用方线程预拍的快照(跨线程安全)。线程：调用方线程投递，执行在 worker 线程。
void SyncWorker::enqueueSelectionPush(const SyncSelection& selection,
                                      const detail::SchemaCatalog& catalog) {
    enqueue([this, selection, catalog]() {
        if (!wconnPtr_) {
            emit errorOccurred({err::E_SYNC_INIT, Severity::Error, QStringLiteral("syncSelected"),
                                QString(), QStringLiteral("Worker wconn not ready")});
            return;
        }

        // Steps 1-3: open a short-lived read-only connection for snapshot reads.
        // IMPORTANT: rconn must be fully destroyed BEFORE QSqlDatabase::removeDatabase() is
        // called; otherwise Qt warns "connection still in use".  We achieve this by scoping
        // rconn inside a block and deferring removeDatabase to after the block exits.
        const QString rConnName = config_.sqlitePath() + QStringLiteral("_sel_ro_") +
                                  QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
        struct SnapResult {
            enum Kind { Ok, InitError, SelectionError, FkError } kind = Ok;
            const char* code = nullptr;
            QString msg;
        } snap;
        QList<FkClosureBuilder::Entry> manifest;
        {
            QSqlDatabase rconn = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), rConnName);
            rconn.setDatabaseName(config_.sqlitePath());
            do {
                if (!rconn.open()) {
                    snap = {SnapResult::InitError, err::E_SYNC_INIT,
                            QStringLiteral("Cannot open read connection")};
                    break;
                }
                // H-01 fix: explicit read transaction so resolvePk() and build() share one
                // snapshot.
                if (!rconn.transaction()) {
                    snap = {SnapResult::InitError, err::E_SYNC_INIT,
                            QStringLiteral("Cannot begin read snapshot transaction")};
                    break;
                }
                // Step 2: resolve PK set.
                SelectionResolver resolver;
                QList<SelectionResolver::ResolveResult> resolved;
                QString resolveErr;
                if (!resolver.resolvePk(rconn, selection, &resolved, &resolveErr)) {
                    rconn.rollback();
                    snap = {SnapResult::SelectionError, err::E_SYNC_SELECTION_EMPTY, resolveErr};
                    break;
                }
                if (resolved.isEmpty()) {
                    rconn.rollback();
                    snap = {SnapResult::SelectionError, err::E_SYNC_SELECTION_EMPTY,
                            QStringLiteral("Selection resolved to zero rows")};
                    break;
                }
                // Step 3: FK closure + topo sort.
                // M-01 fix: use shared consistencyCache_ so pruneConsistentDependencies() works.
                FkClosureBuilder builder;
                QString buildErr;
                // H-02 fix: pass SyncSelection flags so includeFkDeps and pruneConsistent are
                // honoured.
                if (!builder.build(rconn, resolved, catalog, consistencyCache_,
                                   config_.maxSelectionSize(), &manifest, &buildErr,
                                   selection.includeFkDeps(), selection.pruneConsistent())) {
                    rconn.rollback();
                    const char* code = buildErr.contains(QLatin1String("cycle"))
                                           ? err::E_SYNC_FK_CYCLE_UNSUPPORTED
                                       : buildErr.contains(QLatin1String("large"))
                                           ? err::E_SYNC_SELECTION_TOO_LARGE
                                           : err::E_SYNC_FK_CLOSURE_MISSING;
                    snap = {SnapResult::FkError, code, buildErr};
                    break;
                }
                rconn.commit();
            } while (false);
            rconn.close();
        }  // rconn destroyed here — safe to removeDatabase
        QSqlDatabase::removeDatabase(rConnName);  // release read snapshot promptly (§5.5/E-11)

        if (snap.kind == SnapResult::InitError) {
            emit errorOccurred(
                {snap.code, Severity::Error, QStringLiteral("syncSelected"), QString(), snap.msg});
            return;
        }
        if (snap.kind == SnapResult::SelectionError || snap.kind == SnapResult::FkError) {
            // M-05 fix: cancel any pending ACK wait so the foreground caller is not left hanging.
            cancelAckWait();
            emit errorOccurred(
                {snap.code, Severity::Error, QStringLiteral("syncSelected"), QString(), snap.msg});
            return;
        }

        // Step 4: chunk into outbox artifacts.
        ChunkStreamer streamer;
        QList<ChunkStreamer::Chunk> chunks;
        QString streamErr;
        if (!streamer.stream(manifest, config_.nodeId(), QString(), config_.pushChunkBudgetBytes(),
                             *codec_, &chunks, &streamErr)) {
            // M-05 fix: cancel any pending ACK wait so the foreground caller is not left hanging.
            cancelAckWait();
            emit errorOccurred({err::E_SYNC_SELECTION_TOO_LARGE, Severity::Error,
                                QStringLiteral("syncSelected"), QString(), streamErr});
            return;
        }

        // Step 5: encode and write each chunk to the outbox.
        // H-01 fix: capture schemaFingerprint before entering the loop.
        const QString schemaFp = guard_ ? guard_->fingerprint() : QString();
        const QString pushId = chunks.isEmpty() ? QString() : chunks.first().pushId;

        // C-04 fix: record which push we're waiting to be fully ACKed.
        // processAckArtifact() will only complete ackWaiting_ when ALL chunks of THIS pushId
        // are ACKed, preventing stale or unrelated chunk ACKs from prematurely completing
        // the foreground sync() caller.
        pendingPushId_ = pushId;

        // M-05 / C-02: create push_progress(streaming) on the worker thread BEFORE writing chunks.
        // L-01 fix: ON CONFLICT DO UPDATE so re-sends refresh total_chunks/status/updated_ms.
        // H-02 fix: push_progress must be inserted FIRST so FrozenManifest FK constraint is
        // satisfied when foreign_keys=ON (push_progress.push_id is the referenced parent).
        if (!pushId.isEmpty() && wconnPtr_) {
            QSqlQuery ins(*wconnPtr_);
            ins.prepare(QStringLiteral(
                "INSERT INTO __sync_push_progress "
                "(push_id, origin, peer, total_chunks, schema_ver, status, updated_ms)"
                " VALUES (?, ?, '', ?, ?, 'streaming', ?)"
                " ON CONFLICT(push_id) DO UPDATE SET"
                "  status='streaming', total_chunks=excluded.total_chunks,"
                "  updated_ms=excluded.updated_ms"));
            ins.addBindValue(pushId);
            ins.addBindValue(config_.nodeId());
            ins.addBindValue(chunks.size());
            ins.addBindValue(config_.schemaVersion());
            ins.addBindValue(QDateTime::currentMSecsSinceEpoch());
            // H-02 fix: check exec() so FK violations and other DB errors surface at the
            // root cause (parent row) rather than appearing later as a child manifest error.
            if (!ins.exec()) {
                cancelAckWait();
                emit errorOccurred({err::E_SYNC_TRANSPORT, Severity::Error,
                                    QStringLiteral("syncSelected"), QString(),
                                    QStringLiteral("push_progress insert failed: %1")
                                        .arg(ins.lastError().text())});
                return;
            }
        }

        // H-02 fix: persist FrozenManifest entries AFTER push_progress (parent) is inserted
        // so FK constraint is satisfied.  Errors are reported via errorOccurred; outbox is
        // skipped on failure so the caller doesn't receive partial/unrecoverable artifacts.
        if (!pushId.isEmpty() && wconnPtr_) {
            FrozenManifest fm;
            bool fmOk = true;
            for (const auto& chunk : chunks) {
                QString fmErr;
                if (!fm.save(*wconnPtr_, chunk.pushId, chunk.chunkSeq, chunk.entries, &fmErr)) {
                    emit errorOccurred({err::E_SYNC_TRANSPORT, Severity::Error,
                                        QStringLiteral("syncSelected"), QString(),
                                        QStringLiteral("FrozenManifest save failed for "
                                                       "chunk %1: %2")
                                            .arg(chunk.chunkSeq)
                                            .arg(fmErr)});
                    fmOk = false;
                    break;
                }
            }
            if (!fmOk) {
                cancelAckWait();
                return;  // abort outbox writes; push_progress row will time out/be cleaned
            }
        }

        for (const ChunkStreamer::Chunk& chunk : chunks) {
            PayloadHeader hdr;
            hdr.origin = config_.nodeId();
            hdr.originSeq = 0;  // SelectionPush has no changelog seq
            hdr.streamEpoch = streamEpoch_;
            hdr.schemaVer = config_.schemaVersion();
            hdr.schemaFingerprint =
                schemaFp;  // H-01 fix: must be filled so receiver SchemaGuard accepts
            hdr.pushId = chunk.pushId;
            hdr.chunkSeq = chunk.chunkSeq;
            hdr.totalChunks = chunk.totalChunks;

            SelectionPushBody body;
            body.totalChunks = chunk.totalChunks;
            body.frozenEntries = chunk.entries;
            for (const QVariantMap& row : chunk.rows)
                body.rows.append(row);

            QByteArray payload = codec_->encodeSelectionPush(hdr, body);
            // H-07 / H-08 fix: include center node as target peer to disambiguate artifacts.
            const QString centerPeer = config_.centerNodeId().isEmpty() ? QStringLiteral("center")
                                                                        : config_.centerNodeId();
            const QString artifactName = ddl::selectionPushArtifactName(
                hdr.origin, hdr.streamEpoch, chunk.pushId, chunk.chunkSeq, centerPeer);
            QString writeErr;
            if (!outbox_->write(artifactName, payload, &writeErr)) {
                emit errorOccurred({err::E_SYNC_TRANSPORT, Severity::Error,
                                    QStringLiteral("syncSelected"), QString(), writeErr});
                return;
            }
        }

        // Report outbox write as "Exporting" — ACK wait was armed by SyncEngine.
        SyncProgress p;
        p.state = SyncState::Exporting;
        p.percent = -1;
        emit progressUpdated(p);
    });
}

}  // namespace dbridge::sync
