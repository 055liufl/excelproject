#include "ConsistencyCache.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

// ============================================================================
// ConsistencyCache.cpp — 一致性剪枝缓存的实现（内存主存 + 可选落盘镜像）
// ============================================================================
//
// 【实现概览】
//   本类的「真相之源」始终是内存哈希 memCache_：所有查询（isConsistent）只读内存，
//   所有写入（盖戳/失效）先改内存、再视 durable_ 决定是否同步到落盘表。这样设计的好处：
//     · 查询路径零 SQL、零 I/O，被 FkClosureBuilder 在补闭包内环里高频调用也不拖慢；
//     · 落盘只是为了「跨进程重启保留知识」，失败了也不影响本次会话的正确性
//       （内存仍然对、剪枝仍然有效，最坏只是下次冷启动少几条已知一致记录）。
//
// 【职责回顾】见同名头文件文件头：缓存「(表,主键) → 中心指纹」，命中即剪掉冗余父行。
// 【协作者】FkClosureBuilder（读，isConsistent 剪枝）、SyncWorker/BaselineManager
//          （写，stampFromAuthoritative 盖戳 / invalidateTable 失效）、SyncDDL.h（真实建表）。
// ============================================================================

namespace dbridge::sync {

// ── kCreateTable —— 本类自带的「兜底」建表 DDL ──────────────────────────────
//   ⚠ 注意：这份 DDL **不含 updated_ms 列**，与生产中真正生效的权威 DDL
//   （src/sync/SyncDDL.h 第 81 行，含 `updated_ms INTEGER NOT NULL`）并不相同。
//   正常流程下表早已由 SyncDDL.h 建好，本 DDL 因 `IF NOT EXISTS` 而成为空操作，
//   仅在「表恰好尚不存在」时作为最低限度的兜底。这也正是 persistStamp 的 INSERT
//   必须显式写出 updated_ms 的原因（见下方 M-01 fix）：它要匹配的是 SyncDDL.h 建出的
//   真实表结构，而非这份兜底 DDL。两份 DDL 的差异是历史遗留，理解时以 SyncDDL.h 为准。
static constexpr const char* kCreateTable =
    "CREATE TABLE IF NOT EXISTS __sync_consistency_cache ("
    "  table_name      TEXT NOT NULL,"
    "  primary_key     TEXT NOT NULL,"
    "  center_fingerprint BLOB,"
    "  PRIMARY KEY (table_name, primary_key)"
    ")";

// ── init —— 初始化：重置内存，可选建表 + 载入落盘数据 ────────────────────────
// 见头文件声明处的完整契约。流程：记下模式 → 清内存 → 非持久化直接成功 →
// 持久化则建表（兜底）→ loadFromDb 把落盘条目灌回内存。
bool ConsistencyCache::init(QSqlDatabase& db, bool durable, QString* err) {
    durable_ = durable;
    memCache_.clear();  // 无论何种模式，每次 init 都从干净的内存开始

    // 纯内存模式：不碰数据库，直接成功。此后缓存只活在进程内存里，退出即丢。
    if (!durable_)
        return true;

    // 持久化模式：先确保表存在（兜底 DDL；通常 SyncDDL.h 已建好，这里多半是空操作）。
    QSqlQuery q(db);
    if (!q.exec(QLatin1String(kCreateTable))) {
        if (err)
            *err = q.lastError().text();
        return false;
    }

    // 再把落盘的历史条目载入内存，恢复上次会话积累的「对端已有」知识。
    return loadFromDb(db, err);
}

// ── loadFromDb —— 把持久化表全部条目灌回内存哈希 ────────────────────────────
// 仅由 init 在 durable 模式下调用。逐行读 (表名, 主键, 指纹) 写入 memCache_。
bool ConsistencyCache::loadFromDb(QSqlDatabase& db, QString* err) {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT table_name, primary_key, center_fingerprint "
                               "FROM __sync_consistency_cache"))) {
        if (err)
            *err = q.lastError().text();
        return false;
    }

    // 逐行回灌：第 0/1/2 列分别是 表名 / 主键 / 中心指纹（BLOB → QByteArray）。
    // memCache_[tbl][pk] 的下标访问会按需自动创建外层/内层哈希桶（QHash 的 operator[] 语义）。
    while (q.next()) {
        const QString tbl = q.value(0).toString();
        const QString pk = q.value(1).toString();
        const QByteArray fp = q.value(2).toByteArray();
        memCache_[tbl][pk] = fp;
    }
    return true;
}

