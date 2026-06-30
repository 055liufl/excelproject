#include "ChangelogStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

// ============================================================================
// ChangelogStore.cpp — __sync_changelog 读写层的实现
// ============================================================================
//
// 【职责】实现 ChangelogStore.h 声明的全部方法：初始化探测、追加日志(append/appendForward)、
//   范围读取(readRange/readRangeAll)、水位查询(maxLocalSeq)、压实(truncate)，以及私有的
//   统一插入 insertRow 与校验和 blobChecksum。本文件全部是直白的参数化 SQL，无复杂算法，
//   重点在「每条 SQL 为什么这么写」与各 fix 的动机。
//
// 【在管线中的位置】capture 阶段的存档落点：本地写被 session 捕获 → append 入库；对端变更
//   转发 → appendForward 入库；广播 → readRangeAll 取出。详见 .h 文件头。
//
// 【贯穿概念】local_seq(全局 FIFO 序) / origin+origin_seq(来源流序号) / stream_epoch(流纪元)
//   / push_id(选择性推送批次) —— 字段含义见 .h 文件头。
// ============================================================================

namespace dbridge::sync {

// init —— 仅探测表是否存在/可读。`WHERE 0` 让查询条件恒为假，不取任何行，只触发 SQLite
//   对表结构的解析；表缺失或不可读时 exec 失败。不负责建表。
bool ChangelogStore::init(QSqlDatabase& db, QString* err) {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM __sync_changelog WHERE 0"))) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

// append —— 公有薄包装：把「本地产出」变更的全部字段透传给 insertRow。
//   语义见 .h（分支 B/C）；本机自产时 authoritative 由调用方传 true。
bool ChangelogStore::append(QSqlDatabase& db, const QString& kind, const QString& origin,
                            const QString& sourcePeer, qint64 originSeq, qint64 parentSeq,
                            qint64 epoch, qint64 schemaVer, const QString& schemaFp,
                            const QByteArray& changeset, bool authoritative, qint64* localSeqOut,
                            QString* err, const QString& pushId) {
    return insertRow(db, kind, origin, sourcePeer, originSeq, parentSeq, epoch, schemaVer, schemaFp,
                     changeset, authoritative, localSeqOut, err, pushId);
}

// appendForward —— 公有薄包装：把「收到的对端变更」原样存档（转发用）。
//   固定三项语义：kind="forward"、parentSeq=0（转发条目无父序号）、authoritative=false
//   （转发的不是本机权威产出）。其余字段透传 insertRow。
bool ChangelogStore::appendForward(QSqlDatabase& db, const QString& origin,
                                   const QString& sourcePeer, qint64 originSeq, qint64 epoch,
                                   qint64 schemaVer, const QString& schemaFp,
                                   const QByteArray& changesetBlob, qint64* localSeqOut,
                                   QString* err, const QString& pushId) {
    // forwarded changesets: kind="forward", parentSeq=0, authoritative=false
    return insertRow(db, QStringLiteral("forward"), origin, sourcePeer, originSeq, /*parentSeq=*/0,
                     epoch, schemaVer, schemaFp, changesetBlob, /*authoritative=*/false,
                     localSeqOut, err, pushId);
}

// readRange —— 按 origin 流读取一段范围：取 origin == peer 且 origin_seq > afterOriginSeq
//   的条目，按 origin_seq 升序、上限 limit 条。
// 【为什么按 origin_seq 排序】同一 origin 内 origin_seq 严格递增，升序即「自然时间序」，
//   保证回放顺序正确、便于发现序号空洞(gap)。
// 【参数】peer 此处即 origin（调用方做同一 origin 范围读取时令 peer==origin）；afterOriginSeq
//   是排他下界（不含等于）；limit 单批上限。
// 【返回/错误】返回命中条目列表；exec 失败时返回「空列表」（不抛错、不区分空表与失败——
//   调用方据需要自行处理）。三个 ? 占位符按 addBindValue 顺序绑定，防注入。
QList<ChangelogStore::Entry> ChangelogStore::readRange(QSqlDatabase& db, const QString& peer,
                                                       qint64 afterOriginSeq, int limit) {
    // peer-based range: return entries for the given origin after afterOriginSeq.
    // Caller sets peer == origin for same-origin reads.
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("SELECT local_seq, origin, origin_seq, changeset, byte_size "
                       "FROM __sync_changelog "
                       "WHERE origin = ? AND origin_seq > ? "
                       "ORDER BY origin_seq ASC "
                       "LIMIT ?"));
    q.addBindValue(peer);            // → origin = ?
    q.addBindValue(afterOriginSeq);  // → origin_seq > ?
    q.addBindValue(limit);           // → LIMIT ?

    QList<Entry> result;
    if (!q.exec())
        return result;  // 查询失败 → 返回空列表
    // 逐行取出，列下标与 SELECT 列表一一对应（0=local_seq … 4=byte_size）。
    while (q.next()) {
        Entry e;
        e.localSeq = q.value(0).toLongLong();
        e.origin = q.value(1).toString();
        e.originSeq = q.value(2).toLongLong();
        e.changeset = q.value(3).toByteArray();
        e.byteSize = q.value(4).toLongLong();
        result.append(e);
    }
    return result;
}

// readRangeAll —— 广播扇出读取：取 local_seq > afterLocalSeq 且 origin != excludeOrigin 的
//   条目，按 local_seq 升序(FIFO 发送序)、上限 limit 条。
// 【为什么这样筛】向某 peer 广播时，excludeOrigin 设为该 peer 的 origin：
//   · origin != excludeOrigin —— 绝不把 peer 自己产生的变更回声给它（防回环）；
//   · local_seq > afterLocalSeq —— afterLocalSeq 是上次发给该 peer 的水位，只发增量；
//   · ORDER BY local_seq ASC —— 跨多个 origin 的条目按全局接收序发送，保证 FIFO。
// 【与 readRange 的区别】readRange 锁定单一 origin 按 origin_seq 取；本方法跨 origin 按全局
//   local_seq 取，并多带 stream_epoch 与 push_id 两个广播必需字段。
QList<ChangelogStore::EntryFull> ChangelogStore::readRangeAll(QSqlDatabase& db,
                                                              const QString& excludeOrigin,
                                                              qint64 afterLocalSeq, int limit) {
    QSqlQuery q(db);
    // C-04 fix: also SELECT stream_epoch so broadcastToPeer can use each entry's original epoch
    // rather than overwriting every forwarded origin with the local node's epoch.
    // 【C-04 修复】同时取出 stream_epoch，使 broadcastToPeer 能沿用「每条目自己 origin 的原始
    //   纪元」，而不会把所有被转发的 origin 都错误地盖成本地节点的纪元。
    // H-01 fix: also SELECT push_id so the broadcast barrier can filter by the specific push_id
    // rather than blocking all entries for the origin (coarse over-blocking).
    // 【H-01 修复】同时取出 push_id，使广播屏障能按「具体某次推送」过滤，而不是粗暴地把该
    //   origin 的所有条目一并屏蔽（过度屏蔽）。
    q.prepare(QStringLiteral(
        "SELECT local_seq, origin, origin_seq, stream_epoch, changeset, byte_size, push_id "
        "FROM __sync_changelog "
        "WHERE origin != ? AND local_seq > ? "
        "ORDER BY local_seq ASC "
        "LIMIT ?"));
    q.addBindValue(excludeOrigin);  // → origin != ?
    q.addBindValue(afterLocalSeq);  // → local_seq > ?
    q.addBindValue(limit);          // → LIMIT ?

    QList<EntryFull> result;
    if (!q.exec())
        return result;  // 查询失败 → 返回空列表
    // 列下标与 SELECT 列表对应（0=local_seq … 6=push_id）。
    while (q.next()) {
        EntryFull e;
        e.localSeq = q.value(0).toLongLong();
        e.origin = q.value(1).toString();
        e.originSeq = q.value(2).toLongLong();
        e.streamEpoch = q.value(3).toLongLong();
        e.changeset = q.value(4).toByteArray();
        e.byteSize = q.value(5).toLongLong();
        e.pushId = q.value(6).toString();  // H-01 fix: NULL → empty string via toString()
                                           // 【H-01 修复】push_id 为 NULL 时 toString() 得空串。
        result.append(e);
    }
    return result;
}

// maxLocalSeq —— 返回当前日志中最大的 local_seq；空表返回 -1。
//   用 COALESCE(MAX(...), -1)：空表时 MAX 为 NULL，COALESCE 兜底成 -1。
//   查询/取行失败也返回 -1。用于初始化 peer 的 last_sent 发送水位（从 -1 起增量发送）。
qint64 ChangelogStore::maxLocalSeq(QSqlDatabase& db) {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT COALESCE(MAX(local_seq), -1) FROM __sync_changelog")))
        return -1;
    if (!q.next())
        return -1;
    return q.value(0).toLongLong();
}

// truncate —— 删除 local_seq < beforeLocalSeq 的旧条目（日志压实/GC）。
//   调用方通常以「所有 peer 都已确认收到的最小水位」作为 beforeLocalSeq，安全回收已无需
//   保留的历史。严格小于：保留 beforeLocalSeq 本身那条。失败置 *err 返回 false。
bool ChangelogStore::truncate(QSqlDatabase& db, qint64 beforeLocalSeq, QString* err) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("DELETE FROM __sync_changelog WHERE local_seq < ?"));
    q.addBindValue(beforeLocalSeq);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 私有实现
