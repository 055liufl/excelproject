// ============================================================================
// SyncEngine.cpp — ISyncEngine 实现门面的定义（前台请求 → 后台 SyncWorker 的转发器）
// ============================================================================
//
// 【这个文件是什么】
//   SyncEngine.h 中声明的那层“薄门面”的具体实现。本类【自身不碰数据库】：所有真正
//   会改库 / 会收发工件的活，都被它转交给后台单写线程 SyncWorker；而 worker 异步上报
//   的进度与错误（Qt 信号 progressUpdated / errorOccurred），又被它归集进一份加锁保护
//   的“只读快照”（progress_/result_/logs_/errors_），供 state()/progress()/logs()/
//   errors()/result() 这些 getter 跨线程安全返回。
//
// 【在引擎架构中的位置】
//   应用层 ── 只见 ISyncEngine 抽象（createSyncEngine() 返回的就是本类）
//       │  持有 SyncContext（每物理库一份的共享上下文）+ SyncWorker（后台单写线程）
//       ▼
//   SyncWorker ── 独占写连接，真正完成“捕获 / 应用 / 广播 / ACK”全套
//
// 【它编排的三类资源】
//   1) SyncContext ctx_：按物理库（dev+inode）唯一、登记在 SyncContextRegistry 的共享
//      上下文。本类把四个“转发回调”写进 ctx_（importFn / workerWriteFn /
//      workerCaptureWriteFn / rescanFn），供 BatchTransfer / ComparisonSession 在它们
//      自己的线程上把写操作路由进 worker；ctx_->gate 是前台互斥闸门；
//      ctx_->inboundTableGate 是比对暂存期的入站门控（构造 worker 时注入）。
//   2) SyncWorker worker_：后台单写线程，所有改库动作的唯一执行者（见 SyncWorker.h）。
//   3) DataBridge& bridge_：ETL 那半边的门面。初始化成功后本类调用
//      bridge_.setSyncActive(true,...) 把“绕过同步的直写导入”门控住（前台/后台互斥）。
//
// 【前台 vs 后台（贯穿全文件的关键模型）】
//   · 后台：worker 自驱地扫 inbox→apply→回 ACK→按需广播，无需本类介入。
//   · 前台：sync()/syncSelected() 是用户显式发起、受 ForegroundGate（ctx_->gate）
//     互斥的操作——同一时刻至多一个在跑，再来一个得 E_BUSY。这两者发起广播后，门控会
//     一直“持有”到对端 ACK 收齐（状态切 Completed）或超时/出错（切 Failed），才在
//     onWorkerProgress/onWorkerError 的终态分支里 release。stop() 只取消当前前台操作，
//     绝不停后台循环。
//
// 【线程模型（务必牢记）】
//   · 前台方法（initialize/sync/syncSelected/write/stop）在“调用方线程”执行；它们只是
//     投递请求 + 必要时同步等待 worker 结果，本身不持有写连接。
//   · onWorkerProgress/onWorkerError 是 worker 信号的槽——Qt 跨线程信号默认按队列连接，
//     会被投递到“创建本对象的线程”的事件循环里执行（即不在 worker 线程跑）。
//   · 五项快照（progress_/result_/logs_/errors_）一律由 snapMutex_ 保护；任何读写都先
//     加锁，故 const getter 能被任意线程安全调用（snapMutex_ 声明为 mutable）。
//
// 【协作者一览】
//   ISyncEngine（接口，语义见 include/dbridge/sync/ISyncEngine.h 的 ①~⑩）、
//   SyncWorker（SyncWorker.h）、SyncContext / SyncContextRegistry（SyncContext.h）、
//   DataBridge（dbridge/DataBridge.h）、错误码（dbridge/Errors.h）。
// ============================================================================
#include "sync/SyncEngine.h"

#include "dbridge/Errors.h"

#include <QDateTime>     // currentMSecsSinceEpoch：日志/错误打时间戳
#include <QDebug>        // 调试输出（开发期）
#include <QFileInfo>     // absoluteFilePath：把库路径规范化为 OS 绝对路径
#include <QObject>       // QObject::connect：订阅 worker 信号
#include <QSqlDatabase>  // 临时连接：解析 main 库的真实磁盘路径
#include <QSqlError>
#include <QSqlQuery>  // PRAGMA database_list 查询
#include <QUuid>      // 给临时连接生成全局唯一名，避免与其它连接重名

