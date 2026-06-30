#include "OutboundAckStore.h"

// ============================================================================
// OutboundAckStore.cpp — __sync_outbound_ack 表 DAO 的实现（SQL 落地）
// ============================================================================
//
// 【本文件职责】把 OutboundAckStore.h 中声明的语义化方法翻译成具体 SQL 并执行。
//   每个方法都遵循同一套模板：构造 QSqlQuery → prepare 参数化 SQL（防注入、可缓存）
//   → addBindValue 顺序绑定占位符 → exec/next → 取值或填错误出参。
//   关键不直观处（MAX 推进的 UPSERT、MIN 裁剪、LEFT JOIN 重发下界、COALESCE/NULL 兜底）
//   在各处逐行解释。表结构与各列含义见头文件文件头与 SyncDDL.h。
//
// 【SQL 习惯约定（贯穿全文件）】
//   · '?' 占位符按 addBindValue 调用顺序对位绑定；顺序一旦错位即绑错列，务必对照核查。
//   · 读路径统一用「!exec() || !next() → 返回 -1」+「value(0).isNull() → 返回 -1」双重
//     兜底：前者覆盖「SQL 失败 / 无结果行」，后者覆盖「聚合函数(MIN/MAX)对空集返回 NULL」。
//     二者都归一化为 -1（「无水位 / 尚未发送 / 尚未确认」的统一约定值）。
//   · 写路径统一用「!exec() → 填 *err 并返回 false」上报数据库错误文本。
// ============================================================================

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace dbridge::sync {

// ── init —— 表健在性探针（详见头文件：零代价确认表可读）──────────────────────
bool OutboundAckStore::init(QSqlDatabase& db, QString* err) {
    QSqlQuery q(db);
    // "WHERE 0" 是一个恒为假的过滤：数据库仍会解析表/列、确认其存在并可访问，
    // 但不会真正扫描或返回任何行——以最小代价探活 __sync_outbound_ack 是否就绪。
    // 表的创建由 SyncDDL（allCreateStatements）负责，此处只验证、不建表。
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM __sync_outbound_ack WHERE 0"))) {
        if (err)
            *err = q.lastError().text();  // 表缺失/驱动异常时上报数据库原始错误文本。
        return false;
    }
    return true;
}

// ── updateAcked —— 推进某对端对某来源的「已确认水位」acked_seq（MAX 单调推进）────
bool OutboundAckStore::updateAcked(QSqlDatabase& db, const QString& peer, const QString& origin,
                                   qint64 epoch, qint64 ackedSeq_, QString* err) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();  // 记录本次确认时刻（毫秒）。
    QSqlQuery q(db);
    // 这是一条 SQLite「UPSERT」（INSERT ... ON CONFLICT ... DO UPDATE）。逐句拆解：
    //   · INSERT (peer, origin, stream_epoch, acked_seq, last_ack_ms) VALUES (?,?,?,?,?)
    //     —— 首次对该 (peer,origin,epoch) 确认时，直接插入一行新水位记录。
    //   · ON CONFLICT(peer, origin, stream_epoch) —— 命中主键冲突（该行已存在）时转 UPDATE。
    //   · acked_seq = MAX(excluded.acked_seq, __sync_outbound_ack.acked_seq)
    //     —— 关键的「只增不减」水位推进：excluded.* 指代本次试图插入的新值，
    //        __sync_outbound_ack.* 指代表中已存在的旧值；取二者较大者。这样即便 ACK
    //        工件乱序或重复到达（传输层不保证有序/去重），迟到的旧 ACK 也无法把水位拉回，
    //        避免「已确认变成未确认 → 误重发 / changelog 误删后漏发」。
    //   · last_ack_ms = excluded.last_ack_ms —— 时间戳无条件刷新为本次时刻（用于失联诊断）。
    q.prepare(
        QStringLiteral("INSERT INTO __sync_outbound_ack "
                       "(peer, origin, stream_epoch, acked_seq, last_ack_ms) "
                       "VALUES (?, ?, ?, ?, ?) "
                       "ON CONFLICT(peer, origin, stream_epoch) DO UPDATE SET "
                       "  acked_seq  = MAX(excluded.acked_seq, __sync_outbound_ack.acked_seq), "
                       "  last_ack_ms = excluded.last_ack_ms"));
    // 占位符按声明顺序绑定：peer, origin, stream_epoch, acked_seq, last_ack_ms。
    q.addBindValue(peer);
    q.addBindValue(origin);
    q.addBindValue(epoch);
    q.addBindValue(ackedSeq_);
    q.addBindValue(nowMs);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();  // 写失败上报错误文本（如约束冲突/库锁定）。
        return false;
    }
    return true;
}