// ── isConsistent —— 命中判定（FkClosureBuilder 剪枝的依据） ──────────────────
// 见头文件契约。要点：缺键 / 指纹不等 都返回 false，唯有「键在且指纹相等」才算一致。
bool ConsistencyCache::isConsistent(const QString& table, const QString& pk,
                                    const QByteArray& localFp) const {
    // 第一级：表桶不存在 → 这张表从没盖过任何戳 → 一定不一致。
    auto tblIt = memCache_.constFind(table);
    if (tblIt == memCache_.constEnd())
        return false;
    // 第二级：表桶里没有这个主键 → 这一行从没被确认过 → 不一致。
    auto pkIt = tblIt->constFind(pk);
    if (pkIt == tblIt->constEnd())
        return false;
    // 关键点：键存在还不够，必须「缓存的中心指纹 == 本地此刻的指纹」才算一致。
    //   若本地这行在盖戳之后又被改过，localFp 会与 *pkIt 不同 → 返回 false → 仍要推送。
    return *pkIt == localFp;
}

// ── stampFromAuthoritative —— 盖戳：登记某行已被中心确认为某指纹 ─────────────
// 见头文件契约。先无条件改内存（这一步立即让 isConsistent 生效），再视模式决定是否落盘。
void ConsistencyCache::stampFromAuthoritative(QSqlDatabase& db, const QString& table,
                                              const QString& pk, const QByteArray& centerFp) {
    memCache_[table][pk] = centerFp;  // 内存戳：即时生效，是剪枝正确性的根本保障
    // 落盘只是为了跨会话保留；故意不检查 persistStamp 的返回值——
    // 落盘失败不应影响本次会话的剪枝（内存已对），最坏只是下次冷启动少这条记录。
    if (durable_)
        persistStamp(db, table, pk, centerFp);
}

// ── persistStamp —— 把单条盖戳落盘（INSERT OR REPLACE） ─────────────────────
// 仅由 stampFromAuthoritative 在 durable 模式下调用。
bool ConsistencyCache::persistStamp(QSqlDatabase& db, const QString& table, const QString& pk,
                                    const QByteArray& fp) {
    // M-01 fix: include updated_ms (NOT NULL in DDL) to prevent INSERT failure.
    //   （M-01 修复：INSERT 列表必须带上 updated_ms。原因：生产环境真正生效的建表 DDL
    //    在 SyncDDL.h，那里 updated_ms 是 NOT NULL；若 INSERT 不提供该列会触发约束失败、
    //    导致整条盖戳落盘失败。本文件顶部的兜底 kCreateTable 虽未声明此列，但绝不能以它
    //    为准——必须以 SyncDDL.h 的真实表结构为准，故此处显式写入 updated_ms。）
    QSqlQuery q(db);
    q.prepare(
        // INSERT OR REPLACE：主键 (table_name, primary_key) 冲突时整行替换，
        // 实现「同一行重复盖戳就刷新指纹 + 时间戳」的幂等语义。
        QStringLiteral("INSERT OR REPLACE INTO __sync_consistency_cache"
                       " (table_name, primary_key, center_fingerprint, updated_ms)"
                       " VALUES (?, ?, ?, ?)"));
    q.addBindValue(table);                                // ? #1 → table_name
    q.addBindValue(pk);                                   // ? #2 → primary_key
    q.addBindValue(fp);                                   // ? #3 → center_fingerprint（BLOB）
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());  // ? #4 → updated_ms（仅运维/诊断用）
    return q.exec();
}

// ── invalidateTable —— 整表失效（入站 apply 后） ────────────────────────────
// 见头文件契约。先清内存桶（决定性的一步），durable 时再删落盘镜像。
void ConsistencyCache::invalidateTable(QSqlDatabase& db, const QString& table) {
    memCache_.remove(table);  // 整桶移除：该表所有 (pk→指纹) 一并作废，杜绝误剪
    // 同 stampFromAuthoritative：不检查 deleteTable 返回值——内存已清是关键，
    // 落盘删除失败最坏只是镜像滞留，下次 init→loadFromDb 会被新数据覆盖修正。
    if (durable_)
        deleteTable(db, table);
}

// ── deleteTable —— 把某表全部条目从持久化表删除 ─────────────────────────────
// 仅由 invalidateTable 在 durable 模式下调用。
bool ConsistencyCache::deleteTable(QSqlDatabase& db, const QString& table) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("DELETE FROM __sync_consistency_cache WHERE table_name = ?"));
    q.addBindValue(table);  // ? #1 → 待清空的表名（其全部行被删除）
    return q.exec();
}

}  // namespace dbridge::sync