namespace dbridge::sync {

// ── 构造 ─────────────────────────────────────────────────────────────────────
// 做什么：仅保存对 DataBridge 门面的引用，不做任何重活（不开库、不起线程）。
// 为什么：真正的初始化推迟到 initialize()——构造一个引擎对象应当是零成本、不可失败的，
//         失败语义留给可返回错误的 initialize()。
// 参数：bridge —— 已打开的 ETL 门面；本类全程只持有其引用（生命周期由调用方保证更长）。
// 副作用：无。线程：构造线程。复杂度：O(1)。
SyncEngine::SyncEngine(DataBridge& bridge) : bridge_(bridge) {
}

// ── 析构 ─────────────────────────────────────────────────────────────────────
// 做什么：按“安全拆解”次序回收所有资源——解除直写门控 → 切断 ctx_ 上的转发回调
//         → 停止并等待 worker 退出 → 把本库的共享上下文从 registry 释放。
// 为什么这个顺序：
//   1) 先 setSyncActive(false)：让“绕过同步的直写导入”重新放行，避免引擎销毁后
//      DataBridge 仍以为同步在进行、继续门控直写（否则会一直拒绝直写）。
//   2) 再清空 ctx_ 的四个回调指针：BatchTransfer/ComparisonSession 可能仍持有 ctx_
//      并随时回调；把回调置空，确保它们在 worker 即将停掉时不会再被引到一个正在销毁的
//      worker 上（回调里都判过空，置空即等于“关门”）。
//   3) 然后 requestStop()+wait(5000)：协作式停 worker，最多等 5 秒让它排空队列、优雅退出；
//      worker_.reset() 触发其析构（其析构内部还会再 requestStop()+wait 兜底）。
//   4) 最后 release(canonicalKey_)：按 dev+inode 键把共享上下文的引用计数减一；归零时
//      registry 真正销毁该 SyncContext（I-12 修复用 canonicalKey_ 而非原始路径，保证
//      acquire/release 键一致，避免别名路径导致泄漏）。
// 副作用：停后台线程、放行直写、可能销毁共享上下文。
// 线程：析构线程（通常是持有 unique_ptr 的那条线程）。错误模式：无（析构不抛错）。
// 复杂度：受 worker 排空队列耗时支配（上限 5s 等待）。
SyncEngine::~SyncEngine() {
    // J-09: Unblock direct imports before tearing down.
    // 【J-09 修复】拆解前先放行直写导入（解除 setSyncActive 门控）。
    bridge_.setSyncActive(false);
    if (ctx_) {
        // 切断四个转发回调：自此 BatchTransfer/ComparisonSession 即使持有 ctx_ 也无法
        // 再把请求路由进 worker（它们调用前都会判空）。必须先于停 worker 完成。
        ctx_->importFn = nullptr;
        ctx_->workerWriteFn = nullptr;
        ctx_->workerCaptureWriteFn = nullptr;
        ctx_->rescanFn = nullptr;
    }
    if (worker_) {
        worker_->requestStop();  // 置停止标志并唤醒主循环（它会先排空剩余任务）
        worker_->wait(5000);     // 最多等 5 秒让线程优雅退出
        worker_.reset();         // 触发 SyncWorker 析构（内部再兜底 requestStop+wait）
    }
    if (ctx_) {
        // 引用计数减一；归零时 registry 销毁本库的 SyncContext（按 canonicalKey_ 释放）。
        SyncContextRegistry::instance().release(canonicalKey_);
        ctx_.reset();
    }
}

// ── initialize（ISyncEngine ①）─────────────────────────────────────────────
// 做什么：一次性把引擎从“空壳”带到“可同步”状态。流程分五步：
//   (a) 前置校验（不可重复初始化；配置须 isValid）；
//   (b) 把 SQLite 路径解析为 OS 文件真实身份（PRAGMA database_list 取 main 库实际路径）；
//   (c) 据此从 registry 获取/创建本库唯一的共享 SyncContext；
//   (d) 起后台 SyncWorker 线程，并阻塞等待它完成初始化（attach session、建 __sync_* 表等）；
//   (e) 把四个转发回调写进 ctx_，并 setSyncActive(true) 接管直写门控，标记 initialized_。
// 为什么要解析真实路径（b）：同一个库可能被以 URI 路径、相对路径、符号链接等多种字符串
//   引用；若直接拿原始字符串当 registry 的键，会把“同一物理库”误判成多份、各起一个写者，
//   破坏“一库一写者”的根本不变量。故先打开一次、问 SQLite 它到底落在哪个磁盘文件上，
//   再用该文件的 dev+inode 作为规范键（H-02 修复）。
// 参数：config 同步配置（节点身份/目录/阈值，须 isValid()）；err 失败原因输出（可空）。
// 返回：成功 true；任一步失败返回 false 并写 *err，同时把一条 Fatal 错误归集进 errors_。
// 副作用：起后台线程、登记全局 registry、阻塞直写导入；失败路径会完整回滚已起的资源。
// 错误模式：重复初始化 / 配置非法 / 文件身份解析失败 /
//   E_SYNC_SESSION_UNAVAILABLE（驱动未启用 session 扩展）/ E_SYNC_UNSUPPORTED_SCHEMA /
//   worker 初始化超时（默认 10s）。
// 线程：调用方线程；内部 waitForInit 会阻塞至 worker 完成或超时。复杂度：受 worker
// 建表/迁移耗时支配。
bool SyncEngine::initialize(const SyncConfig& config, QString* err) {
    // (a-1) 防重复初始化：本类设计为一次性，重复 init 视为调用错误。
    if (initialized_) {
        if (err)
            *err = QStringLiteral("Already initialized");
        return false;
    }
    // (a-2) 配置自检：节点 id、目录、对端列表等基本完整性（具体规则见 SyncConfig::isValid）。
    if (!config.isValid()) {
        if (err)
            *err = QStringLiteral("Invalid SyncConfig");
        return false;
    }

    // 深拷贝一份配置自持（configPtr_），后续 worker、ctx_、回调都引用这份稳定副本。
    configPtr_ = std::make_unique<SyncConfig>(config);
    setProgress(SyncState::Idle);  // 进入“空闲就绪”态，便于外部立即观察到引擎已存在。

    // H-02 fix: resolve the SQLite main library's actual path via PRAGMA database_list
    // before calling getOrCreate(), so URI paths, relative paths, and platform-specific
    // aliases all map to the same OS file identity (dev+inode / volume+fileindex).
    // 【H-02 修复】(b) 在 getOrCreate() 之前，先用 PRAGMA database_list 问出 SQLite 的
    // main 库实际落在哪个磁盘文件，使 URI 路径 / 相对路径 / 平台别名都归一到同一个 OS 文件
    // 身份（Unix 的 dev+inode、Windows 的 volume+fileindex）。这样 registry 才能正确判定
    // “是不是同一个库”。
    QString resolvedPath = configPtr_->sqlitePath();  // 兜底：解析失败时退回原始路径。
    {
        // 用 UUID 拼出全局唯一的临时连接名，避免与进程内其它 QSqlDatabase 连接重名冲突。
        const QString tmpConn = QStringLiteral("dbridge_se_resolve_") +
                                QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            // 内层作用域：确保 QSqlDatabase 句柄 tmp 在 removeDatabase() 之前先析构释放——
            // Qt 要求“移除连接前不得有该连接的活动副本”，否则会告警且移除不彻底。
            QSqlDatabase tmp = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), tmpConn);
            tmp.setDatabaseName(configPtr_->sqlitePath());
            if (tmp.open()) {
                QSqlQuery q(tmp);
                // PRAGMA database_list 返回 (seq, name, file) 三列；我们要 name=="main" 那行的
                // file 列（第 2 列，0 基索引）——它就是 SQLite 认定的主库真实文件路径。
                if (q.exec(QStringLiteral("PRAGMA database_list"))) {
                    while (q.next()) {
                        if (q.value(1).toString() == QLatin1String("main")) {
                            const QString p = q.value(2).toString();
                            if (!p.isEmpty())
                                resolvedPath = QFileInfo(p).absoluteFilePath();  // 规范成绝对路径
                            break;  // 只关心 main，找到即停。
                        }
                    }
                }
                tmp.close();
            }
            // 注意：即便 open 失败也无妨——resolvedPath 仍保留原始路径，由 getOrCreate 再处理。
        }
        // tmp 已离开作用域析构；现在可安全移除这个临时连接，不留痕迹。
        QSqlDatabase::removeDatabase(tmpConn);
    }

    // Acquire (or create) the shared SyncContext for this database file.
    // canonicalKey_ receives the dev+inode key used for release() (I-12 fix).
    // (c) 取/建本物理库唯一的共享 SyncContext。getOrCreate 用 resolvedPath 的 OS 文件身份
    // 作键：库已被别处打开则复用其上下文（引用计数 +1），否则新建。
    // 【I-12 修复】canonicalKey_ 会被回填为该 dev+inode 规范键——析构时必须用它（而非
    // 原始路径）调用 release()，否则别名路径会导致 acquire/release 键不一致而泄漏。
    QString ctxErr;
    ctx_ = SyncContextRegistry::instance().getOrCreate(resolvedPath, &canonicalKey_, &ctxErr);
    if (!ctx_) {
        // 取上下文失败（如同一库已被另一不兼容配置占用）：直接收束，记 Fatal。
        if (err)
            *err = ctxErr;
        appendError({err::E_SYNC_INIT, Severity::Fatal, "init", configPtr_->nodeId(), ctxErr});
        return false;
    }
    ctx_->config = *configPtr_;  // 把配置登记到共享上下文，供 worker 与其它复用者读取。

    // Start SyncWorker — it creates its own write connection in run() (I-01 / I-02 fix).
    // (d) 起后台单写线程。【I-01 / I-02 修复】写连接由 worker 在自己的 run() 内部创建，
    // 因此连接永远归属 worker 线程，绝不发生跨线程使用 QSqlDatabase。
    // 注入 ctx_->inboundTableGate：让 worker 与“比对会话”共享同一个入站门控实例
    // （比对暂存期间可对相关表的入站工件延迟应用）。
    worker_ = std::make_unique<SyncWorker>(*configPtr_, ctx_->inboundTableGate);
    // 订阅 worker 的两路信号。Qt 跨线程信号默认按队列连接——槽会在“本对象所属线程”的事件
    // 循环里执行（不在 worker 线程），所以槽内访问快照只需 snapMutex_，无需担心 worker 重入。
    QObject::connect(worker_.get(), &SyncWorker::progressUpdated,
                     [this](SyncProgress p) { onWorkerProgress(p); });
    QObject::connect(worker_.get(), &SyncWorker::errorOccurred,
                     [this](SyncError e) { onWorkerError(e); });
    worker_->start();  // 启动 QThread → 进入 run()：建连接、建 __sync_* 表、初始化各 store。

    // Block until worker finishes initialisation (or times out / fails).
    // 阻塞等待 worker 完成初始化（建连接/attach session/建表/初始化各 store），最多 10 秒。
    // waitForInit 底层是对 worker 的 initSemaphore_ 做 tryAcquire；返回 false = 超时未就绪。
    //
    // 失败分两类、各一个分支，但回滚动作完全相同（停 worker + 切回调 + 释放 ctx_）：
    //   · 本分支：waitForInit 返回 false —— 超时（worker 迟迟没发完成信号），也可能是它在
    //     超时前就写了 initError；用 initError 优先，空则记“Worker init timeout”。
    //   · 下个分支：waitForInit 返回 true 但 initError 非空 —— 即“按时完成但报告了错误”。
    if (!worker_->waitForInit(10000)) {
        const QString rawErr = worker_->initError();
        const QString displayErr =
            rawErr.isEmpty() ? QStringLiteral("Worker init timeout") : rawErr;
        if (err)
            *err = displayErr;
        // J-14: Propagate precise error code from worker initError prefix.
        // 【J-14 修复】worker 把精确错误码作为 initError 的前缀字符串带回；这里据前缀还原成
        // 规范错误码，让上层能精确区分“驱动缺 session 扩展”与“表结构不被支持”等致命原因，
        // 而非笼统的 E_SYNC_INIT。
        QString errCode = QLatin1String(err::E_SYNC_INIT);
        if (rawErr.startsWith(QLatin1String("E_SYNC_SESSION_UNAVAILABLE")))
            errCode = QLatin1String(err::E_SYNC_SESSION_UNAVAILABLE);
        else if (rawErr.startsWith(QLatin1String("E_SYNC_UNSUPPORTED_SCHEMA")))
            errCode = QLatin1String(err::E_SYNC_UNSUPPORTED_SCHEMA);
        appendError(
            {errCode, Severity::Fatal, QStringLiteral("init"), configPtr_->nodeId(), displayErr});
        // 完整回滚已起的资源：停线程（这里只等 3s，比正常析构的 5s 短，因为初始化卡住时
        // 不值得久等）→ 切断回调 → 释放共享上下文。注意 initialized_ 仍为 false。
        worker_->requestStop();
        worker_->wait(3000);
        worker_.reset();
        ctx_->importFn = nullptr;
        ctx_->workerWriteFn = nullptr;
        ctx_->workerCaptureWriteFn = nullptr;
        ctx_->rescanFn = nullptr;
        SyncContextRegistry::instance().release(canonicalKey_);
        ctx_.reset();
        return false;
    }
    // 第二类失败：worker 按时发了完成信号，但其 initError 非空（初始化逻辑内部失败）。
    // 处理与上一分支镜像一致——同样按前缀还原错误码并完整回滚。
    if (!worker_->initError().isEmpty()) {
        const QString rawErr = worker_->initError();
        if (err)
            *err = rawErr;
        // J-14: Propagate precise error code from worker initError prefix.
        // 【J-14 修复】同上：从 initError 前缀还原精确错误码。
        QString errCode = QLatin1String(err::E_SYNC_INIT);
        if (rawErr.startsWith(QLatin1String("E_SYNC_SESSION_UNAVAILABLE")))
            errCode = QLatin1String(err::E_SYNC_SESSION_UNAVAILABLE);
        else if (rawErr.startsWith(QLatin1String("E_SYNC_UNSUPPORTED_SCHEMA")))
            errCode = QLatin1String(err::E_SYNC_UNSUPPORTED_SCHEMA);
        appendError(
            {errCode, Severity::Fatal, QStringLiteral("init"), configPtr_->nodeId(), rawErr});
        worker_->requestStop();
        worker_->wait(3000);
        worker_.reset();
        ctx_->importFn = nullptr;
        ctx_->workerWriteFn = nullptr;
        ctx_->workerCaptureWriteFn = nullptr;
        ctx_->rescanFn = nullptr;
        SyncContextRegistry::instance().release(canonicalKey_);
        ctx_.reset();
        return false;
    }

    // (e) 初始化成功——把四个“转发回调”安装进共享上下文 ctx_。这是本门面的精髓：它让
    // BatchTransfer / ComparisonSession 这些活在【其它线程】的协作者，能把“会改库”的操作
    // 统一漏斗进 worker 单写线程执行，从而：① 不跨线程触碰主库对象；② 改动都经 session 捕获、
    // 能被广播。所有回调都先判 worker_ 是否还在（析构时会被置空 → 自动“关门”）。

    // I-04: Wire importFn so BatchTransfer can route imports through SyncWorker.
    // Profile/catalog snapshots are taken on the calling thread (safe) and forwarded to
    // the worker so it never touches DataBridge::db_ or catalog_ from the wrong thread.
    // 【I-04 修复】importFn：BatchTransfer 的导入改走这里。profile/catalog 是“调用方线程上
    // 拍好的快照”，原样转给 worker——worker 永不从错误的线程去碰 DataBridge::db_ 或 catalog_。
    ctx_->importFn = [this](const QString& xlsxPath, const ImportOptions& opts,
                            const detail::ProfileSpec& profile,
                            const detail::SchemaCatalog& catalog) -> ImportResult {
        return worker_->submitImportSync(opts, xlsxPath, profile, catalog);
    };
    // workerWriteFn：把任意“需要写连接才能干”的通用任务（签名 (QSqlDatabase&,QString*)->bool）
    // 投递到 worker 同步执行。worker_ 为空（已拆解）时短路返回 false。
    ctx_->workerWriteFn = [this](const std::function<bool(QSqlDatabase&, QString*)>& task,
                                 QString* taskErr) {
        return worker_ && worker_->submitWriteSync(task, taskErr);
    };
    // C-05 fix: route comparison-session saves through CapturedWriteTemplate so writes are
    // session-captured and broadcast to peers like normal local writes.
    // 【C-05 修复】workerCaptureWriteFn：把“比对会话的保存”也经 CapturedWriteTemplate 落库，
    // 使其与普通本地写完全同语义——同样被 session 捕获、写 changelog、广播给 peer
    // （区别于绕过捕获、不会被同步出去的裸直写）。
    ctx_->workerCaptureWriteFn = [this](const QList<RowMutation>& mutations,
                                        const QStringList& syncTables, QString* captureErr) {
        return worker_ && worker_->submitCaptureWriteSync(mutations, syncTables, captureErr);
    };
    // rescanFn：请求 worker 立刻重扫一次 inbox（如外部刚投了工件、想尽快应用时调用）。
    ctx_->rescanFn = [this]() {
        if (worker_)
            worker_->requestRescan();
    };

    // J-09: Block direct DataBridge::importExcel() while sync is active.
    // M-02: pass sync-monitored tables so importExcel() can allow non-sync profiles through.
    // 【J-09 修复】同步活动期间，门控住 DataBridge::importExcel() 的直写（前台/后台互斥）。
    // 【M-02 修复】把“被同步监控的表清单”一并传入：这样只涉及非同步表的 profile 仍可放行直写，
    // 不至于因开了同步而把所有导入全堵死。
    bridge_.setSyncActive(true, configPtr_->syncTables());

    initialized_ = true;  // 至此引擎完全就绪，后续 sync/write 等方法才会真正干活。
    appendLog(Severity::Info, QStringLiteral("init"),
              QStringLiteral("SyncEngine initialized for node ") + configPtr_->nodeId());
    return true;
}

