#pragma once
#include "dbridge/sync/IComparisonSession.h"

#include <QHash>
#include <QList>
#include <QSqlDatabase>
#include <QString>

#include "../SyncContext.h"
#include "../apply/UpsertExecutor.h"
#include "../schema/TableStateStore.h"
#include "DiffEngine.h"
#include "InboundTableGate.h"
#include "StagingBuffer.h"

// ============================================================================
// ComparisonSession.h — 交互式比对会话的具体实现（IComparisonSession 的落地类）
// ============================================================================
//
// 【这个类是什么 / 在 diff 链路中的位置】
//   diff 三件套（会话 + 暂存 + 门控）里的「会话」，是 IComparisonSession 公共接口的实现。
//   它把四个协作组件串成一条完整的「比对→交互合并→写回」流水线：
//     · DiffEngine       —— 算两级差异（表级/行级），纯计算（见 DiffEngine.h）；
//     · StagingBuffer    —— 暂存「采用对端」的逐行决策（内存，见 StagingBuffer.h）；
//     · InboundTableGate —— 比对期间门控被比对表的 inbox 应用（见 InboundTableGate.h）；
//     · UpsertExecutor / SyncContext 的 worker 写函数 —— 把暂存决策落库。
//
// 【一次会话的生命周期（与 IComparisonSession 文件头呼应）】
//   构造（注入依赖） → initialize（钉读快照+算表级差异+开门控） → 反复
//   tableDiffs/rowDiffs 查看、stageRow/stageCell/acceptRemote/unstage 调整 →
//   save（stale 检测通过则一次性写回，经 worker 捕获式写）或 discard（全部放弃） →
//   析构。save/discard 都会释放读事务与门控，故会话应尽快收尾、勿长期持有。
//
// 【关键不直观处（实现里逐一注释）】
//   · 读事务钉快照（H-7）：initialize 用 rconn_.transaction() 开 BEGIN DEFERRED，把一个
//     一致的本地读快照固定住，使整场比对看到的本地数据不随后台写入漂移。
//   · stale 检测：initialize 时记下 PRAGMA data_version（pinnedDataVersion_），save 前
//     再读一次，不一致即说明本地库被外部改过 → 拒绝写回（E_SYNC_STAGE_STALE）。
//   · 门控开闭：initialize 开、save/discard 关；释放后触发 rescanFn 让被推迟的 inbox 补上。
//
// 【关于 local / remote】local=本地库（rconn_ 上 SELECT 的真值）；remote=对端快照
//   （存在 remoteData_ 里的只读行集合）。合并方向永远是把 remote 合并进 local。
//
// 【线程】非线程安全，约定单线程使用；实际写库经 SyncContext 的 worker 写函数串行化到
//   SyncWorker 的单写线程，本类自身不直接在 wconn_ 上写。
// ============================================================================

namespace dbridge::sync {

// RemoteTableData —— 一张表的对端侧数据缓存（initialize 时从快照转存，比对期内只读）。
//   meta：表级三元组（schemaFp/checksum/rowCount），供表级快速判等。
//   rows：行级比对/暂存所需的对端行集合（可能为空 = 惰性，未随快照带来）。
struct RemoteTableData {
    DiffEngine::RemoteMeta meta;  // 对端该表的表级元数据三元组
    QList<QVariantMap> rows;  // 对端该表的行集合（列名→值）；空表示未提供/惰性
};

class ComparisonSession : public IComparisonSession {
   public:
    // 构造：注入全部协作依赖（均为引用，生命周期由外层 OwningComparisonSession/工厂保证）。
    //   rconn —— 本地只读连接，用于 diff 查询并在其上钉读事务快照；
    //   wconn —— 历史遗留的写连接引用，本实现并不直接用它写库（写经 worker 函数），
    //            工厂传入时甚至复用 rconn 占位（见 .cpp 工厂注释）；
    //   ts    —— 本地表状态存储（读 __sync_table_state 供表级比对）；
    //   diff  —— 差异计算引擎；gate —— inbox 门控；upsert —— UPSERT 执行器（回退路径用）；
    //   streamEpoch —— 流纪元，定位本地表状态的版本（须用 worker 发布的真实值，见 H-13）；
    //   context —— 同步上下文（提供 worker 写函数 / rescanFn / 规范表集 等），可为空（测试桩）。
    ComparisonSession(QSqlDatabase& rconn, QSqlDatabase& wconn, TableStateStore& ts,
                      DiffEngine& diff, InboundTableGate& gate, UpsertExecutor& upsert,
                      qint64 streamEpoch, std::shared_ptr<SyncContext> context = nullptr);

