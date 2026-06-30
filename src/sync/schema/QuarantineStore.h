#pragma once
#include <QByteArray>
#include <QList>
#include <QPair>
#include <QSqlDatabase>
#include <QString>

// ============================================================================
// QuarantineStore.h — 「隔离区」：暂存暂时无法应用的变更载荷，待条件满足再重放
// ============================================================================
//
// 【这个文件是什么 / 解决什么问题】
//   同步是分布式异步的：变更包（payload）可能「先于」其依赖到达。最典型的情形是
//   schema 版本错位——某个变更包是在「更新的表结构（payload_schema_ver 更高）」下
//   产生的，而本节点的基线还停留在旧结构上。此时直接套用会因结构不符而失败/损坏。
//   QuarantineStore 把这类「现在还不能应用」的载荷原样存进元数据表 __sync_quarantine
//   「隔离」起来，等本地基线追上（currentSchemaVer 升高）后，再由上层取出「重放」。
//   它既是正确性兜底（不丢弃、不误用未来版本的数据），也是排查与重放的落点。
//
// 【与「损坏 artifact 隔离」的关系】
//   注：本类聚焦「schemaVer 超前」这一类可恢复的隔离（存 DB、可重放）；而彻底损坏、
//   校验失败的 inbox 文件由 InboxLedger 标 Corrupt + 文件层归档处理，二者职责不同。
//
// 【典型时序】
//   收到 payload(schemaVer=N) 但本地仍是 schemaVer=M (M<N)
//      → quarantine(...)  入库等待
//   稍后本地基线升级到 schemaVer>=N
//      → drainReady(currentSchemaVer)  取出所有 payload_schema_ver <= 当前版本的行
//      → 上层逐条重放成功后 markReplayed(id) 删除该行
//   未被取出的（schemaVer 仍超前的）继续留在隔离区，等下一次基线推进。
//
// 【为什么 drainReady 不在内部删除、而要分两步（取 + markReplayed）】
//   见 H-01 fix：取出与删除解耦，保证「只有真正重放成功的行才被删除」。若在取出时
//   就删，一旦重放过程中途失败，这些载荷就永久丢失了。分两步即「至少一次」语义。
//
// 【协作者】
//   · SyncDDL —— 建 __sync_quarantine 表（init() 只校验可访问）；
//   · 上层 apply/replay 逻辑 —— 在基线推进后驱动 drainReady → 重放 → markReplayed；
//   · PayloadCodec —— 真正解码/应用被存的 payload 字节（本类只负责字节的存与取）。
//
// 【线程模型】无可变成员（纯方法类），状态全在传入的 QSqlDatabase 中。
// ============================================================================

namespace dbridge::sync {

// QuarantineStore —— 暂存「schemaVer 超前于本地基线」的载荷；drainReady() 返回此刻
// 已经可以重放（版本不再超前）的载荷集合。
class QuarantineStore {
   public:
    // init —— 启动期自检：确认 __sync_quarantine 表可访问（表由 SyncDDL 创建，本类不建表）。
    // 返回 true 成功；失败时（若 err 非空）填入 SQL 错误文本。
    bool init(QSqlDatabase& db, QString* err);

    // quarantine —— 把一条「现在不能应用」的载荷原样持久化进隔离区。
    // 【参数】origin/seq/epoch 标识这条变更的来源、序号与流纪元（便于排查与定序）；
    //   schemaVer 是该载荷所基于的表结构版本（重放门槛：本地需 >= 它才能应用）；
    //   payload 是原始字节（连同创建时刻 created_ms 一并入库）。
    // 【副作用】向 __sync_quarantine 插入一行；自增主键 id 同时充当「到达顺序」。
    bool quarantine(QSqlDatabase& db, const QString& origin, qint64 seq, qint64 epoch,
                    qint64 schemaVer, const QByteArray& payload, QString* err);

    // H-01 fix: return (id, payload) pairs for all rows where payload_schema_ver <=
    // currentSchemaVer, ordered by id ASC (arrival order).  Rows are NOT deleted here;
    // call markReplayed(id) after each successful replay.
    // 译：H-01 fix —— 返回所有 payload_schema_ver <= currentSchemaVer 的 (id, payload) 对，
    //   按 id 升序（即到达顺序）排列；本方法「不删除」任何行——必须在每条成功重放之后
    //   显式调用 markReplayed(id) 才删除。这样保证“仅删已成功重放者”，避免重放中途失败导致丢数据。
    // 【返回】可重放的载荷列表（可能为空）；按 id ASC 保证跨 origin 的重放仍遵循入库先后。
    QList<QPair<qint64, QByteArray>> drainReady(QSqlDatabase& db, qint64 currentSchemaVer);

    // H-01 fix: delete a successfully replayed quarantine row.
    // 译：H-01 fix —— 删除一条已成功重放的隔离行（按主键 id 删）。配合 drainReady 实现两步法。
    // 返回 void：删除失败（极少见）不阻断流程，最坏情况是下次重放会幂等地重复一次。
    void markReplayed(QSqlDatabase& db, qint64 id);
};

}  // namespace dbridge::sync