// ── sync（ISyncEngine ②）─────────────────────────────────────────────────────
// 做什么：手动触发一轮“收发抽干（drain）”——让 worker 扫 inbox 应用入站、并把本地累积的
//         changelog 打包成 outbox 工件广播给各 peer。
// 为什么是“前台操作”：它由用户显式发起，受 ForegroundGate（ctx_->gate）互斥，同一时刻至多
//         一个；后台 worker 的自驱收发循环不受此限、照常运行。
// ACK 窗口的关键时序（见 C-1 修复）：必须在 enqueueDrain 之前先 startAckWait() 武装好 ACK
//         截止时钟。否则存在竞态——若“广播完成”与“startAckWait()”之间对端 ACK 就到了，
//         那条 ACK 会被漏掉，ackWaiting_ 永远停在 true（直到超时），前台永远收不了束。
// 门控的释放时机（重要）：本方法成功受理后【不在此处释放门控】——门控要一直持有到 worker
//         上报终态：收齐 ACK → onWorkerProgress 切 Completed 时 release；超时/出错 →
//         onWorkerError 切 Failed 时 release。只有两个“当场就结束”的短路分支（drain 出错、
//         无任何负载可发）才在本方法内立即 release。
// 参数：err 失败原因输出（可空）。
// 返回：true = 已成功受理（不代表同步已完成！最终结果须看 result()/state()）；
//       false = 受理失败（未初始化 / E_BUSY / drain 出错）。
// 副作用：占用前台门控、武装/取消 ACK 等待、推动 worker 收发。
// 错误模式：未初始化 / E_BUSY（门控被占）/ E_SYNC_TRANSPORT（drain 失败）/
//          （异步）E_SYNC_ACK_TIMEOUT。线程：调用方线程；实际收发在 worker 线程。
bool SyncEngine::sync(QString* err) {
    if (!initialized_) {
        if (err)
            *err = QStringLiteral("Not initialized");
        return false;
    }
    // Acquire the foreground gate so at most one manual sync runs at a time
    // 抢前台门控，保证同一时刻至多一个手动 sync 在跑；抢不到则 tryAcquire 写 E_BUSY 并返 false。
    if (!ctx_->gate.tryAcquire(err))
        return false;

    setProgress(SyncState::Importing);  // 进入“导入入站”阶段（仅用于对外展示进度语义）。

    setProgress(SyncState::Exporting, -1);  // 切到“导出/广播”阶段；-1 表示百分比未知。
    // C-1 fix: arm ACK wait BEFORE enqueueDrain to close the race window where an ACK arrives
    // between broadcast completion and startAckWait() — which would leave ackWaiting_ true
    // forever (or until timeout).  If no payload is actually sent, cancelAckWait() clears the
    // flag before any timeout can fire.
    // 【C-1 修复】先武装 ACK 等待，再 enqueueDrain，关闭上面所述的竞态窗口；若实际没发出任何
    // 负载，下方 hasPayload 分支会 cancelAckWait() 把标志清掉，免得空等到超时。
    worker_->startAckWait();
    QString drainErr;
    // 同步执行一轮抽干：返回值 hasPayload 表示“本轮是否真的写出了工件”（即是否有东西要等 ACK）。
    const bool hasPayload = worker_->enqueueDrain(&drainErr);
    if (!drainErr.isEmpty()) {
        // 抽干本身出错（传输层失败）：撤掉 ACK 等待、记错、切 Failed、当场还闸并返回 false。
        worker_->cancelAckWait();
        appendError({err::E_SYNC_TRANSPORT, Severity::Error, QStringLiteral("sync"),
                     configPtr_->nodeId(), drainErr});
        setProgress(SyncState::Failed, 0);
        ctx_->gate.release();
        if (err)
            *err = drainErr;
        return false;
    }
    if (!hasPayload) {
        // Nothing was broadcast — no ACK will come.
        // 没有任何工件被广播 → 不会有 ACK 回来 → 没什么可等：撤销 ACK 等待，直接判定本轮
        // 圆满完成（Completed/100%），当场还闸。这是“当场就结束”的成功短路路径。
        worker_->cancelAckWait();
        setProgress(SyncState::Completed, 100);
        ctx_->gate.release();
        return true;
    }
    // 确有工件发出：门控继续持有，静待对端 ACK。终态（Completed/Failed）会在 worker 的
    // 进度/错误信号槽里落定并释放门控；本方法到此“已成功受理”，返回 true。
    return true;
}