    // Initialize: compute diffs against remote, open gate.
    // remoteMetas: table->RemoteMeta. remoteRows: table->rows.
    // 内部初始化重载（接收已拆解的内部类型）：钉读快照、缓存对端数据、算表级差异、开门控。
    //   参数：tables 参与比对的表名；remoteMetas 表→对端表级元数据；remoteRows 表→对端行集。
    //   返回：成功 true；开读事务或读 data_version 失败则 false。被下面的公共重载调用。
    bool initialize(const QStringList& tables,
                    const QHash<QString, DiffEngine::RemoteMeta>& remoteMetas,
                    const QHash<QString, QList<QVariantMap>>& remoteRows, QString* err);

    // C-10 fix: IComparisonSession::initialize override accepting public API type.
    // C-10 修复：实现公共接口的 initialize——接收对外的 RemoteTableSnapshot 列表，
    //   把它拆成内部三件套（tables/remoteMetas/remoteRows）后转调上面的内部重载。
    bool initialize(const QList<RemoteTableSnapshot>& remoteSnapshots,
                    QString* err = nullptr) override;

    // IComparisonSession implementation
    // —— 以下为 IComparisonSession 公共接口的实现；各方法语义见 IComparisonSession.h，
    //    具体实现细节见 ComparisonSession.cpp 内对应函数的逐行注释。
    QList<TableDiff> tableDiffs() const override;
    QList<RowDiff> rowDiffs(const QString& table, int offset, int limit) const override;
    bool stageRow(const QString& table, const QString& pk) override;
    bool stageTable(const QString& table) override;
    bool unstage(const QString& table, const QString& pk) override;
    bool acceptLocal(const QString& table, const QString& pk) override;
    bool acceptRemote(const QString& table, const QString& pk) override;
    bool stageCell(const QString& table, const QString& pk, const QString& column,
                   const QVariant& value) override;
    QList<RowDiff> fetchRemoteRows(const QString& table, const QString& keysetPageToken,
                                   int pageSize, const QString& snapshotId) const override;
    bool save(QString* err = nullptr) override;
    void discard() override;

   private:
    // stale（过期）检测：当前 data_version 与 initialize 时钉住的是否一致；不一致返回 false。
    bool checkStale(QString* err);
    // 读 PRAGMA data_version（本地库被改一次该值就 +1）；失败返回 -1 并写 err。
    qint64 readDataVersion(QString* err) const;
    // 在缓存的对端行集合里按主键找一行（找不到返回空 map）。
    QVariantMap findRemoteRow(const QString& table, const QString& pk) const;
    // 在本地库里按主键 SELECT 一行（找不到返回空 map）。
    QVariantMap findLocalRow(const QString& table, const QString& pk) const;
    // 取某表主键列名（经 PRAGMA table_info，结果进 pkColCache_ 缓存）。
    QString getPkColumn(const QString& table) const;

    QSqlDatabase& rconn_;  // 本地只读连接：所有 diff 查询 + 在其上钉读事务快照
    QSqlDatabase& wconn_;  // 写连接引用（历史遗留；本实现不直接用它写，写经 worker 函数）
    TableStateStore& ts_;     // 本地表状态存储（表级比对读 __sync_table_state）
    DiffEngine& diff_;        // 差异计算引擎（表级/行级）
    InboundTableGate& gate_;  // inbox 门控（比对期间冻结被比对表的入站应用）
    UpsertExecutor& upsert_;  // UPSERT 执行器（仅回退落库路径会用到）
    StagingBuffer staging_;  // 暂存区（本会话私有，攒「采用对端」的逐行决策）
    qint64 streamEpoch_;     // 流纪元（定位本地表状态版本）
    std::shared_ptr<SyncContext> context_;  // 同步上下文（worker 写函数 / rescanFn / 规范表集）
    qint64 pinnedDataVersion_ = 0;  // initialize 时钉住的 data_version；save 前比对它做 stale 检测
    bool readTxnActive_ =
        false;  // H-7: tracks whether BEGIN DEFERRED is open on rconn_
                // H-7：标记 rconn_ 上的 BEGIN DEFERRED 读事务是否仍打开（避免重复回滚）
    QList<TableDiff> diffs_;  // initialize 时算好的表级差异结果缓存（tableDiffs() 直接返回）
    QHash<QString, RemoteTableData> remoteData_;  // 表名→对端数据缓存（meta + rows），比对期内只读
    mutable QHash<QString, QString>
        pkColCache_;  // 表名→主键列名 缓存；mutable 以便 const 方法内填充
};

}  // namespace dbridge::sync
