#include "AppliedVectorStore.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

// ============================================================================
// AppliedVectorStore.cpp — 已应用向量的 SQL 实现
// ============================================================================
//
// 【实现要点速览】
//   · 三个写方法 advance/reset/resetTo 都用 SQLite 的 UPSERT
//     （INSERT … ON CONFLICT(origin, stream_epoch) DO UPDATE …）一条语句搞定
//     「没有就插入、有就更新」，避免「先 SELECT 再决定 INSERT/UPDATE」的竞态。
//   · 它们的区别只在「冲突时更新什么、加不加防回退条件」——见各函数逐行注释。
//   · 所有读写都用「占位符 ? + addBindValue」绑定参数，杜绝 SQL 注入，且让 origin
//     这种任意字符串安全入库。
// ============================================================================

namespace dbridge::sync {

// ── init：表就绪自检 ─────────────────────────────────────────────────────────
bool AppliedVectorStore::init(QSqlDatabase& db, QString* err) {
    QSqlQuery q(db);
    // "WHERE 0" 永远为假：不返回任何行、几乎零开销，纯粹用来探测
    // 「__sync_applied_vector 表及其列是否存在」。若 SyncDDL 还没建表，这条会失败。
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM __sync_applied_vector WHERE 0"))) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

// ── check：seq 三态判定（去重 / 连续 / 空洞）────────────────────────────────
// 这是全类最核心的逻辑：决定一条来件「应用 / 跳过 / 暂留」。两种分支：
//   分支一：水位行尚不存在（本节点从未应用过该 (origin, epoch) 的任何变更）。
//   分支二：水位行已存在，拿 seq 与水位 applied_seq 比较。
SeqCheckResult AppliedVectorStore::check(QSqlDatabase& db, const QString& origin, qint64 epoch,
                                         qint64 seq, QString* err) {
    qint64 appliedSeq = -1;  // readRow 失败时被置 -1（哨兵：从未应用过）
    qint64 baselineGen = 0;  // 这里用不到 baselineGen，仅为满足 readRow 出参签名
    if (!readRow(db, origin, epoch, &appliedSeq, &baselineGen)) {
        // 【分支一】尚无水位行：等价于「水位 = 0」，期望的第一条必须正好是 seq==1。
        if (seq == 1)
            return SeqCheckResult::Apply;  // 正好是开篇第 1 条 → 应用
        if (seq <= 0)
            return SeqCheckResult::NoOp;  // 非法/0 号 → 当作幂等空操作吞掉（防御性）
        // seq >= 2 但本地一条都没应用过 → 中间缺了 1..seq-1，是空洞。
        if (err)
            *err = QStringLiteral("gap: no prior applied seq, got %1").arg(seq);
        return SeqCheckResult::Gap;
    }

    // 【分支二】水位行已存在，applied_seq 即当前水位。
    if (seq <= appliedSeq)
        return SeqCheckResult::NoOp;  // 已应用过（含重复投递）→ 幂等跳过
    if (seq == appliedSeq + 1)
        return SeqCheckResult::Apply;  // 正好是下一条 → 应用（连续）

    // 走到这里必有 seq >= appliedSeq+2：跳过了 appliedSeq+1 这条 → 空洞。
    if (err)
        *err = QStringLiteral("gap: applied=%1 but seq=%2").arg(appliedSeq).arg(seq);
    return SeqCheckResult::Gap;
}

// ── advance：推进水位（带防回退保护）────────────────────────────────────────
// 用一条 UPSERT 完成「行不存在则插入 applied_seq=seq；行已存在则把水位抬到 seq」。
bool AppliedVectorStore::advance(QSqlDatabase& db, const QString& origin, qint64 epoch, qint64 seq,
                                 QString* err) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();  // 记一笔更新时间戳（诊断用）
    QSqlQuery q(db);
    // 逐句解读这条 UPSERT：
    //   INSERT … VALUES(?, ?, ?, 0, ?)
    //     —— 首次出现该 (origin, epoch)：插入新行，applied_seq=seq，baseline_generation=0。
    //   ON CONFLICT(origin, stream_epoch) DO UPDATE SET applied_seq=excluded.applied_seq …
    //     —— 行已存在（撞主键）：把水位更新为本次的 seq。
    //   WHERE excluded.applied_seq > applied_seq
    //     —— 关键的「防回退守卫」：只有当新 seq 严格大于当前水位时才真正更新。
    //        excluded.* 指「本次试图插入的值」，不带前缀的 applied_seq 指「表中现存值」。
    //        没有这个 WHERE，乱序/重复投递可能把水位往回拨，破坏单调性。
    //        （正常流程下 check() 已保证 seq==水位+1，这里是纵深防御。）
    q.prepare(
        QStringLiteral("INSERT INTO __sync_applied_vector "
                       "(origin, stream_epoch, applied_seq, baseline_generation, updated_ms) "
                       "VALUES (?, ?, ?, 0, ?) "
                       "ON CONFLICT(origin, stream_epoch) DO UPDATE SET "
                       "  applied_seq  = excluded.applied_seq, "
                       "  updated_ms   = excluded.updated_ms "
                       "WHERE excluded.applied_seq > applied_seq"));
    q.addBindValue(origin);  // ? #1 → origin
    q.addBindValue(epoch);   // ? #2 → stream_epoch
    q.addBindValue(seq);     // ? #3 → applied_seq（新水位）
    q.addBindValue(nowMs);   // ? #4 → updated_ms
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

// ── reset：基线重置，水位归零 ───────────────────────────────────────────────
// 与 advance 不同：这里 applied_seq 硬写 0（无条件归零），并刷新 baseline_generation。
// 适用于「该 origin 的全量基线起点 = 空」的场景，后续增量从 seq=1 重新累计。
bool AppliedVectorStore::reset(QSqlDatabase& db, const QString& origin, qint64 epoch,
                               qint64 baselineGeneration, QString* err) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery q(db);
    // INSERT 分支：applied_seq=0（VALUES 里第三个字段直接是字面量 0）。
    // ON CONFLICT 分支：applied_seq 也硬写 0，并把 baseline_generation 抬到新值。
    // 注意此处「没有」防回退 WHERE：基线重置就是要强制覆盖，归零是其本意。
    q.prepare(
        QStringLiteral("INSERT INTO __sync_applied_vector "
                       "(origin, stream_epoch, applied_seq, baseline_generation, updated_ms) "
                       "VALUES (?, ?, 0, ?, ?) "
                       "ON CONFLICT(origin, stream_epoch) DO UPDATE SET "
                       "  applied_seq          = 0, "
                       "  baseline_generation  = excluded.baseline_generation, "
                       "  updated_ms           = excluded.updated_ms"));
    q.addBindValue(origin);              // ? #1 → origin
    q.addBindValue(epoch);               // ? #2 → stream_epoch
    q.addBindValue(baselineGeneration);  // ? #3 → baseline_generation（新基线代号）
    q.addBindValue(nowMs);               // ? #4 → updated_ms
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

// ── resetTo：基线重置到指定 origin_seq（C-03 fix）───────────────────────────
// 与 reset() 的唯一差别：applied_seq 写入「传入的 originSeq」而非硬零。
// 因为基线快照通常截止到某个 seq=N（不是空白起点），把水位置成 N 后，下一条增量
// 期望 N+1，gap 判定才不会把正常的后续变更误判成空洞。详见头文件 C-03 注释。
bool AppliedVectorStore::resetTo(QSqlDatabase& db, const QString& origin, qint64 epoch,
                                 qint64 originSeq, qint64 baselineGeneration, QString* err) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery q(db);
    // 与 reset() 结构相同，但 applied_seq 由占位符 ? 提供（= originSeq），无防回退守卫
    // （基线重置语义即「强制设定权威水位」，无条件覆盖）。
    q.prepare(
        QStringLiteral("INSERT INTO __sync_applied_vector "
                       "(origin, stream_epoch, applied_seq, baseline_generation, updated_ms) "
                       "VALUES (?, ?, ?, ?, ?) "
                       "ON CONFLICT(origin, stream_epoch) DO UPDATE SET "
                       "  applied_seq          = excluded.applied_seq, "
                       "  baseline_generation  = excluded.baseline_generation, "
                       "  updated_ms           = excluded.updated_ms"));
    q.addBindValue(origin);              // ? #1 → origin
    q.addBindValue(epoch);               // ? #2 → stream_epoch
    q.addBindValue(originSeq);           // ? #3 → applied_seq（权威截断点 N）
    q.addBindValue(baselineGeneration);  // ? #4 → baseline_generation
    q.addBindValue(nowMs);               // ? #5 → updated_ms
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

// ── current：读当前水位（便捷只读）──────────────────────────────────────────
// 转调 readRow；行不存在时 readRow 已把 appliedSeq 置 -1，原样返回。
// 返回 -1 = 「从未应用过」；返回 >=0 = 实际水位。
qint64 AppliedVectorStore::current(QSqlDatabase& db, const QString& origin, qint64 epoch) {
    qint64 appliedSeq = -1;
    qint64 baselineGen = 0;  // 此处不关心基线代号，仅占位
    readRow(db, origin, epoch, &appliedSeq, &baselineGen);
    return appliedSeq;
}

// ---------------------------------------------------------------------------
// private
// ---------------------------------------------------------------------------

// ── readRow：私有底层读，统一处理「行不存在」语义 ───────────────────────────
// 按 (origin, epoch) 主键查一行，把 (applied_seq, baseline_generation) 写到出参。
// 行不存在或查询失败 → 返回 false，并把出参置成哨兵值（-1 / 0）。
bool AppliedVectorStore::readRow(QSqlDatabase& db, const QString& origin, qint64 epoch,
                                 qint64* appliedSeq, qint64* baselineGen) {
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("SELECT applied_seq, baseline_generation "
                       "FROM __sync_applied_vector "
                       "WHERE origin = ? AND stream_epoch = ?"));
    q.addBindValue(origin);  // ? #1 → origin
    q.addBindValue(epoch);   // ? #2 → stream_epoch
    // 两种「没读到」都归为同一处理：exec() 出错，或 next() 取不到行（无此 (origin,epoch)）。
    if (!q.exec() || !q.next()) {
        if (appliedSeq)
            *appliedSeq = -1;  // 哨兵：从未应用过
        if (baselineGen)
            *baselineGen = 0;
        return false;
    }
    // 读到行：列 0 = applied_seq（水位），列 1 = baseline_generation。出参可为 nullptr（按需取）。
    if (appliedSeq)
        *appliedSeq = q.value(0).toLongLong();
    if (baselineGen)
        *baselineGen = q.value(1).toLongLong();
    return true;
}

}  // namespace dbridge::sync
