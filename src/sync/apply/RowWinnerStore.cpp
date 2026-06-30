#include "RowWinnerStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>

#include "../schema/TableStateStore.h"
#include <climits>

// ============================================================================
// RowWinnerStore.cpp — 胜者表的读写与仲裁规则实现
// ============================================================================
//
// 本文件实现头文件声明的全部方法。最关键的是文件末尾的 beats()：它把头文件描述的
// 「max-element 仲裁规则(G-01)」翻译成一段短小、可逐行阅读的比较逻辑——任何关于
// 「谁是胜者」的疑问都应回到 beats() 求证，其余方法只是它的读写外壳。
//
// 表结构（__sync_row_winner）一行的语义：
//   (table_name, pk_hash)  —— 复合主键，定位「哪张表的哪一行」；
//   winning_origin/rank/origin_seq —— 当前胜者的三元裁决坐标；
//   content_hash           —— 胜者内容指纹（快速比对用）；
//   winning_content        —— 胜者整行 JSON（低 rank DELETE 误删后的恢复素材）；
//   updated_ms             —— 最后更新时间（毫秒时间戳，便于诊断/排错）。
// ============================================================================

namespace dbridge::sync {

// init() —— 启动期自检：探测胜者表是否就绪。
// 做什么：执行 `SELECT COUNT(*) ... WHERE 0`——条件恒假，不读任何数据行，纯粹靠
//         「这条 SQL 能否成功 prepare+exec」来确认表存在且列名正确。
// 为什么这样写：比 `PRAGMA table_info` 更直接，一次往返即可判定表可用性。
// 返回：成功 true；失败（表不存在/结构不符）false，并把驱动错误文本写入 *err。
bool RowWinnerStore::init(QSqlDatabase& db, QString* err) {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM __sync_row_winner WHERE 0"))) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

// get() —— 读出 (table, pkHash) 当前在位胜者。
// 做什么：按复合键查一行，命中则装配成 RowWinner 返回；未命中（无此行 / 查询失败）
//         返回一个 rank=INT_MIN 的哨兵 RowWinner，调用方据此识别「尚无胜者」。
// 参数：db 写连接；table 表名；pkHash_ 由 pkHash() 算出的主键哈希串。
// 返回：在位胜者，或 rank==INT_MIN 的空对象。副作用：一次 SELECT。
RowWinner RowWinnerStore::get(QSqlDatabase& db, const QString& table, const QString& pkHash_) {
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("SELECT winning_origin, winning_rank, winning_origin_seq, "
                       "content_hash, winning_content "
                       "FROM __sync_row_winner "
                       "WHERE table_name = ? AND pk_hash = ?"));
    q.addBindValue(table);
    q.addBindValue(pkHash_);
    // exec 失败或没有结果行（!next）都视为「无胜者」：返回 rank=INT_MIN 哨兵。
    if (!q.exec() || !q.next()) {
        RowWinner empty;
        empty.rank = INT_MIN;
        return empty;
    }
    // 命中：按 SELECT 列序逐字段装配（0=origin,1=rank,2=seq,3=contentHash,4=content）。
    RowWinner w;
    w.origin = q.value(0).toString();
    w.rank = q.value(1).toInt();
    w.originSeq = q.value(2).toLongLong();
    w.contentHash = q.value(3).toByteArray();
    w.winningContent = q.value(4).toString();
    return w;
}

// put() —— 「条件写入」：仅当 winner 真正战胜在位者时才落库，否则放弃（幂等无操作）。
// 做什么：先 get() 读在位者，用 beats() 判胜；判负直接返回 true（什么都不改）。
//         判胜则用 INSERT ... ON CONFLICT DO UPDATE 原子地「插入或覆盖」整条胜者记录。
// 为什么先判再写：避免「低 rank 后到」覆盖「高 rank 先到」——胜负只由 beats() 规则定，
//         与谁先调用 put() 无关，从而保证多节点收敛到同一胜者。
// 参数：winner 为挑战者；其余同 get()。返回：成功（含无操作）true；SQL 失败 false+*err。
// 副作用：最多一次 SELECT + 一次 UPSERT。
bool RowWinnerStore::put(QSqlDatabase& db, const QString& table, const QString& pkHash_,
                         const RowWinner& winner, QString* err) {
    const RowWinner cur = get(db, table, pkHash_);
    if (!beats(winner, cur)) {
        return true;  // 在位者胜出；本次写入是无操作（幂等返回成功）
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("INSERT INTO __sync_row_winner "
                       "(table_name, pk_hash, winning_origin, winning_rank, "
                       " winning_origin_seq, content_hash, winning_content, updated_ms) "
                       "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
                       "ON CONFLICT(table_name, pk_hash) DO UPDATE SET "
                       "  winning_origin     = excluded.winning_origin, "
                       "  winning_rank       = excluded.winning_rank, "
                       "  winning_origin_seq = excluded.winning_origin_seq, "
                       "  content_hash       = excluded.content_hash, "
                       "  winning_content    = excluded.winning_content, "
                       "  updated_ms         = excluded.updated_ms"));
    // 绑定顺序须与 VALUES(?,...) 占位符一一对应。
    q.addBindValue(table);
    q.addBindValue(pkHash_);
    q.addBindValue(winner.origin);
    q.addBindValue(winner.rank);
    q.addBindValue(winner.originSeq);
    q.addBindValue(winner.contentHash);
    q.addBindValue(winner.winningContent);
    q.addBindValue(nowMs);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

// resetAll() —— 清空整张胜者表。基线重置(baseline reset)后调用：旧库状态作废，
// 所有历史胜负归零，后续 apply 从空表重新累积。返回：成功 true；DELETE 失败 false+*err。
bool RowWinnerStore::resetAll(QSqlDatabase& db, QString* err) {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("DELETE FROM __sync_row_winner"))) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

// C-06 fix: clear() —— 删除单行胜者记录。当某行的胜者被一个「合法」DELETE 擦除
// （删除方 rank 不低于在位胜者，删除应当生效）时调用，使胜者表与实际数据保持一致。
// 参数同 get()。返回：成功 true；DELETE 失败 false+*err。副作用：一次 DELETE。
bool RowWinnerStore::clear(QSqlDatabase& db, const QString& table, const QString& pkHash_,
                           QString* err) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("DELETE FROM __sync_row_winner WHERE table_name = ? AND pk_hash = ?"));
    q.addBindValue(table);
    q.addBindValue(pkHash_);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