// private
// ---------------------------------------------------------------------------

// insertRow —— append / appendForward 共用的「写一条日志记录」实现。
// 【做什么】记录当前时间戳、算 changeset 的校验和与字节数，然后参数化 INSERT 一行；成功后
//   把 SQLite 自增分配的 local_seq 经 lastInsertId 回填给调用方。
// 【几处 NULL 化的讲究】source_peer / parent_seq / push_id 三列在「无意义」时显式绑定为
//   对应类型的 NULL（而非空串/0），让 NULL 在语义上代表「不适用」，并与列上的可空约束、
//   后续查询的 IS NULL 判定保持一致。注意 parent_seq==0 被视作「无父」→ 存 NULL。
bool ChangelogStore::insertRow(QSqlDatabase& db, const QString& kind, const QString& origin,
                               const QString& sourcePeer, qint64 originSeq, qint64 parentSeq,
                               qint64 epoch, qint64 schemaVer, const QString& schemaFp,
                               const QByteArray& changeset, bool authoritative, qint64* localSeqOut,
                               QString* err, const QString& pushId) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();  // created_ms：入库时刻(毫秒)
    const QString checksum = blobChecksum(changeset);          // payload_checksum：完整性校验
    const qint64 byteSize = changeset.size();  // byte_size：便于体积统计/告警

    QSqlQuery q(db);
    // H-01 fix: plain INSERT (not OR IGNORE) so a duplicate (origin,epoch,origin_seq) triggers
    // a real error that can be caught by the caller. Silently ignoring duplicates would let the
    // caller believe the changelog was updated when it was not, causing broadcast/ACK drift.
    // 【H-01 修复】用普通 INSERT（不是 INSERT OR IGNORE）：当出现重复的
    //   (origin, epoch, origin_seq) 唯一键时，要让它真正报错并被调用方捕获。若静默忽略重复，
    //   调用方会误以为日志已更新（实则没有），进而导致广播进度/ACK 水位漂移、数据不一致。
    // M-04 fix: include push_id column for selection push changesets (NULL for normal changesets).
    // 【M-04 修复】INSERT 列表含 push_id 列：选择性推送写其批次 id，普通变更写 NULL。
    q.prepare(
        QStringLiteral("INSERT INTO __sync_changelog "
                       "(kind, origin, source_peer, origin_seq, parent_seq, stream_epoch, "
                       " schema_ver, schema_fingerprint, changeset, payload_checksum, "
                       " byte_size, authoritative, created_ms, push_id) "
                       "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    // 以下 addBindValue 顺序与上面 14 个 ? 一一对应，不可错位。
    q.addBindValue(kind);
    q.addBindValue(origin);
    // source_peer 为空（本地自产）→ 绑定为 SQL NULL。
    q.addBindValue(sourcePeer.isEmpty() ? QVariant(QVariant::String) : QVariant(sourcePeer));
    q.addBindValue(originSeq);
    // parent_seq==0 视作「无父序号」→ 绑定为 LongLong 型 NULL。
    q.addBindValue(parentSeq == 0 ? QVariant(QVariant::LongLong) : QVariant(parentSeq));
    q.addBindValue(epoch);
    q.addBindValue(schemaVer);
    q.addBindValue(schemaFp);
    q.addBindValue(changeset);  // BLOB
    q.addBindValue(checksum);
    q.addBindValue(byteSize);
    q.addBindValue(authoritative ? 1 : 0);  // bool → 0/1
    q.addBindValue(nowMs);
    // M-04 fix: NULL for plain changesets, pushId for selection-push changesets.
    // 【M-04 修复】普通变更绑 NULL，选择性推送绑 pushId。
    q.addBindValue(pushId.isEmpty() ? QVariant(QVariant::String) : QVariant(pushId));

    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }

    // 回填本行自增主键 local_seq（调用方据此推进发送水位、关联 ACK）。
    if (localSeqOut) {
        *localSeqOut = q.lastInsertId().toLongLong();
    }
    return true;
}

// blobChecksum —— 计算 changeset 的 SHA-256 全摘要并转十六进制小写串（写入 payload_checksum）。
//   用途：接收端可据此校验变更字节在传输/存储中是否损坏（对应 E_SYNC_PAYLOAD_CORRUPT）。
QString ChangelogStore::blobChecksum(const QByteArray& data) {
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

}  // namespace dbridge::sync