// ── stop（ISyncEngine ③）─────────────────────────────────────────────────────
// 做什么：协作式中止“当前前台操作”（正在进行的 sync/syncSelected）。
// 关键边界（务必理解）：stop ≠ 关引擎。它只取消当前前台操作的 ACK 等待并释放门控，让下一个
//   前台操作能开始；【绝不停后台 worker 循环】——inbox 扫描 / apply / 回 ACK / 广播照常继续。
//   真正的关停在析构里完成。
// 参数：err 失败原因输出（本实现不会失败，恒不写 err）。
// 返回：恒 true。未初始化时也直接返回 true（无操作可停，视为成功）。
// 副作用：撤销 ACK 等待、把前台状态切 Stopped、释放门控。
// 线程：调用方线程。错误模式：无。复杂度：O(1)。
bool SyncEngine::stop(QString* err) {
    if (!initialized_)
        return true;  // 还没 init，没有任何前台操作可中止，直接成功返回。
    // C-01 fix: stop() cancels only the current foreground operation (sync/syncSelected), NOT
    // the background SyncWorker loop. The worker keeps running: inbox scan, apply, ACK, broadcast.
    // Only the ACK wait for the active foreground operation is cancelled; the gate is released so
    // the next caller can start a new foreground operation.
    // 【C-01 修复】stop() 只取消当前前台操作（sync/syncSelected），不动后台 worker 循环：
    // 后者继续扫 inbox、应用、回 ACK、广播。这里仅取消当前前台操作的 ACK 等待，并释放门控，
    // 使下一个调用方可以开启新的前台操作。
    if (worker_)
        worker_->cancelAckWait();  // 撤销正在进行的 ACK 等待，避免门控被一直挂着等到超时。
    setProgress(SyncState::Stopped);
    releaseGateIfTerminal(SyncState::Stopped);  // H-02 fix: gate must be released on stop
                                                // 【H-02 修复】停止时务必还闸（Stopped 属终态）。
    return true;
}

