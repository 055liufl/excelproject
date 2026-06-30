// ============================================================================
// BatchTransfer.cpp —— IBatchTransfer 的实现（异步批量传输引擎，定义部分）
// ============================================================================
//
// 【职责】把同步阻塞的 ImportService/ExportService 包装成「后台执行 + 加锁快照
//   轮询 + 协作式停止」的异步传输器。类的整体设计、线程模型、停止协议、状态机
//   与协作者一览，详见 BatchTransfer.h 的文件头注释；本文件聚焦各方法的实现细节。
//
// 【阅读顺序建议】构造/析构 → startImport → runImport（工作线程，最深）→ startExport
//   → runExport → stop → getter。导入与导出几乎对称，导出侧只标注与导入不同之处。
//
// 【贯穿全文件的两条线程安全约定（看代码时随时对照）】
//   ① 凡是读写受保护状态（state_/progress_/errors_/result_），必先 QMutexLocker
//      抢 mutex_；锁的作用域用 {} 收得很窄，改完即放，绝不持锁去做耗时 I/O。
//   ② 停止标志是 std::atomic<bool>，跨线程读写无需锁；它与受保护状态是两套独立
//      机制（标志管「要不要停」，锁管「快照一致性」）。
// ============================================================================

#include "BatchTransfer.h"

#include "dbridge/Errors.h"

#include <QDebug>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QtConcurrent/QtConcurrent>

#include "profile/ProfileValidator.h"
#include "service/ErrorCollector.h"
#include "service/ExportService.h"
#include "service/ImportService.h"
#include "sync/SyncContext.h"

