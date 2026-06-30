#include "InboxLedger.h"

#include <QDateTime>  // first_seen_ms / consumed_ms 时间戳
#include <QSqlError>  // lastError() 文本
#include <QSqlQuery>
#include <QVariant>

// ============================================================================
// InboxLedger.cpp — inbox 已处理台账的存取实现
// 幂等去重的动机与状态机（Seen → Consumed/Corrupt）详见 InboxLedger.h 文件头注释。
// 落盘里 status 列以小写字符串存储：'seen' / 'consumed' / 'corrupt'。
// ============================================================================

namespace dbridge::sync {

bool InboxLedger::init(QSqlDatabase& db, QString* err) {
    // 零行查询（WHERE 0 恒假）：仅探测 __sync_inbox_ledger 表是否可访问。
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM __sync_inbox_ledger WHERE 0"))) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

bool InboxLedger::markSeen(QSqlDatabase& db, const QString& artifactName, QString* err) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery q(db);
    // INSERT OR IGNORE：幂等的关键。artifact_name 是主键/唯一键，已存在则整条 INSERT 被忽略，
    //   故不会把一个已是 'consumed'/'corrupt' 的条目意外重置回 'seen'，也不会重写 first_seen_ms。
    q.prepare(
        QStringLiteral("INSERT OR IGNORE INTO __sync_inbox_ledger "
                       "(artifact_name, status, first_seen_ms) VALUES (?, 'seen', ?)"));
    q.addBindValue(artifactName);
    q.addBindValue(nowMs);  // 首次发现时刻，供 stalePending 计算滞留时长
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

bool InboxLedger::markConsumed(QSqlDatabase& db, const QString& artifactName, QString* err) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery q(db);
    // 把状态推进到 'consumed' 并记录消费时刻。这是“此后永不再处理”的持久化标记。
    q.prepare(
        QStringLiteral("UPDATE __sync_inbox_ledger SET status = 'consumed', consumed_ms = ? "
                       "WHERE artifact_name = ?"));
    q.addBindValue(nowMs);
    q.addBindValue(artifactName);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

bool InboxLedger::markCorrupt(QSqlDatabase& db, const QString& artifactName, QString* err) {
    QSqlQuery q(db);
    // 标记为 'corrupt'：损坏/无法应用，终态、不自动重试（不记时间戳，沿用 first_seen_ms 即可）。
    q.prepare(QStringLiteral(
        "UPDATE __sync_inbox_ledger SET status = 'corrupt' WHERE artifact_name = ?"));
    q.addBindValue(artifactName);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

LedgerStatus InboxLedger::status(QSqlDatabase& db, const QString& artifactName) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT status FROM __sync_inbox_ledger WHERE artifact_name = ?"));
    q.addBindValue(artifactName);
    // 查询失败「或」无命中行：都视为 Unknown（保守地当“没见过”，绝不假装已消费）。
    if (!q.exec() || !q.next())
        return LedgerStatus::Unknown;
    // 把落盘的小写字符串映射回枚举（用 QLatin1String 比较，避免临时 QString 分配）。
    const QString s = q.value(0).toString();
    if (s == QLatin1String("seen"))
        return LedgerStatus::Seen;
    if (s == QLatin1String("consumed"))
        return LedgerStatus::Consumed;
    if (s == QLatin1String("corrupt"))
        return LedgerStatus::Corrupt;
    return LedgerStatus::Unknown;  // 出现未知字符串（理论不应发生）→ 兜底当 Unknown
}

QStringList InboxLedger::pendingSeen(QSqlDatabase& db) {
    QStringList result;
    QSqlQuery q(db);
    // 列出所有还停留在 'seen'（待消费）的 artifact；查询失败则返回空列表。
    if (!q.exec(
            QStringLiteral("SELECT artifact_name FROM __sync_inbox_ledger WHERE status = 'seen'")))
        return result;
    while (q.next())
        result.append(q.value(0).toString());
    return result;
}

// M-01 fix: surface artifacts stuck in 'seen' longer than the gap timeout threshold.
// 译：M-01 fix —— 把「停留在 seen 状态超过空洞超时阈值」的 artifact 暴露出来。
QStringList InboxLedger::stalePending(QSqlDatabase& db, qint64 gapTimeoutMs) {
    // 计算截止时刻：first_seen_ms 早于 cutoff 的，即“已滞留超过 gapTimeoutMs”。
    const qint64 cutoff = QDateTime::currentMSecsSinceEpoch() - gapTimeoutMs;
    QStringList result;
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("SELECT artifact_name FROM __sync_inbox_ledger "
                       "WHERE status = 'seen' AND first_seen_ms < ?"));
    q.addBindValue(cutoff);
    if (!q.exec())
        return result;
    while (q.next())
        result.append(q.value(0).toString());
    return result;  // 由上层据此触发 E_SYNC_GAP / 基线回退
}

}  // namespace dbridge::sync