// ── 只读快照 getter（ISyncEngine ④~⑧）────────────────────────────────────────
// 共性：五者都是“线程安全的快照读取”。worker 异步回调（onWorkerProgress/onWorkerError）
//   写入这几份共享状态时持 snapMutex_；getter 读取时同样持锁，并按值返回一份拷贝。
//   因此调用方拿到的永远是某一时刻的一致快照，可在任意线程安全调用（snapMutex_ 是 mutable，
//   故这些 const 方法也能加锁）。QMutexLocker 在作用域结束（return 语句求值后）自动解锁。
//   复杂度：O(1)（state/progress/result）或 O(n) 拷贝列表（logs/errors）。无副作用。

// ④ state —— 当前前台状态机状态。
SyncState SyncEngine::state() const {
    QMutexLocker lk(&snapMutex_);
    return progress_.state;
}

// ⑤ progress —— 完整进度快照（状态/百分比/已打包·已应用字节·变更数·冲突数等）。
SyncProgress SyncEngine::progress() const {
    QMutexLocker lk(&snapMutex_);
    return progress_;
}

// ⑥ logs —— 日志环形缓冲的拷贝（限长 kMaxLogs，超出丢最早，见 appendLog）。
QList<SyncLogEntry> SyncEngine::logs() const {
    QMutexLocker lk(&snapMutex_);
    return logs_;
}

