#include "FrozenManifest.h"

#include <QSqlError>
#include <QSqlQuery>

namespace dbridge::sync {

bool FrozenManifest::init(QSqlDatabase& db, QString* err) {
    // Table is created by SyncDDL::allCreateStatements(); nothing extra needed here.
    Q_UNUSED(db);
    Q_UNUSED(err);
    return true;
}

bool FrozenManifest::save(QSqlDatabase& db, const QString& pushId, int chunkSeq,
                          const QList<FrozenEntry>& entries, QString* err) {
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("INSERT OR REPLACE INTO __sync_frozen_manifest "
                       "(push_id, chunk_seq, table_name, pk_hash, primary_key, record_kind, "
                       "topo_index, fingerprint) "
                       "VALUES (?,?,?,?,?,?,?,?)"));

    for (const auto& e : entries) {
        q.addBindValue(pushId);
        q.addBindValue(chunkSeq);
        q.addBindValue(e.table);
        q.addBindValue(e.pkHash);
        q.addBindValue(e.primaryKey);
        q.addBindValue(e.recordKind);
        q.addBindValue(e.topoIndex);
        q.addBindValue(e.fingerprint);
        if (!q.exec()) {
            if (err)
                *err = q.lastError().text();
            return false;
        }
    }
    return true;
}

QList<FrozenEntry> FrozenManifest::load(QSqlDatabase& db, const QString& pushId, int chunkSeq) {
    QList<FrozenEntry> result;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT table_name, pk_hash, primary_key, record_kind, topo_index, fingerprint "
        "FROM __sync_frozen_manifest WHERE push_id=? AND chunk_seq=? "
        "ORDER BY topo_index"));
    q.addBindValue(pushId);
    q.addBindValue(chunkSeq);
    if (!q.exec())
        return result;

    while (q.next()) {
        FrozenEntry e;
        e.table = q.value(0).toString();
        e.pkHash = q.value(1).toString();
        e.primaryKey = q.value(2).toString();
        e.recordKind = q.value(3).toString();
        e.topoIndex = q.value(4).toInt();
        e.fingerprint = q.value(5).toByteArray();
        result.append(e);
    }
    return result;
}

bool FrozenManifest::remove(QSqlDatabase& db, const QString& pushId, QString* err) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("DELETE FROM __sync_frozen_manifest WHERE push_id=?"));
    q.addBindValue(pushId);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

}  // namespace dbridge::sync
