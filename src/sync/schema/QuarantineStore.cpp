#include "QuarantineStore.h"

#include <QDateTime>  // created_ms 入库时间戳
#include <QSqlError>  // 取 lastError() 文本
#include <QSqlQuery>
#include <QVariant>

// ============================================================================
// QuarantineStore.cpp — 隔离区的存取实现
// 设计意图（schemaVer 超前、两步重放法）详见 QuarantineStore.h 的文件头注释。
// ============================================================================

namespace dbridge::sync {

bool QuarantineStore::init(QSqlDatabase& db, QString* err) {
    // 零行查询（WHERE 0 恒假）：仅探测 __sync_quarantine 表是否存在/连接是否可用。
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM __sync_quarantine WHERE 0"))) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

bool QuarantineStore::quarantine(QSqlDatabase& db, const QString& origin, qint64 seq, qint64 epoch,
                                 qint64 schemaVer, const QByteArray& payload, QString* err) {
    // 记录入库时刻：供排查（隔离了多久）与可能的清理策略使用。
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery q(db);
    // 不显式写 id：依赖表的自增主键，自增值天然反映「到达顺序」，drainReady 据此定序。
    q.prepare(QStringLiteral(
        "INSERT INTO __sync_quarantine "
        "(origin, origin_seq, stream_epoch, payload_schema_ver, payload, created_ms) "
        "VALUES (?, ?, ?, ?, ?, ?)"));
    q.addBindValue(origin);
    q.addBindValue(seq);
    q.addBindValue(epoch);
    q.addBindValue(schemaVer);  // 重放门槛：本地 currentSchemaVer 需 >= 它
    q.addBindValue(payload);    // QByteArray 绑定为 BLOB，原始字节无损保存
    q.addBindValue(nowMs);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

QList<QPair<qint64, QByteArray>> QuarantineStore::drainReady(QSqlDatabase& db,
                                                             qint64 currentSchemaVer) {
    QList<QPair<qint64, QByteArray>> result;

    // H-01 fix: order by id ASC (arrival order) so cross-origin replay preserves insertion
    // sequence.
    // 译：H-01 fix —— 按 id 升序（= 到达顺序）排列，使来自不同 origin 的载荷在重放时
    //   仍保持其原本的入库先后。否则乱序重放可能破坏依赖（如先放“引用”后放“被引用”）。
    // 筛选条件 payload_schema_ver <= currentSchemaVer：只取「本地结构已能容纳」的载荷；
    //   版本仍超前的继续留在隔离区，等下一次基线推进再说。
    QSqlQuery sel(db);
    sel.prepare(
        QStringLiteral("SELECT id, payload FROM __sync_quarantine "
                       "WHERE payload_schema_ver <= ? "
                       "ORDER BY id ASC"));
    sel.addBindValue(currentSchemaVer);
    if (!sel.exec())
        return result;  // 查询失败：返回空列表（本轮不重放任何东西，下轮再试）

    // 注意：此处只「读」，绝不删除——删除留给重放成功后的 markReplayed（两步法核心）。
    while (sel.next())
        result.append(qMakePair(sel.value(0).toLongLong(), sel.value(1).toByteArray()));

    return result;
}

void QuarantineStore::markReplayed(QSqlDatabase& db, qint64 id) {
    // 按主键删除这一条已成功重放的隔离行。
    // 故意忽略 exec() 返回值：删除失败不抛错也不阻断——最坏只是该行残留，
    // 下次 drainReady 会再取出、上层重放应是幂等的，故重复一次无害。
    QSqlQuery del(db);
    del.prepare(QStringLiteral("DELETE FROM __sync_quarantine WHERE id = ?"));
    del.addBindValue(id);
    del.exec();
}

}  // namespace dbridge::sync