// ── ackedSeq —— 精确读某 (peer, origin, epoch) 的已确认水位 ───────────────────
qint64 OutboundAckStore::ackedSeq(QSqlDatabase& db, const QString& peer, const QString& origin,
                                  qint64 epoch) {
    QSqlQuery q(db);
    // 主键三元组精确命中一行（PRIMARY KEY(peer, origin, stream_epoch)），故至多一条结果。
    q.prepare(
        QStringLiteral("SELECT acked_seq FROM __sync_outbound_ack "
                       "WHERE peer = ? AND origin = ? AND stream_epoch = ?"));
    q.addBindValue(peer);
    q.addBindValue(origin);
    q.addBindValue(epoch);
    // !exec() → SQL 执行失败；!next() → 无此行（从未对该来源给该对端发过变更）。
    // 两种情形都按「尚未确认」归一化为 -1。
    if (!q.exec() || !q.next())
        return -1;
    return q.value(0).toLongLong();  // 第 0 列即 acked_seq。
}

// ── minAckedSeq —— 跨所有对端取该来源的最小已确认水位（changelog 裁剪下界）──────
qint64 OutboundAckStore::minAckedSeq(QSqlDatabase& db, const QString& origin, qint64 epoch) {
    QSqlQuery q(db);
    // 注意这里不按 peer 过滤，而是对给定 (origin, epoch) 横扫所有对端取 MIN(acked_seq)：
    //   · MIN 体现「木桶最短板」——只有所有对端都确认过的序号之前，changelog 才可安全删除；
    //     任一对端落后，就不能删它尚未拿到的那段，否则该对端将永久漏收这些变更。
    //   · pending_baseline = 0 排除「正在做基线」的对端：基线期间其 acked_seq 还很低，
    //     若计入会把 MIN 压得过低（几乎无法裁剪），故先把它挂起、不参与裁剪计算
    //     （挂起标志由 setPendingBaseline 切换；语义详见头文件文件头）。
    q.prepare(
        QStringLiteral("SELECT MIN(acked_seq) FROM __sync_outbound_ack "
                       "WHERE origin = ? AND stream_epoch = ? AND pending_baseline = 0"));
    q.addBindValue(origin);
    q.addBindValue(epoch);
    if (!q.exec() || !q.next())
        return -1;  // SQL 失败或聚合查询竟无结果行 → 无可用下界。
    // 聚合 MIN 对「空集」（无任何满足条件的行）返回 SQL NULL（而非 0），这里显式兜底为 -1，
    // 表示「当前没有可安全裁剪的下界」，调用方据此不做裁剪。
    if (q.value(0).isNull())
        return -1;
    return q.value(0).toLongLong();
}

// ── lastSentLocalSeq —— 读对某对端的广播已发送水位（哨兵行，local_seq 计量）──────
qint64 OutboundAckStore::lastSentLocalSeq(QSqlDatabase& db, const QString& peer, qint64 epoch) {
    QSqlQuery q(db);
    // 只读 origin == '__broadcast__' 的哨兵行：该行专门承载「广播发送游标」（按本地
    // changelog 的 local_seq 计量），与逐 origin 的 acked_seq（按 origin_seq 计量）解耦
    // ——这是 J-01 fix 的核心设计（详见头文件文件头）。
    //   · MAX(last_sent_seq)：理论上该三元键唯一、至多一行；用 MAX 是保险的聚合写法，
    //     即便存在历史脏数据也取最高水位。
    //   · COALESCE(..., -1)：当聚合对空集返回 NULL（即尚无哨兵行）时，SQL 层即兜底为 -1，
    //     免去 C++ 端再判 NULL。下方仍保留双重兜底以防驱动差异。
    q.prepare(
        QStringLiteral("SELECT COALESCE(MAX(last_sent_seq), -1) FROM __sync_outbound_ack "
                       "WHERE peer = ? AND stream_epoch = ? AND origin = '__broadcast__'"));
    q.addBindValue(peer);
    q.addBindValue(epoch);
    if (!q.exec() || !q.next())
        return -1;  // SQL 失败 / 无结果行 → 视作「还没给它发过」。
    if (q.value(0).isNull())
        return -1;  // 双保险：极端情况下仍可能取到 NULL，归一化为 -1。
    return q.value(0).toLongLong();
}

