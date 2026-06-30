#include "FrozenManifest.h"

#include <QSqlError>
#include <QSqlQuery>

// ============================================================================
// FrozenManifest.cpp — 冻结清单持久化的实现
// ============================================================================
//
// 【本文件做什么】
//   实现 FrozenManifest 的四个方法（init/save/load/remove），即冻结清单与本地控制表
//   __sync_frozen_manifest 之间的“存 / 取 / 删”三类基本操作（init 是占位）。
//   所有方法都是“连接进、操作完即走”的无状态实现，不持有任何成员、不自开事务。
//
// 【职责边界——本文件“不做”的事，务必先厘清】
//   · 不计算指纹（fingerprint）：行内容指纹 = 该行紧凑 JSON 的 SHA-1，由上游
//     ChunkStreamer::entryToFrozen() 算好后放进 FrozenEntry，本文件只原样存取。
//   · 不计算 pkHash：pkHash = SHA-1("table\x1fprimaryKey") 的十六进制，同样在 ChunkStreamer
//     里算好。
//   · 不做漂移判定（drift）：本文件只把“冻结那一刻的指纹”稳妥地存下来；判定“某行是否在
//     冻结后又被改动”是调用方拿当前实时指纹与这里存下的冻结指纹比对，不一致才报
//     W_SYNC_PUSH_ROW_DRIFTED。换言之，本文件提供的是漂移检测的“基准线”，而非检测本身。
//   · 不建表：建表 DDL 集中在 SyncDDL；见 init()。
//
// 【列序约定（贯穿 save/load，必须一一对齐）】
//   表列顺序固定为：
//     (push_id, chunk_seq, table_name, pk_hash, primary_key, record_kind, topo_index, fingerprint)
//   save() 的 8 个占位符、load() 的 SELECT 列清单与 q.value(idx) 取值，全部依此顺序。
//   修改任一处都必须同步另一处，否则会出现“字段错位”的隐蔽 bug。
//
// 注释风格参照 Errors.h / SyncTypes.h；保留并翻译既有英文注释。
// ============================================================================

