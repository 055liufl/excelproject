#pragma once
// ============================================================================
// SyncContext.h — “每个物理库一份”的同步共享上下文 + 进程级登记表（声明）
// ============================================================================
//
// 【这个文件是什么】
//   解决一个核心难题：同一个 SQLite 物理文件，可能被多个上层对象同时引用——
//   一个 SyncEngine、若干 BatchTransfer（ETL）、还有 ComparisonSession（比对）。
//   它们必须共享同一份“同步状态”（同一把前台门控、同一组转发回调、同一个 stream_epoch
//   等），否则各管各的就会破坏“单写者 / 前台互斥”等不变量。
//   SyncContext 就是这份共享状态的载体；SyncContextRegistry 则是“按物理库唯一”地
//   创建、查找、引用计数、销毁这些 SyncContext 的进程级单例登记表。
//
// 【“同一物理库”如何判定（关键不直观处，G-07）】
//   不能用路径字符串判等——同一个文件可经由相对路径、URI、符号链接、不同盘符别名
//   等多种写法表达。登记表用【操作系统文件身份】作 key：POSIX 用 dev+inode，
//   Windows 用 卷序列号+文件索引（见 .cpp 的 canonicalKey()）。这样所有指向同一
//   inode 的别名都会命中同一个 SyncContext，"单写者" 才真正成立。
//   代价：判定身份必须先 stat/open 文件，故【文件必须已存在】（见 .cpp 的 M-04 修复）。
//
// 【引用计数与生命周期】
//   getOrCreate() 命中已有则 refCount++ 并返回共享指针；首次则新建并置 refCount=1。
//   release(key) 使 refCount--，归零即从登记表抹除（SyncContext 随最后一个 shared_ptr
//   析构而销毁）。注意：getExisting() 是“只看不增计数”的旁路查询，其返回指针【不可】
//   用于 release()（见 J-10）。
//
// 【写连接不在这里（务必牢记）】
//   SyncContext 不持有写连接！写连接由 SyncWorker 在自己的 run() 线程内创建并独占
//   （线程亲和性要求，见 SyncWorker.h）。本上下文只存“跨对象共享、与连接无关”的状态。
// ============================================================================
#include "dbridge/Types.h"
#include "dbridge/sync/SyncConfig.h"

#include <QList>
#include <QMutex>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

#include "ForegroundGate.h"
#include "diff/InboundTableGate.h"
#include "profile/ProfileSpec.h"
#include "schema/SchemaCatalog.h"
#include <functional>
#include <memory>
#include <optional>

namespace dbridge::sync {

// SyncContext —— 为“共享同一个物理 SQLite 文件”的所有 SyncEngine / BatchTransfer /
// ComparisonSession 持有的共享状态（按 OS dev+inode 唯一，G-07）。
// 注意：写连接归 SyncWorker（在其 run() 线程创建）所有，【不在】本结构内。
struct SyncContext {
    // 本库的同步配置；由 SyncEngine::initialize() 写入。用 optional 表达“尚未初始化”。
    std::optional<SyncConfig> config;
    // 前台门控：保证同一库同一时刻至多一个前台操作（sync/比对）。见 ForegroundGate.h。
    ForegroundGate gate;
    // 入站表门控：比对“暂存”期间，对涉及表的入站 artifact 延迟（defer）应用，避免暂存被冲掉。
    // 用 shared_ptr 是因为它需要在 SyncWorker 与 ComparisonSession 之间共享同一个实例。
    std::shared_ptr<InboundTableGate> inboundTableGate = std::make_shared<InboundTableGate>();
    // 本库的上下文 UUID；会持久化进 __sync_context_meta（见 ensureContextUuid）以便跨重启双重校验。
    QString contextUuid;

    // 引用计数：归零时本上下文被登记表销毁。仅由 SyncContextRegistry 在持锁下增减。
    int refCount = 0;

    // I-04：worker 初始化完成后由 SyncEngine 写入。BatchTransfer 调用它，把 Excel 导入
    // 路由进 SyncWorker（走写连接 wconn + session 捕获）。同步未激活时为空（nullptr）。
    // 签名：(xlsxPath, options, profile, catalog) -> ImportResult。
    // profile/catalog 是在【调用方线程】上预先拷好的快照再分发，使 worker 永不触碰
    // DataBridge 拥有的 QSqlDatabase 或其可变 catalog（杜绝跨线程访问主库对象）。
    std::function<ImportResult(const QString&, const ImportOptions&,
                               const dbridge::detail::ProfileSpec&,
                               const dbridge::detail::SchemaCatalog&)>
        importFn;