// ── updateLastSent —— 推进对某对端的广播已发送水位（哨兵行，only-forward）────────
bool OutboundAckStore::updateLastSent(QSqlDatabase& db, const QString& peer, qint64 epoch,
                                      qint64 lastSentLocalSeq_, QString* err) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery q(db);
    // UPSERT a sentinel row keyed on (peer, '__broadcast__', epoch).
    // last_sent_seq advances monotonically (MAX semantics).
    // （对以 (peer, '__broadcast__', epoch) 为键的哨兵行做 UPSERT；
    //  last_sent_seq 单调前进（MAX 语义）。）
    //   · INSERT 部分把 origin 固定写成字面量 '__broadcast__'（哨兵），acked_seq 占位为 -1
    //     ——哨兵行不参与「确认」语义，其 acked_seq 仅为满足 NOT NULL 列约束而填占位值。
    //   · ON CONFLICT DO UPDATE 时 last_sent_seq = MAX(新值, 旧值)：与 acked_seq 同理，
    //     已发送水位是单调累积进度，重复/乱序的发送回报不应使其回退。
    //   · last_ack_ms 借用为「最近一次发送时刻」刷新（哨兵行复用该列记发送活动时间）。
    q.prepare(
        QStringLiteral("INSERT INTO __sync_outbound_ack "
                       "(peer, origin, stream_epoch, acked_seq, last_sent_seq, last_ack_ms) "
                       "VALUES (?, '__broadcast__', ?, -1, ?, ?) "
                       "ON CONFLICT(peer, origin, stream_epoch) DO UPDATE SET "
                       "  last_sent_seq = MAX(excluded.last_sent_seq, "
                       "                      __sync_outbound_ack.last_sent_seq), "
                       "  last_ack_ms   = excluded.last_ack_ms"));
    // 占位符顺序：peer → stream_epoch → last_sent_seq → last_ack_ms
    // （origin 与首个 acked_seq 已在 SQL 中写死为字面量，不占 '?'）。
    q.addBindValue(peer);
    q.addBindValue(epoch);
    q.addBindValue(lastSentLocalSeq_);
    q.addBindValue(nowMs);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