// pkHash() —— 把主键各列的值规范化后哈希成稳定的键串。
// 为什么委托给 TableStateStore::rowHash()：必须与表状态/基线等其它哈希生产者用「完全
//   相同的规范化编码」（带类型标签，区分整数 1 与字符串 "1"），否则同一行在不同子系统
//   会算出不同 key，导致仲裁、恢复、对账错位（M-01 fix——防止可构造的哈希碰撞）。
// 复杂度：O(列数)。无副作用（纯函数）。
QString RowWinnerStore::pkHash(const QVariantMap& pkValues) {
    // M-01 fix: use the same canonical type-tagged encoding as TableStateStore::rowHash()
    // to prevent constructible collisions between different PK rows.
    // —— 复用 TableStateStore::rowHash() 的规范化类型标签编码，防止不同主键行产生可构造碰撞。
    return QString::fromLatin1(TableStateStore::rowHash(pkValues).toHex());
}

// H-01 fix: putOrRefill() —— put() 的增强版，多接受一种「补写」情形。
// 背景：conflictCb 在冲突回调里会先写一条胜者记录占位，但那时拿不到完整行内容，
//       于是 winningContent 留空。apply 之后本方法带着完整内容再写一次——此时
//       rank/seq 与那条占位记录完全相同（put() 会判为「未胜出」而拒绝），故需特殊放行。
// 放行条件：① 挑战者按 beats() 正常胜出；或 ② rank、seq 都相等且在位者 winningContent
//           为空（即那条待填满的占位记录）。两者之一成立即写入。
// 返回：成功（含无操作）true；UPSERT 失败 false+*err。
bool RowWinnerStore::putOrRefill(QSqlDatabase& db, const QString& table, const QString& pkHash_,
                                 const RowWinner& winner, QString* err) {
    const RowWinner cur = get(db, table, pkHash_);
    // sameRankSeq：识别「同 rank 同 seq、且在位记录内容为空」的占位记录——这正是要被填满的对象。
    // 条件里 cur.rank != INT_MIN 排除「根本没有在位者」的情形（那种由 beats() 直接放行）。
    const bool sameRankSeq = (cur.rank != INT_MIN) && (winner.rank == cur.rank) &&
                             (winner.originSeq == cur.originSeq) && cur.winningContent.isEmpty();
    if (!beats(winner, cur) && !sameRankSeq) {
        return true;  // 在位者胜出且不是待填占位记录；无操作
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("INSERT INTO __sync_row_winner "
                       "(table_name, pk_hash, winning_origin, winning_rank, "
                       " winning_origin_seq, content_hash, winning_content, updated_ms) "
                       "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
                       "ON CONFLICT(table_name, pk_hash) DO UPDATE SET "
                       "  winning_origin     = excluded.winning_origin, "
                       "  winning_rank       = excluded.winning_rank, "
                       "  winning_origin_seq = excluded.winning_origin_seq, "
                       "  content_hash       = excluded.content_hash, "
                       "  winning_content    = excluded.winning_content, "
                       "  updated_ms         = excluded.updated_ms"));
    // 绑定顺序须与 VALUES(?,...) 占位符一一对应。
    q.addBindValue(table);
    q.addBindValue(pkHash_);
    q.addBindValue(winner.origin);
    q.addBindValue(winner.rank);
    q.addBindValue(winner.originSeq);
    q.addBindValue(winner.contentHash);
    q.addBindValue(winner.winningContent);
    q.addBindValue(nowMs);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// private —— 仲裁规则的唯一真理来源
// ---------------------------------------------------------------------------

// beats() —— 判断「挑战者是否战胜在位者」。这是整个冲突仲裁的核心，全库所有「谁赢」
// 决策最终都归到这几行（G-01）。比较是「字典序级联」的：先比 rank，平了比 seq，再平了
// 比 origin——逐级击穿，任一级分出胜负即返回，保证结果对任意到达顺序都唯一确定。
// 纯函数：无副作用、无 I/O，复杂度 O(1)。
bool RowWinnerStore::beats(const RowWinner& challenger, const RowWinner& incumbent) {
    if (incumbent.rank == INT_MIN)
        return true;  // 在位者不存在（哨兵）：挑战者直接占位获胜
    if (challenger.rank > incumbent.rank)
        return true;  // 第 1 级：高 rank 胜（如中心节点 rank 高于边缘节点）
    if (challenger.rank < incumbent.rank)
        return false;  // 低 rank 负
    // 第 2 级：rank 持平，比来源流序号 seq——越大代表越新，越优先。
    if (challenger.originSeq > incumbent.originSeq)
        return true;
    if (challenger.originSeq < incumbent.originSeq)
        return false;
    // H-01 fix: rank == rank and seq == seq — use originId as a stable, deterministic
    // tie-breaker so that applying changesets in any order yields the same final state.
    // Lexicographically larger originId wins (arbitrary but consistent).
    // —— 第 3 级（兜底）：rank、seq 都相等，用 originId 字符串字典序当稳定的确定性裁决：
    //    较大者胜。规则本身无业务含义，纯粹为了「任意顺序应用 changeset 都收敛到同一终态」。
    return challenger.origin > incumbent.origin;
}

}  // namespace dbridge::sync
