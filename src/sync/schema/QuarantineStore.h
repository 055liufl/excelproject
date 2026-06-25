#pragma once
#include <QByteArray>
#include <QList>
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

    // Return (and delete) all rows where payload_schema_ver <= currentSchemaVer,
    // ordered by (origin_seq ASC) so they replay in order.
    QList<QByteArray> drainReady(QSqlDatabase& db, qint64 currentSchemaVer);
};

}  // namespace dbridge::sync
