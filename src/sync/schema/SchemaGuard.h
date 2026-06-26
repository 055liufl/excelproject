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
    // M-02 fix: accept verifyFingerprint flag at construction; when false, fingerprint
    // comparison is skipped in verifyPayload() (version mismatch still rejects).
    explicit SchemaGuard(bool verifyFingerprint = true) : verifyFingerprint_(verifyFingerprint) {
    }

    void setLocal(qint64 localVer, const QString& localFp);

    // Returns true if payload matches the local baseline.
    // On mismatch *err is set to a human-readable reason.
    bool verifyPayload(qint64 payloadVer, const QString& payloadFp, QString* err);

    // Compute a schema fingerprint for the given tables: sorted column names,
    // types, and PK list → SHA-256 hex string.
    static QString computeFingerprint(QSqlDatabase& db, const QStringList& tables);

    // Current local fingerprint (set via setLocal)
    QString fingerprint() const {
        return localFp_;
    }

   private:
    qint64 localVer_ = 0;
    QString localFp_;
    bool verifyFingerprint_ = true;
};

}  // namespace dbridge::sync