namespace dbridge {

// ---------------------------------------------------------------------------
// 构造 / 析构  (Construction / destruction)
// ---------------------------------------------------------------------------

// 构造：仅记下 DataBridge 引用，不启动任何线程、不打开任何连接。
// 副作用：无（两个停止标志在头文件里已就地初始化为 false）。复杂度 O(1)。
BatchTransfer::BatchTransfer(DataBridge& bridge) : bridge_(bridge) {
}

// 析构：尽力而为的优雅停机。
// 为什么必须等：后台 lambda 捕获了 this，若对象先于工作线程析构，工作线程随后
//   回写 importState_ 等成员就是 use-after-free。故这里先广播停止、再阻塞等待
//   两条任务真正结束，确保「没有任何工作线程还活着引用本对象」后才允许析构完成。
// 注意：这是「尽力」而非「立即」——仍受协作式停止约束，最坏需等到工作线程跑到
//   下一个停止检查点（例如一次 ImportService::run 内部不可中断的批写完成）。
// 线程安全：停止标志原子写；isRunning()/waitForFinished() 只在析构线程调用。
BatchTransfer::~BatchTransfer() {
    // 尽力优雅停机：通知两个任务并等待其结束。
    // Best-effort graceful shutdown: signal both tasks and wait.
    importStopRequested_.store(true);
    exportStopRequested_.store(true);
    if (importFuture_.isRunning()) {
        importFuture_.waitForFinished();
    }
    if (exportFuture_.isRunning()) {
        exportFuture_.waitForFinished();
    }
}

// ---------------------------------------------------------------------------
// startImport —— 非阻塞启动导入（在控制线程执行）
// ---------------------------------------------------------------------------
//
// 流程：① 前置校验（未在运行、必填参数齐）→ ② 在拥有者线程拷出 Profile+catalog
//   快照 → ③ 若该库正在同步则获取前台门控 → ④ 重置本方向轮询状态并置 Running →
//   ⑤ QtConcurrent::run 把 runImport 排进线程池 → 立即返回 true。
// 参数：options 导入选项（按值复制进 lambda）；err 失败原因出参（可空）。
// 返回：true=已排程后台任务；false=被拒绝（原因写入 *err）。
// 副作用：成功时重置 importProgress_/Errors_/Result_ 并把 importState_ 置 Running，
//   importFuture_ 指向新任务。失败时不改变任何可见状态。
// 线程安全：前半段持 mutex_；排程前用 lock.unlock() 主动放锁（不持锁启动线程，
//   避免工作线程一上来就抢锁却被自己阻塞）。复杂度：O(快照拷贝)。
bool BatchTransfer::startImport(const ImportOptions& options, QString* err) {
    QMutexLocker lock(&mutex_);
    // 拒绝重入：已在 Running 或 Stopping（收尾中）时不允许再次启动，否则会与
    // 尚未结束的旧任务争抢同一批状态字段。
    if (importState_ == TransferState::Running || importState_ == TransferState::Stopping) {
        if (err)
            *err = QStringLiteral("Import already running");
        return false;
    }

    // 必填参数校验：没有源文件路径直接拒绝（廉价的快速失败，省得开线程才发现）。
    if (options.xlsxPath.isEmpty()) {
        if (err)
            *err = QStringLiteral("xlsxPath is required");
        return false;
    }

    // 关键：在「DataBridge 拥有者线程」（即当前控制线程）上把 Profile 与 catalog
    // 各拷一份独立快照。工作线程之后只用这两个副本，绝不跨线程访问 DataBridge 内部
    // 的可变 catalog（杜绝数据竞争）。失败（profile 不存在等）即拒绝启动。
    detail::ProfileSpec profile;
    detail::SchemaCatalog catalog;
    if (!bridge_.snapshotProfileCatalog(options.profileName, &profile, &catalog, err))
        return false;
    const QString dbPath = bridge_.dbPath();

    // M-01 / H-06 修复：获取共享的前台门控（ForegroundGate）。捕获 shared_ptr 以防
    // 导入在后台运行期间 SyncEngine 销毁了 context 而导致 use-after-free。
    // M-01 / H-06 fix: acquire the shared ForegroundGate. Capture shared_ptr to prevent
    // use-after-free when SyncEngine destroys the context while import runs in background.
    //   · getExisting()：只查不增引用计数（见 SyncContext.h 的 J-10 约定）；返回 nullptr
    //     说明该库当前未在同步 → 无需门控，直接走原生路径。
    //   · ctx 存在则必须先 tryAcquire 门控（保证同一库同一时刻至多一个前台操作）；
    //     抢不到（已有 sync/比对/另一传输占用）即失败返回，原因写入 *err。
    //   · gateCtx 这份 shared_ptr 被一路捕获进下面的 lambda → 把 context 的存活期
    //     延长到「门控 release 之后」，杜绝悬垂。
    std::shared_ptr<sync::SyncContext> gateCtx;
    auto ctx = sync::SyncContextRegistry::instance().getExisting(dbPath);
    if (ctx) {
        if (!ctx->gate.tryAcquire(err))
            return false;
        gateCtx = ctx;  // 保活直到 lambda 执行完毕  // keep alive until lambda completes
    }

    // 重置全部导入状态：每次启动都让进度/错误/结果「清零重来」，使本轮轮询不会读到
    // 上一轮的残留（契约见 IBatchTransfer.h ①）。停止标志也复位为 false。
    // Reset all import state.
    importStopRequested_.store(false);
    importState_ = TransferState::Running;
    importErrors_.clear();
    importResult_ = ImportResult{};
    importProgress_ = TransferProgress{};
    lock.unlock();  // 在排程工作线程之前主动放锁（见函数头说明）。

    // 把 runImport 丢进 QtConcurrent 全局线程池异步执行；options/dbPath/profile/catalog
    // 均「按值」捕获（各自独立副本，与控制线程无共享可变状态）。任务体末尾负责释放
    // 门控——务必在 runImport 返回「之后」release，保证整个搬运期间门控始终持有。
    importFuture_ = QtConcurrent::run([this, options, dbPath, profile, catalog, gateCtx]() {
        runImport(options, dbPath, profile, catalog);
        if (gateCtx)
            gateCtx->gate.release();
    });
    return true;
}

// ---------------------------------------------------------------------------
// startExport —— 非阻塞启动导出（在控制线程执行）
// ---------------------------------------------------------------------------
//
// 与 startImport 完全对称（只是操作 export 侧的状态、标志、future 与 runExport），
// 各步骤含义同上，不再逐句重复。差异仅在：导出不涉及 SyncWorker 改道（导出是只读，
// 无需 session 捕获），其门控获取纯粹是为了与 sync/比对互斥。
// 返回/副作用/线程安全：同 startImport（只是落在 export 一侧）。
bool BatchTransfer::startExport(const ExportOptions& options, QString* err) {
    QMutexLocker lock(&mutex_);
    if (exportState_ == TransferState::Running || exportState_ == TransferState::Stopping) {
        if (err)
            *err = QStringLiteral("Export already running");
        return false;
    }

    if (options.xlsxPath.isEmpty()) {
        if (err)
            *err = QStringLiteral("xlsxPath is required");
        return false;
    }

    detail::ProfileSpec profile;
    detail::SchemaCatalog catalog;
    if (!bridge_.snapshotProfileCatalog(options.profileName, &profile, &catalog, err))
        return false;
    const QString dbPath = bridge_.dbPath();

    // M-06 / H-06 修复：导出同样获取门控，并捕获 shared_ptr 以保证生命周期安全。
    // M-06 / H-06 fix: export acquires gate, capturing shared_ptr for lifetime safety.
    std::shared_ptr<sync::SyncContext> gateCtx;
    auto ctx = sync::SyncContextRegistry::instance().getExisting(dbPath);
    if (ctx) {
        if (!ctx->gate.tryAcquire(err))
            return false;
        gateCtx = ctx;
    }

    // 重置导出侧全部状态并置 Running（语义同导入侧的复位段）。
    exportStopRequested_.store(false);
    exportState_ = TransferState::Running;
    exportErrors_.clear();
    exportResult_ = ExportResult{};
    exportProgress_ = TransferProgress{};
    lock.unlock();

    exportFuture_ = QtConcurrent::run([this, options, dbPath, profile, catalog, gateCtx]() {
        runExport(options, dbPath, profile, catalog);
        if (gateCtx)
            gateCtx->gate.release();
    });
    return true;
}

// ---------------------------------------------------------------------------
// runImport  (在工作线程上运行 / runs on worker thread)
// ---------------------------------------------------------------------------
//
// 这是整个类最核心的函数：实际的导入搬运逻辑，全程跑在 QtConcurrent 借来的后台线程。
// 它有「两条互斥的执行路径」：
//   路径 A（同步激活）：该库正在被 SyncEngine 同步 → 必须改走 SyncWorker 的写连接，
//     以便 SQLite session 能捕获本次导入产生的变更集（否则同步会漏掉这批写入）。
//     这条路径不在本线程开连接，而是把活儿委托给 ctx->importFn 同步执行。
//   路径 B（同步未激活，回退）：本线程自开一条独立的 QSQLITE 连接，直接调
//     ImportService::run 写库（不做 session 捕获）。
// 进度模型：因无法侵入 ImportService 内部逐行回报，故采用「粗粒度三段式」——
//   开始 0% → 运行中 50% → 完成 100%（被停止则回落 0%）。
// 停止检查点：本函数在三处轮询 importStopRequested_（路径 A 完成后、路径 B 开连接前、
//   路径 B 写库返回后）；命中即落 Stopped 并保留已得到的部分结果。
// 副作用：在锁内推进 importProgress_/Errors_/Result_/State_；路径 B 还会临时向
//   QSqlDatabase 全局注册表 add/removeDatabase 一条以 UUID 命名的连接。
// 线程安全：所有对受保护状态的读写都在窄作用域 {} + QMutexLocker 内完成。
//
void BatchTransfer::runImport(const ImportOptions& opts, const QString& dbPath,
                              const detail::ProfileSpec& profile,
                              const detail::SchemaCatalog& catalog) {
    // ── 路径 A：若该库已激活同步，则把导入改道给 SyncWorker ──────────────────────
    // I-04 / J-10：若该数据库的同步处于激活状态，则将导入路由经 SyncWorker。
    // J-10：使用 getExisting()（不增加 refCount、不创建上下文），从而绝不会创建出
    //   一个空上下文，也不会在 importFn 未设置时泄漏一个引用。
    // I-04 / J-10: Route import through SyncWorker if sync is active for this database.
    // J-10: Use getExisting() (no refCount increment, no context creation) so we never
    // create an empty context and never leak a reference if importFn is not set.
    {
        // 注意：这里重新取一次 dbPath（局部变量，遮蔽参数 dbPath）。两者值相同，
        // 此举只是就近向 bridge_ 要一次，逻辑上等价。
        const QString dbPath = bridge_.dbPath();
        if (!dbPath.isEmpty()) {
            auto ctx = sync::SyncContextRegistry::instance().getExisting(dbPath);
            // 仅当 context 存在「且」其 importFn 已被 SyncEngine 装好时，才走改道路径。
            // importFn 内部用 SyncWorker 的写连接 wconn 执行导入并捕获 session 变更。
            if (ctx && ctx->importFn) {
                ImportResult result = ctx->importFn(opts.xlsxPath, opts, profile, catalog);
                // 无需 release()——getExisting() 不增加 refCount。
                // No release() — getExisting() does not increment refCount.
                // 提交结果并结束。
                // Commit result and finish
                // 停止检查点①：改道执行期间若收到停止请求，落 Stopped 并保留部分结果。
                if (importStopRequested_.load()) {
                    QMutexLocker lock(&mutex_);
                    importResult_ = result;
                    importErrors_ = result.errors;
                    importProgress_ = TransferProgress{0, 0, -1};
                    importState_ = TransferState::Stopped;
                } else {
                    // 正常收尾：成功置 100%、失败置 0%，rowsDone 取实际写入行数，
                    // rowsTotal 置 -1（改道路径拿不到可靠的源总行数）。
                    QMutexLocker lock(&mutex_);
                    importResult_ = result;
                    importErrors_ = result.errors;
                    importProgress_ = TransferProgress{result.ok ? 100 : 0,
                                                       static_cast<qint64>(result.writtenRows), -1};
                    importState_ = result.ok ? TransferState::Completed : TransferState::Failed;
                }
                return;  // 路径 A 到此结束，不再走下面的回退路径。
            }
            // ctx 为 nullptr 或 importFn 未设置——无需 release。
            // ctx is nullptr or importFn not set — no release needed.
        }
    }
    // ── 路径 B：同步未激活，回退到本线程直接调用（不做 session 捕获）──────────────
    // Sync not active: fall back to direct DataBridge call (no session capture).

    // 开始时先把进度报为 0%（rowsDone=0、rowsTotal=-1 表示总量未知）。
    // Report 0 % at the start.
    {
        QMutexLocker lock(&mutex_);
        importProgress_ = TransferProgress{0, 0, -1};
    }

    // 停止检查点②：尊重「还没真正开始就已到达」的停止请求——直接落 Stopped 返回，
    // 连数据库都不打开（省掉无谓 I/O）。
    // Honour a stop request that arrived before we even started.
    if (importStopRequested_.load()) {
        QMutexLocker lock(&mutex_);
        importState_ = TransferState::Stopped;
        return;
    }

    // 使用在「本 QtConcurrent 线程」上新开的连接；切勿在此复用 DataBridge::db_
    //   （QSqlDatabase 有线程亲和性，跨线程使用同一连接是未定义行为）。
    // 由于无法向其注入逐行进度，故运行期间报 50%、完成时报 100%（提前停止则报 0%）。
    // Use a connection opened on this QtConcurrent thread; never reuse DataBridge::db_ here.
    // We cannot inject progress ticks into it, so we report 50 % while it
    // runs and 100 % when it finishes (or 0 % on early-stop).
    {
        QMutexLocker lock(&mutex_);
        importProgress_.percent = 50;
    }

    ImportResult result;
    // 连接名用 UUID 保证全局唯一：QSqlDatabase 的连接以「名字」为键登记在进程级
    // 注册表中，多个并发传输/重复启动若撞名会互相覆盖，故每次都生成不重名的连接名。
    const QString connName =
        QStringLiteral("dbridge_bt_import_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        // 内层 {} 限定 db 的作用域，保证它在 removeDatabase 之前先析构（Qt 要求：
        // 移除连接前不得再有该连接的 QSqlDatabase 副本存活，否则会有警告/句柄残留）。
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(dbPath);
        // 忙等待 5s：并发占用时重试这么久再报错，缓解同库被其它连接短暂锁住的情况。
        db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
        if (!db.open()) {
            // 打开失败：构造一条表级错误（row 缺省 0、column 空），用 E_OPEN_DB 码 +
            // 驱动给出的人类可读原因，附进结果错误列表。
            RowError e;
            e.code = QString::fromLatin1(err::E_OPEN_DB);
            e.message = db.lastError().text();
            result.errors.append(e);
        } else {
            // 导入方向必须开启外键约束（默认 SQLite 关闭），否则 FK 校验/级联失效。
            QSqlQuery q(db);
            q.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
            // 真正的逐行 ETL 委托给 ImportService（读 Excel→校验→外键查找→UPSERT）。
            detail::ImportService svc;
            result = svc.run(profile, catalog, opts.xlsxPath, opts, db);
        }
        db.close();
    }
    // 连接已不再被任何 QSqlDatabase 副本引用，安全地从全局注册表移除（释放句柄）。
    QSqlDatabase::removeDatabase(connName);

    // 停止检查点③：检查在「桥接调用进行中」到达的停止请求。
    // Check for stop request that arrived while the bridge call was in flight.
    // 注意：此时 ImportService 已（可能部分地）写完库——停止是协作式的，不会回滚已写入
    //   的数据；我们能做的是落 Stopped 并把已得到的 result 原样保留供调用方查看。
    if (importStopRequested_.load()) {
        QMutexLocker lock(&mutex_);
        importResult_ = result;  // 保留部分结果  // preserve partial result
        importErrors_ = result.errors;
        importProgress_ = TransferProgress{0, 0, -1};
        importState_ = TransferState::Stopped;
        return;
    }

    // 在锁内提交最终结果。
    // Commit results under the lock.
    {
        QMutexLocker lock(&mutex_);
        importResult_ = result;
        importErrors_ = result.errors;
        importProgress_.percent = 100;
        importProgress_.rowsDone = result.writtenRows;
        // 回退路径能从 ImportService 拿到真实读取行数 → 若 >0 则填为总量，否则仍 -1（未知）。
        importProgress_.rowsTotal = result.readRows > 0 ? static_cast<qint64>(result.readRows) : -1;
        // 终态：根据 result.ok 落 Completed 或 Failed。
        importState_ = result.ok ? TransferState::Completed : TransferState::Failed;
    }
}

// ---------------------------------------------------------------------------
// runExport  (在工作线程上运行 / runs on worker thread)
// ---------------------------------------------------------------------------
//
// 导出搬运逻辑，结构与 runImport 的「回退路径 B」高度对称，但有三处关键不同：
//   ① 导出没有 SyncWorker 改道分支——导出是只读操作，不产生需 session 捕获的变更，
//      故无需走写连接；连接以「只读」方式打开（QSQLITE_OPEN_READONLY=1）。
//   ② 多了 H-03 的「导出前 Profile 校验」前置步骤（见下）。
//   ③ rowsTotal 直接取 writtenRows（导出按查询结果逐行写出，总量即写出量）。
// 进度模型与停止检查点同 runImport（0%→50%→100%，两处停止轮询：开始时、写出后）。
//
void BatchTransfer::runExport(const ExportOptions& opts, const QString& dbPath,
                              const detail::ProfileSpec& profile,
                              const detail::SchemaCatalog& catalog) {
    // 开始时报 0%。
    {
        QMutexLocker lock(&mutex_);
        exportProgress_ = TransferProgress{0, 0, -1};
    }

    // 停止检查点①：还没开始就被请求停止 → 直接落 Stopped 返回。
    if (exportStopRequested_.load()) {
        QMutexLocker lock(&mutex_);
        exportState_ = TransferState::Stopped;
        return;
    }

    // 进入运行态，报 50%（粗粒度进度，理由同 runImport）。
    {
        QMutexLocker lock(&mutex_);
        exportProgress_.percent = 50;
    }

    ExportResult result;

    // H-03 修复：在打开数据库连接「之前」先校验导出 Profile。
    // 此前 BatchTransfer 绕过了 DataBridge::exportExcel 所做的导出模式 Profile 校验，
    // 使得非法 Profile（错误的列顺序、缺失的表等）能直达 ExportService，进而产生令人
    // 困惑的下游错误。这里就地校验，把错误尽早暴露出来。
    // H-03 fix: validate the export profile before opening a DB connection.
    // BatchTransfer previously bypassed the export-mode profile validation that
    // DataBridge::exportExcel performs, allowing invalid profiles (bad column order, missing
    // tables, etc.) to reach ExportService and produce confusing downstream errors.  Validate here
    // and surface errors early.
    {
        detail::ErrorCollector valErrors;
        detail::ProfileValidator validator;
        // validateForExport 失败：把收集器里的每条校验错误并入结果，直接落 Failed 返回，
        // 连数据库都不打开（快速失败）。注意此处进度强制置 100% 以示「流程已结束」。
        if (!validator.validateForExport(profile, catalog, &valErrors)) {
            for (const auto& e : valErrors.list())
                result.errors.append(e);
            QMutexLocker lock(&mutex_);
            exportResult_ = result;
            exportErrors_ = result.errors;
            exportProgress_.percent = 100;
            exportState_ = TransferState::Failed;
            return;
        }
    }

    // 连接名用 UUID 保证全局唯一（理由同 runImport）。
    const QString connName =
        QStringLiteral("dbridge_bt_export_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(dbPath);
        // 只读 + 忙等待 5s：导出不写库，以 READONLY 打开既表意清晰，也避免无谓持有写锁。
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY=1;QSQLITE_BUSY_TIMEOUT=5000"));
        if (!db.open()) {
            // 打开失败：附一条 E_OPEN_DB 表级错误（同 runImport）。
            RowError e;
            e.code = QString::fromLatin1(err::E_OPEN_DB);
            e.message = db.lastError().text();
            result.errors.append(e);
        } else {
            // 真正的导出（SELECT 查库→逐行写 .xlsx）委托给 ExportService。
            detail::ExportService svc;
            result = svc.run(profile, catalog, opts.xlsxPath, opts, db);
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

    // 停止检查点②：写出过程中到达的停止请求 → 落 Stopped 并保留部分结果。
    if (exportStopRequested_.load()) {
        QMutexLocker lock(&mutex_);
        exportResult_ = result;
        exportErrors_ = result.errors;
        exportProgress_ = TransferProgress{0, 0, -1};
        exportState_ = TransferState::Stopped;
        return;
    }

    // 在锁内提交最终结果。导出的 rowsDone 与 rowsTotal 都取 writtenRows
    // （导出按查询结果集逐行写出，已写出即全部，二者相等）。
    {
        QMutexLocker lock(&mutex_);
        exportResult_ = result;
        exportErrors_ = result.errors;
        exportProgress_.percent = 100;
        exportProgress_.rowsDone = result.writtenRows;
        exportProgress_.rowsTotal = static_cast<qint64>(result.writtenRows);
        exportState_ = result.ok ? TransferState::Completed : TransferState::Failed;
    }
}

// ---------------------------------------------------------------------------
// stop —— 协作式停止（在控制线程执行）
// ---------------------------------------------------------------------------
//
// 停止协议的「发起端」：本函数不强杀线程，只做三件事——
//   ① 置两个原子停止标志为 true（向工作线程广播「请收尾」）；
//   ② 把仍处于 Running 的状态机推进到 Stopping（让轮询者立刻看到「正在停」）；
//   ③ 阻塞等待两条工作线程跑到下一个停止检查点、自行退出。
// 注意它一次性停「两个方向」（导入+导出），这是 IBatchTransfer 契约的简化设计。
// 参数：err 未使用（当前实现无失败路径）。返回：恒为 true。
// 副作用：可能阻塞调用线程直到工作线程收尾（最坏一次不可中断的批处理时长）。
// 线程安全：标志原子写；状态推进在锁内；等待用 future（仅控制线程访问）。
bool BatchTransfer::stop(QString* err) {
    Q_UNUSED(err)

    // 通知两个任务。
    // Signal both tasks.
    importStopRequested_.store(true);
    exportStopRequested_.store(true);

    // 把任何处于 Running 的状态切到 Stopping，让调用方能看到「我们已尝试停止」。
    // 仅切 Running→Stopping：若已是终态（Completed/Stopped/Failed）则不动，避免把
    //   已经结束的传输错误地拉回 Stopping。
    // Transition any Running state to Stopping so callers can see we tried.
    {
        QMutexLocker lock(&mutex_);
        if (importState_ == TransferState::Running) {
            importState_ = TransferState::Stopping;
        }
        if (exportState_ == TransferState::Running) {
            exportState_ = TransferState::Stopping;
        }
    }

    // 等待两个 future 结束。工作线程会检查那个原子标志并提前退出；
    //   若它们其实早已结束，这两次等待就是 no-op（空操作）。
    // Wait for both futures to finish.  The worker threads check the atomic
    // flag and exit early; if they have already finished this is a no-op.
    if (importFuture_.isRunning()) {
        importFuture_.waitForFinished();
    }
    if (exportFuture_.isRunning()) {
        exportFuture_.waitForFinished();
    }

    return true;
}

// ---------------------------------------------------------------------------
// 轮询 getter（加锁快照 / Polling getters, locked snapshots）
// ---------------------------------------------------------------------------
//
// 以下八个 getter 模式完全统一：进函数即 QMutexLocker 抢 mutex_，返回受保护成员的
// 「一份值拷贝」（按值返回，离开作用域自动放锁）。因此调用方任何时刻拿到的都是某一
// 瞬间的一致快照，绝不会读到工作线程写到一半的中间态。
// 线程安全：全部可在任意线程（典型是 GUI 线程）并发调用。复杂度：O(被拷贝对象大小)
//   ——errors/result 含 QList，拷贝是浅拷贝（Qt 隐式共享，写时复制），代价很低。

TransferProgress BatchTransfer::importProgress() const {
    QMutexLocker lock(&mutex_);
    return importProgress_;
}

QList<RowError> BatchTransfer::importErrors() const {
    QMutexLocker lock(&mutex_);
    return importErrors_;
}

ImportResult BatchTransfer::importResult() const {
    QMutexLocker lock(&mutex_);
    return importResult_;
}

TransferProgress BatchTransfer::exportProgress() const {
    QMutexLocker lock(&mutex_);
    return exportProgress_;
}

QList<RowError> BatchTransfer::exportErrors() const {
    QMutexLocker lock(&mutex_);
    return exportErrors_;
}

ExportResult BatchTransfer::exportResult() const {
    QMutexLocker lock(&mutex_);
    return exportResult_;
}

TransferState BatchTransfer::importState() const {
    QMutexLocker lock(&mutex_);
    return importState_;
}

TransferState BatchTransfer::exportState() const {
    QMutexLocker lock(&mutex_);
    return exportState_;
}

// ---------------------------------------------------------------------------
// 工厂函数（Factory）
// ---------------------------------------------------------------------------
//
// createBatchTransfer —— IBatchTransfer.h 中声明的工厂。new 出 BatchTransfer 并以
// unique_ptr<IBatchTransfer> 上抛：调用方独占所有权、只见接口不见实现，析构即自动
// 释放（并触发上面的优雅停机析构）。bridge 引用须在返回对象的整个生命周期内有效。
std::unique_ptr<IBatchTransfer> createBatchTransfer(DataBridge& bridge) {
    return std::make_unique<BatchTransfer>(bridge);
}

}  // namespace dbridge
