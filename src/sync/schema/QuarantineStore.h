#pragma once
#include <QByteArray>
#include <QList>
#include <QPair>
#include <QSqlDatabase>
#include <QString>

namespace dbridge::sync {

// Stores payloads whose schemaVer is ahead of the local baseline.
// drainReady() returns payloads that can now be replayed.
class QuarantineStore {
   public:
    bool init(QSqlDatabase& db, QString* err);

    // Persist a payload under quarantine.
    bool quarantine(QSqlDatabase& db, const QString& origin, qint64 seq, qint64 epoch,
                    qint64 schemaVer, const QByteArray& payload, QString* err);

    // H-01 fix: return (id, payload) pairs for all rows where payload_schema_ver <=
    // currentSchemaVer, ordered by id ASC (arrival order).  Rows are NOT deleted here;
    // call markReplayed(id) after each successful replay.
    QList<QPair<qint64, QByteArray>> drainReady(QSqlDatabase& db, qint64 currentSchemaVer);

    // H-01 fix: delete a successfully replayed quarantine row.
    void markReplayed(QSqlDatabase& db, qint64 id);
};

}  // namespace dbridge::sync