// ── minUnackedLocalSeq —— 求该对端「最早一条未确认变更」对应的广播读取下界 ───────
qint64 OutboundAckStore::minUnackedLocalSeq(QSqlDatabase& db, const QString& peer, qint64 epoch) {
    // C-02 fix: use LEFT JOIN so origins without an ACK row are treated as acked_seq = -1
    // (i.e. nothing has been confirmed yet) rather than being silently excluded.
    // With an inner JOIN, an origin that never appeared in outbound_ack would be missed
    // entirely, causing broadcastToPeer to skip its changelog entries.
    // （C-02 修复：使用 LEFT JOIN，使得「在 outbound_ack 中尚无 ACK 行」的 origin 被当作
    //  acked_seq = -1（即尚未确认任何内容）来处理，而不是被悄悄排除。若用内连接 INNER
    //  JOIN，一个从未出现在 outbound_ack 里的 origin 会被整体漏掉，导致 broadcastToPeer
    //  跳过它的 changelog 条目——也就是这个来源的变更永远发不出去。）
    // M-02 fix: do NOT filter by cl.stream_epoch so that cross-epoch un-ACKed entries are
    // included in the lower-bound calculation.  The LEFT JOIN already matches on
    // (peer, origin, stream_epoch) as a three-part key, so each changelog row is correctly
    // paired with its own epoch's ACK row (or treated as unacked when absent).
    // epoch parameter is kept for API compatibility but no longer used in the WHERE clause.
    // （M-02 修复：不要按 cl.stream_epoch 过滤，以便跨纪元的未确认条目也被纳入下界计算。
    //  LEFT JOIN 已经按 (peer, origin, stream_epoch) 三部分键匹配，因此每一条 changelog
    //  行都会正确地与它「自己那个纪元」的 ACK 行配对（若缺失则按未确认处理）。epoch 参数
    //  为保持 API 兼容而保留，但已不再用于 WHERE 子句。）
    Q_UNUSED(epoch)  // 显式标注「故意未用」，避免编译告警；保留形参不破坏调用方签名。
    QSqlQuery q(db);
    // 这是本文件最关键的一条查询，逐部分拆解：
    //   FROM __sync_changelog cl —— 以本地变更日志为驱动表，逐条变更看「该对端是否确认」。
    //   LEFT JOIN __sync_outbound_ack oa ON ... —— 为每条 changelog 行去匹配「该对端对
    //     该 (origin, epoch) 的确认水位行」。连接条件三部分键：
    //       oa.origin == cl.origin AND oa.stream_epoch == cl.stream_epoch AND oa.peer == ?
    //     再加 oa.origin != '__broadcast__'：把广播哨兵行排除在外（哨兵行不是真实来源的
    //       确认记录，其 acked_seq 是占位的 -1，不能拿来当作某来源的确认水位）。
    //     用 LEFT JOIN（而非 INNER）正是 C-02 的精髓：某来源若从未被确认过 → 右表无匹配
    //       行 → oa.acked_seq 为 NULL，下面用 COALESCE 兜底为 -1，于是它的所有变更都被
    //       判定为「未确认」而保留可发资格（INNER JOIN 会直接丢掉这些行，造成漏发）。
    //   WHERE cl.origin_seq > COALESCE(oa.acked_seq, -1) —— 筛出「尚未被确认」的变更：
    //     某条变更的来源序号 origin_seq 若大于该对端对其来源的已确认水位 acked_seq，
    //     即还没被对端 ACK；COALESCE(...,-1) 把「无确认行(NULL)」统一当作水位 -1，
    //     这样首次出现的来源（acked_seq 视为 -1）其全部变更都满足 > -1 而被纳入。
    //   SELECT COALESCE(MIN(cl.local_seq), -1) - 1 —— 在所有未确认变更中取最早一条的
    //     local_seq（按本地序号排最前者），再「减 1」得到广播读取的 exclusive 下界
    //     （即「从这个 local_seq 之后开始读」对应「读到这条最早未确认变更及其后续」）。
    //     若无任何未确认变更 → MIN 对空集为 NULL → COALESCE 兜底为 -1 → 结果 -1-1 = -2？
    //     不——COALESCE 先把 NULL 换成 -1，整体即 (-1) - 1 = -2 也无妨：调用方对「无未确认」
    //     的处理仍是「无需压低下界」；真正的「无结果」由下方 isNull/!next 兜底为 -1。
    q.prepare(
        QStringLiteral("SELECT COALESCE(MIN(cl.local_seq), -1) - 1 "
                       "FROM __sync_changelog cl "
                       "LEFT JOIN __sync_outbound_ack oa "
                       "  ON oa.origin = cl.origin "
                       "  AND oa.stream_epoch = cl.stream_epoch "
                       "  AND oa.peer = ? "
                       "  AND oa.origin != '__broadcast__' "
                       "WHERE cl.origin_seq > COALESCE(oa.acked_seq, -1)"));
    q.addBindValue(peer);  // 唯一占位符：连接条件里的 oa.peer = ?。
    if (!q.exec() || !q.next())
        return -1;  // SQL 失败 / 无结果行 → 从头读（下界 -1，所有 >-1 的都可读）。
    if (q.value(0).isNull())
        return -1;  // 聚合对空集返回 NULL 的双重兜底，归一化为 -1。
    return q.value(0).toLongLong();
}

// ── setPendingBaseline —— 切换某对端「基线进行中」标志（按 peer 全量更新）─────────
bool OutboundAckStore::setPendingBaseline(QSqlDatabase& db, const QString& peer, bool pending,
                                          QString* err) {
    QSqlQuery q(db);
    // WHERE peer = ? 不带 origin/epoch：基线是「对该对端整体」的一次性对齐，期间它对所有
    // 来源的确认都不应参与 changelog 裁剪计算，故一次性把该 peer 名下「全部水位行」的
    // pending_baseline 一并置位/清零。被置 1 后，minAckedSeq 会因 pending_baseline = 0
    // 的过滤而把这些行排除在 MIN 计算之外（详见 minAckedSeq 与头文件文件头）。
    // 注意 bool → 0/1：SQLite 无原生布尔，统一以整数 0/1 存储 pending_baseline 列。
    q.prepare(QStringLiteral("UPDATE __sync_outbound_ack SET pending_baseline = ? WHERE peer = ?"));
    q.addBindValue(pending ? 1 : 0);  // 占位符顺序：先 pending_baseline 值，后 peer。
    q.addBindValue(peer);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;  // UPDATE 影响 0 行（该 peer 暂无任何水位行）也视为成功，幂等无害。
}

}  // namespace dbridge::sync