// ⑦ errors —— 错误环形缓冲的拷贝（限长 kMaxErrors，见 appendError）。
QList<SyncError> SyncEngine::errors() const {
    QMutexLocker lk(&snapMutex_);
    return errors_;
}

// ⑧ result —— 最近一次“已完成操作”的最终战报（成败/终态/计数/各对端画像）。
//   仅在进度到达终态（Completed/Failed/Stopped）时由 onWorkerProgress 填充。
SyncResult SyncEngine::result() const {
    QMutexLocker lk(&snapMutex_);
    return result_;
}

// ── syncSelected（ISyncEngine ⑨）─────────────────────────────────────────────
// 做什么：上行“选择性推送”（FR-17）——只把指定的一批行推给对端。具体把整条链
//         SelectionResolver→FkClosureBuilder→ChunkStreamer→OutboxWriter 入队到 worker：
//         解析选择集 → 补全外键闭包 → 分片成 chunk → 写 outbox。
// 与 sync() 的差异：sync 是“把已捕获的本地 changelog 全量抽干广播”；syncSelected 是“按 PK
//         精挑一小撮行，连同其外键闭包，定向推送”。二者同为前台操作、共用门控与 ACK 窗口机制。
// 受理前校验（不占门控）：空/非法选择集属“受理前错误”，必须在抢门控之前就同步拒绝
//         （H-03 修复），返回 E_SYNC_SELECTION_EMPTY——否则会白白占用门控再失败。
// catalog 快照（C-01 修复）：在【调用方线程】先把 schema catalog 拍成快照（此处访问 DataBridge
//         是安全的），再把快照随选择集一起入队给 worker——worker 全程不碰主库的可变 catalog。
// 门控释放：同 sync()——成功受理后门控持续持有，直到 ACK 收齐（Completed）或超时/出错（Failed）
//         由 worker 的进度/错误信号槽落定并释放；只有两个早退分支（取快照失败）在本方法内还闸。
// 参数：selection 选择集（PK 集合 + 是否带外键闭包/剪枝一致项，见 SyncSelection.h）；err 输出。
// 返回：true=已受理；false=未初始化 / E_SYNC_SELECTION_EMPTY / 取快照失败 / E_BUSY。
// 错误模式：另含（异步）E_SYNC_FK_CLOSURE_MISSING / E_SYNC_SELECTION_TOO_LARGE /
// E_SYNC_ACK_TIMEOUT。 线程：调用方线程；解析/分片/写盘在 worker 线程。
bool SyncEngine::syncSelected(const SyncSelection& selection, QString* err) {
    if (!initialized_) {
        if (err)
            *err = QStringLiteral("Not initialized");
        return false;
    }
    // H-03 fix: pre-validate empty selection synchronously before occupying the gate.
    // Per spec, empty/invalid selection is a pre-acceptance error that must not acquire gate.
    // 【H-03 修复】空选择集要在“抢门控之前”同步校验：按规范这是受理前错误，不应占用门控。
    if (selection.isEmpty()) {
        if (err)
            *err = QString::fromLatin1(err::E_SYNC_SELECTION_EMPTY);
        return false;
    }
    if (!ctx_->gate.tryAcquire(err))  // 抢前台门控（被占则 E_BUSY）。
        return false;

    // C-01 fix: snapshot the catalog on the calling thread (safe), then enqueue the full
    // SelectionResolver → FkClosureBuilder → ChunkStreamer → OutboxWriter chain on the worker.
    // 【C-01 修复】在调用方线程拍 catalog 快照（安全），随后把整条选择推送链入队给 worker。
    detail::SchemaCatalog catalogSnapshot;
    QString snapErr;
    if (!bridge_.snapshotCatalog(&catalogSnapshot, &snapErr)) {
        // 拍快照失败：已占了门控，必须先还闸再返回，避免门控被永久占住。
        ctx_->gate.release();
        if (err)
            *err = QStringLiteral("Cannot snapshot schema catalog: ") + snapErr;
        return false;
    }

    setProgress(SyncState::Capturing);  // 进入“捕获/打包选择集”阶段。
    worker_->startAckWait();            // arm ACK deadline before enqueueing so no race window
                              // 入队前先武装 ACK 截止时钟，关闭竞态窗口（同 sync 的 C-1 理由）。
    worker_->enqueueSelectionPush(selection, catalogSnapshot);  // 异步入队，立即返回。
    // Gate stays held until ACK (Completed) or timeout (Failed) via onWorkerError.
    // 门控继续持有，直到 ACK 收齐（Completed）或超时（Failed）由信号槽释放。
    return true;
}

