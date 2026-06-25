#pragma once
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

namespace dbridge::sync {

// Minimal schema-version + fingerprint guard.
// Caller sets the local baseline via setLocal(); verifyPayload() then
// rejects any payload whose (schemaVer, schemaFp) diverges from that baseline.
class SchemaGuard {
   public:
    void setLocal(qint64 localVer, const QString& localFp);

    // Returns true if payload matches the local baseline.
    // On mismatch *err is set to a human-readable reason.
    bool verifyPayload(qint64 payloadVer, const QString& payloadFp, QString* err);

    // Compute a schema fingerprint for the given tables: sorted column names,
    // types, and PK list → SHA-256 hex string.
    static QString computeFingerprint(QSqlDatabase& db, const QStringList& tables);

   private:
    qint64 localVer_ = 0;
    QString localFp_;
};

}  // namespace dbridge::sync