namespace dbridge::sync {

// ── init —— 初始化占位 ──────────────────────────────────────────────────────
// 见头文件说明：物理表由 SyncDDL 统一创建，本类无需也不应重复建表。
bool FrozenManifest::init(QSqlDatabase& db, QString* err) {
    // Table is created by SyncDDL::allCreateStatements(); nothing extra needed here.
    //   （该表由 SyncDDL::allCreateStatements() 负责创建；此处无需任何额外动作。）
    Q_UNUSED(db);  // 形参保留以维持接口对称（与其它 *Store 一致），当前用不到 → 显式抑制告警
    Q_UNUSED(err);  // 同上：本方法不会产生错误，err 形参仅为签名一致而存在
    return true;    // 恒成功
}

// ── save —— 把一个分片的冻结条目逐条写入控制表 ───────────────────────────────
// 调用时机：在“冻结读快照尚未释放”时（C16），趁本地数据仍与冻结指纹一致，把清单落库。
// 幂等性来自 INSERT OR REPLACE（同键重复写则覆盖，不报主键冲突）。
bool FrozenManifest::save(QSqlDatabase& db, const QString& pushId, int chunkSeq,
                          const QList<FrozenEntry>& entries, QString* err) {
    QSqlQuery q(db);
    // 预编译一次、循环复用：8 个 '?' 占位符严格对应下方 addBindValue 的 8 次绑定顺序。
    // INSERT OR REPLACE：若 (push_id,chunk_seq,table_name,pk_hash) 主键已存在则整行替换，
    //   使“重发 / 续传重存”这类重复保存天然幂等、不会因主键冲突中断。
    q.prepare(
        QStringLiteral("INSERT OR REPLACE INTO __sync_frozen_manifest "
                       "(push_id, chunk_seq, table_name, pk_hash, primary_key, record_kind, "
                       "topo_index, fingerprint) "
                       "VALUES (?,?,?,?,?,?,?,?)"));

    // 逐条绑定并执行。注意：每次循环都重新 addBindValue 全部 8 个值——QSqlQuery 会按
    // 调用顺序把绑定值压入位置参数，exec() 后下一轮再压入即覆盖上一轮，故无需手动清空。
    for (const auto& e : entries) {
        q.addBindValue(pushId);  // → push_id     ：本批次 id（同一 push 的所有行相同）
        q.addBindValue(chunkSeq);  // → chunk_seq   ：本分片序号
        q.addBindValue(e.table);   // → table_name  ：行所属表
        q.addBindValue(e.pkHash);  // → pk_hash     ：主键哈希（定长键，做主键/比对用）
        q.addBindValue(e.primaryKey);  // → primary_key ：业务可读主键
        q.addBindValue(e.recordKind);  // → record_kind ："selected" | "dependency"
        q.addBindValue(e.topoIndex);  // → topo_index  ：拓扑序（小者先应用，父先子后）
        // → fingerprint：行内容指纹（二进制）。该列 DDL 为 BLOB NOT NULL，因此 e.fingerprint
        //   必须是非空字节；上游 ChunkStreamer 已确保对每行算出非空 SHA-1（绑定一个
        //   默认构造的空 QByteArray() 会被当成 SQL NULL，从而违反 NOT NULL 约束）。
        q.addBindValue(e.fingerprint);
        if (!q.exec()) {
            // 任一条插入失败立即返回 false。常见原因：foreign_keys=ON 时父表
            // __sync_push_progress 尚无对应 push_id 行（外键约束失败）——这正是调用方按
            // H-02 fix 必须“先插 push_progress、再 save”的原因；其它如连接异常等亦在此被捕获。
            // 注意：本方法不自开事务，已成功插入的前序条目不会回滚（事务边界由调用方掌控）。
            if (err)
                *err = q.lastError().text();  // 透传底层驱动错误文本，便于上层诊断
            return false;
        }
    }
    return true;  // 全部条目写入成功
}

// ── load —— 取回某个 push/分片的冻结清单（按拓扑序）──────────────────────────
// 续传（resumption）用：中断后据此把冻结状态原样恢复，免去重新解析行/重算指纹。
QList<FrozenEntry> FrozenManifest::load(QSqlDatabase& db, const QString& pushId, int chunkSeq) {
    QList<FrozenEntry> result;
    QSqlQuery q(db);
    // SELECT 列顺序须与下方 q.value(0..5) 的取值下标严格对应。
    // ORDER BY topo_index：取回即“父先子后”有序，调用方可直接顺序 apply / 续推，无需再排序。
    q.prepare(QStringLiteral(
        "SELECT table_name, pk_hash, primary_key, record_kind, topo_index, fingerprint "
        "FROM __sync_frozen_manifest WHERE push_id=? AND chunk_seq=? "
        "ORDER BY topo_index"));
    q.addBindValue(pushId);    // WHERE push_id=?
    q.addBindValue(chunkSeq);  // WHERE chunk_seq=?
    if (!q.exec())
        return result;  // 查询失败：返回空列表（续传语义下等价于“该分片无需恢复”，不写错误）

    // 逐行重建 FrozenEntry：下标 0..5 对应 SELECT 列清单的次序。
    while (q.next()) {
        FrozenEntry e;
        e.table = q.value(0).toString();       // table_name  → table
        e.pkHash = q.value(1).toString();      // pk_hash     → pkHash
        e.primaryKey = q.value(2).toString();  // primary_key → primaryKey
        e.recordKind = q.value(3).toString();  // record_kind → recordKind
        e.topoIndex = q.value(4).toInt();      // topo_index  → topoIndex
        e.fingerprint = q.value(5).toByteArray();  // fingerprint → fingerprint（原始二进制还原）
        result.append(e);
    }
    return result;  // 已按 topoIndex 升序；无匹配时为空列表
}

// ── remove —— 清理一次推送的全部冻结条目（不限分片）─────────────────────────
// 何时调用：该 pushId 的所有分片均已被 ACK、不再需要续传时，回收冻结记录避免控制表膨胀。
bool FrozenManifest::remove(QSqlDatabase& db, const QString& pushId, QString* err) {
    QSqlQuery q(db);
    // 仅以 push_id 为条件，一次性删掉该批次跨所有 chunk_seq 的全部冻结行。
    q.prepare(QStringLiteral("DELETE FROM __sync_frozen_manifest WHERE push_id=?"));
    q.addBindValue(pushId);
    if (!q.exec()) {
        // 删除执行失败（连接异常等）。注意：没有匹配行并不算失败——SQLite 的 DELETE 命中 0 行
        // 仍返回成功，故本方法对“重复清理 / 清理不存在的 push”是幂等的。
        if (err)
            *err = q.lastError().text();  // 透传驱动错误文本
        return false;
    }
    return true;
}

}  // namespace dbridge::sync