// ── write（ISyncEngine ⑩）─────────────────────────────────────────────────────
// 做什么：业务改库的“正确姿势”——把一批 RowMutation 经 session 录制器在 worker 单写线程上
//         同步落库，使每一行改动都被 SQLite changeset 记录，从而能在下次 sync() 时打包进
//         outbox 并传播给对端。
// 为什么必须走这里而非裸 SQL：裸直写在同步活动期会被门控阻止（E_SYNC_WRITE_BLOCKED）；即便
//         绕过去写进了库，也不会被 session 捕获、不会同步给别人，破坏一致性。
// 与前台操作（sync/syncSelected）的区别：write 不抢 ForegroundGate——它只是把写任务投递给
//         worker 同步执行并等结果（worker 内部自有串行化），不参与“前台互斥/ACK 窗口”那套。
//         第二个参数传空 {} 表示“不额外限定同步表集合”，用默认 canonicalSyncTables_。
// 参数：mutations 待写入的一批行级变更；err 失败原因输出（可空）。
// 返回：全部成功 true；worker 已不存在或任一写失败则 false 并写 err。
// 错误模式：未初始化 / E_SYNC_APPLY_CONSTRAINT / E_SYNC_APPLY_FK 等（来自 worker）。
// 线程：调用方线程；真正写入在 worker 线程串行执行，本调用同步等待其结果（最多约 60s）。
bool SyncEngine::write(const QList<RowMutation>& mutations, QString* err) {
    if (!initialized_) {
        if (err)
            *err = QStringLiteral("Not initialized");
        return false;
    }
    QString captureErr;
    // worker_ && ... 的短路：worker 已被拆解（析构中）时直接判失败，不解引用空指针。
    const bool ok = worker_ && worker_->submitCaptureWriteSync(mutations, {}, &captureErr);
    if (!ok && err)
        *err = captureErr;
    return ok;
}

// --- Private helpers ---
// ── 私有辅助 ─────────────────────────────────────────────────────────────────
// 以下方法或维护 snapMutex_ 保护的快照（appendLog/appendError/setProgress/onWorker*），
// 或编排门控/状态机（releaseGateIfTerminal）。除显式说明外都假定可被多线程调用，故内部加锁。

// appendLog —— 往日志环形缓冲追加一条（带毫秒时间戳）。
// 做什么：构造 SyncLogEntry，打上当前时间戳，加锁后追加到 logs_。
// 限长策略：上限 kMaxLogs=500；满了先丢最早一条（removeFirst）再追加，使内存有界（环形缓冲）。
// 参数：sev 级别、phase 所处阶段（如 "init"/"sync"）、msg 文本。副作用：改 logs_。
// 线程：可被任意线程调用（持 snapMutex_）。复杂度：O(1) 均摊。
void SyncEngine::appendLog(Severity sev, const QString& phase, const QString& msg) {
    SyncLogEntry e;
    e.epochMs = QDateTime::currentMSecsSinceEpoch();  // 在锁外构造，缩短临界区。
    e.severity = sev;
    e.phase = phase;
    e.message = msg;

    QMutexLocker lk(&snapMutex_);
    constexpr int kMaxLogs = 500;
    if (logs_.size() >= kMaxLogs)
        logs_.removeFirst();  // 丢弃最旧一条，保持上界。
    logs_.append(e);
}

// appendError —— 往错误环形缓冲追加一条已构造好的 SyncError。
// 限长策略：上限 kMaxErrors=200，同样“满则丢最早”。
// 线程：可被任意线程调用（持 snapMutex_）。复杂度：O(1) 均摊。副作用：改 errors_。
void SyncEngine::appendError(const SyncError& e) {
    QMutexLocker lk(&snapMutex_);
    constexpr int kMaxErrors = 200;
    if (errors_.size() >= kMaxErrors)
        errors_.removeFirst();
    errors_.append(e);
}