    // 在 SyncWorker 的单写线程上同步执行一个写任务（task 签名见 SyncWorker::submitWriteSync）。
    std::function<bool(const std::function<bool(QSqlDatabase&, QString*)>&, QString*)>
        workerWriteFn;

    // C-05 修复：把一批 RowMutation 路由进 worker 的 CapturedWriteTemplate，使“比对会话的保存”
    // 与“普通本地写”拥有完全一致的语义——被 sqlite session 捕获、写入 changelog、并广播给 peer。
    // 参数 syncTables：需 attach session 录制器的表清单（传 canonicalSyncTables）。
    std::function<bool(const QList<RowMutation>&, const QStringList&, QString*)>
        workerCaptureWriteFn;

    // 在比对门控释放后，请求 worker 立即重扫一次 inbox（把暂存期间被 defer 的入站补上）。
    std::function<void()> rescanFn;

    // L-01 修复：规范化后的同步表清单（配置留空时展开 = 所有用户表）。
    // 由 SyncWorker 在初始化后填充并发布到这里，使所有模块共享同一份表集合。
    QStringList canonicalSyncTables;

    // H-13 修复：worker 当前生效的 stream_epoch。由 SyncWorker 在 init 后发布到此，
    // 使 ComparisonSession 工厂能用【正确的 epoch】读取本地 __sync_table_state——
    // 否则用占位值 0 会让 tableDiffs() 把状态读成“未找到”。
    qint64 streamEpoch = 0;
};

// SyncContextRegistry —— 进程级单例：按物理库唯一地登记/查找/计数/销毁 SyncContext。
class SyncContextRegistry {
   public:
    // 取得进程唯一的登记表实例（Meyers 单例，首次调用时构造，线程安全）。
    static SyncContextRegistry& instance();

    // 打开或取得 path 所指 SQLite 文件对应的上下文（命中则 refCount++，首次则新建置 1）。
    // 成功时把该库的“dev+inode 规范键”写入 *canonicalKeyOut，调用方须用它来 release()。
    // 失败（如文件不存在/无法解析身份）返回 nullptr 并写 *err。
    std::shared_ptr<SyncContext> getOrCreate(const QString& sqlitePath, QString* canonicalKeyOut,
                                             QString* err = nullptr);

    // J-10：只查询 path 已有的上下文——【不】增引用计数、【不】新建条目。
    // 无登记则返回 nullptr。经本方法取得的指针【不可】调用 release()（它没占计数）。
    // 返回的是 shared_ptr 拷贝：共享所有权可在调用方“查看期间”保活对象，但不动内部 refCount。
    std::shared_ptr<SyncContext> getExisting(const QString& sqlitePath);

    // 减引用：refCount 归零即销毁该上下文（从登记表抹除）。key 须是 getOrCreate 给出的规范键。
    void release(const QString& canonicalKey);

    // H-01 修复：把 contextUuid 持久化进 __sync_context_meta，使同一物理库始终关联同一 UUID
    //   （即便跨进程重启）。行为：
    //   - 库中尚无 UUID    → 把 *uuid 写入库；返回 true。
    //   - 库中 UUID == *uuid → 无操作；返回 true。
    //   - 库中是另一个 UUID  → 回读：把 *uuid 置为库中已存值；返回 true。
    //     （这是常见的“重启复用”场景；调用方据此更新 ctx->contextUuid。）
    //   - 仅当 DB 查询本身失败时返回 false（并写 *err）。
    static bool ensureContextUuid(QSqlDatabase& db, QString* uuid, QString* err);

   private:
    SyncContextRegistry() = default;  // 私有构造：强制经 instance() 取唯一实例。

    // 计算规范键：POSIX = dev+inode；Windows = 卷序列号+文件索引。失败写 *err 返回空串。
    static QString canonicalKey(const QString& path, QString* err = nullptr);

    QMutex mutex_;  // 保护 registry_ 的并发访问（getOrCreate/getExisting/release 均在锁内）。
    QHash<QString, std::shared_ptr<SyncContext>> registry_;  // 规范键 → 共享上下文 的映射表。
};

}  // namespace dbridge::sync