// setProgress —— 只更新进度快照里的状态与百分比两字段（其余字段不动）。
// 参数：st 目标状态；pct 百分比，默认 -1 表示“不确定/不更新具体进度数值”。
// 线程：可被任意线程调用（持 snapMutex_）。复杂度：O(1)。副作用：改 progress_。
void SyncEngine::setProgress(SyncState st, int pct) {
    QMutexLocker lk(&snapMutex_);
    progress_.state = st;
    progress_.percent = pct;
}

// onWorkerProgress —— worker progressUpdated 信号的槽：吞下 worker 的最新整份进度。
// 做什么：用 worker 上报的 p 整体替换本地 progress_ 快照；若 p 已到终态
//         （Completed/Failed/Stopped），把战报 result_ 的 finalState/ok 一并落定。
// 门控编排：终态意味着“当前前台操作结束”，故在锁外调用 releaseGateIfTerminal 还闸（让下一个
//         前台操作能开始）。注意 release 故意放在临界区【之外】——避免“持快照锁的同时再去
//         操作门控信号量”造成不必要的锁嵌套/潜在死锁，且 ctx_/gate 自身另有同步保证。
// 线程：在“本对象所属线程”的事件循环执行（Qt 队列连接），非 worker 线程。复杂度：O(1)。
void SyncEngine::onWorkerProgress(SyncProgress p) {
    {
        QMutexLocker lk(&snapMutex_);
        progress_ = p;
        if (p.state == SyncState::Completed || p.state == SyncState::Failed ||
            p.state == SyncState::Stopped) {
            result_.finalState = p.state;
            result_.ok = (p.state == SyncState::Completed);
        }
    }  // 释放 snapMutex_，再去碰门控。
    releaseGateIfTerminal(p.state);
}

// onWorkerError —— worker errorOccurred 信号的槽：归集错误，并在“前台操作进行中”时把它升级
//   为终态失败。
// 做什么：先无条件把错误同时记入 errors_ 缓冲与 logs_ 缓冲（appendError/appendLog 各自加锁）。
//   随后若错误足够严重（Fatal/Error）且【当前正处于前台操作的进行态】（Exporting 或 Capturing），
//   则把前台状态切到 Failed、写战报、并还闸——这就是 sync()/syncSelected() 受理后“异步失败”
//   的落定点（如 E_SYNC_ACK_TIMEOUT、传输失败等）。
// 为什么要判 Exporting/Capturing：只有这两态代表“有前台操作在等终态”。若错误发生在后台自驱
//   阶段（无前台操作在跑），就不应把它误判为前台失败、更不能去 release 一个本就没人持有的门控。
// 线程：本对象所属线程事件循环（非 worker 线程）。复杂度：O(1)。副作用：改 errors_/logs_/
//   progress_/result_，可能释放门控。
void SyncEngine::onWorkerError(SyncError e) {
    appendError(e);                             // 进错误环形缓冲
    appendLog(e.severity, e.phase, e.message);  // 同时落一条日志，便于时间线排查

    if (e.severity == Severity::Fatal || e.severity == Severity::Error) {
        bool terminalFailure = false;
        QMutexLocker lk(&snapMutex_);
        // 仅当确有前台操作在进行（Exporting=sync 广播中 / Capturing=syncSelected 打包中）时，
        // 才把这次错误升级为“前台终态失败”。
        if (progress_.state == SyncState::Exporting || progress_.state == SyncState::Capturing) {
            progress_.state = SyncState::Failed;
            progress_.percent = 0;
            result_.ok = false;
            result_.finalState = SyncState::Failed;
            result_.errors.append(e);
            terminalFailure = true;
        }
        // 注意：此处 release 仍在持 snapMutex_ 的临界区内（与 onWorkerProgress 的“锁外释放”
        // 略有不同），但 releaseGateIfTerminal 只触碰 ctx_->gate（独立信号量），不会回头再抢
        // snapMutex_，故不构成自锁。
        if (terminalFailure)
            releaseGateIfTerminal(SyncState::Failed);
    }
}

// releaseGateIfTerminal —— 若给定状态属“终态”则释放前台门控。
// 做什么：仅当 state ∈ {Completed, Failed, Stopped} 时 release 一次门控；否则什么都不做。
// 幂等/安全：先判 ctx_ 是否存在（析构清空后调用是安全的，直接返回）。本函数是“是否还闸”决策
//   的唯一收口，sync 成功收束 / 异步失败 / stop 都汇到这里，避免门控释放逻辑散落各处。
// 线程：调用方上下文（通常是信号槽线程）。复杂度：O(1)。副作用：可能 release 门控信号量。
void SyncEngine::releaseGateIfTerminal(SyncState state) {
    if (!ctx_)
        return;
    if (state == SyncState::Completed || state == SyncState::Failed || state == SyncState::Stopped)
        ctx_->gate.release();
}

// --- Factory ---
// ── 工厂函数 ─────────────────────────────────────────────────────────────────
// createSyncEngine —— ISyncEngine 的唯一构造入口（声明见 ISyncEngine.h）。
// 做什么：基于一个已打开的 DataBridge，造一个 SyncEngine 并以 ISyncEngine* 的形式用
//   unique_ptr 返回——调用方只依赖抽象接口，且独占所有权（析构即自动停 worker、释放上下文）。
// 参数：bridge 已打开的 ETL 门面（本引擎全程持其引用）。返回：持有 SyncEngine 的 unique_ptr。
// 线程：调用方线程。副作用：仅分配对象（不开库、不起线程——那些都在 initialize() 才发生）。
std::unique_ptr<ISyncEngine> createSyncEngine(DataBridge& bridge) {
    return std::make_unique<SyncEngine>(bridge);
}

}  // namespace dbridge::sync
