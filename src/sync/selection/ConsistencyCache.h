#pragma once
#include <QByteArray>
#include <QHash>
#include <QSqlDatabase>
#include <QString>

namespace dbridge::sync {

// In-memory (optionally durable) fingerprint cache for determining
// whether a dependency row is already consistent with the center.
class ConsistencyCache {
   public:
    // Initialize; if durable=true, creates/reads __sync_consistency_cache table.
    bool init(QSqlDatabase& db, bool durable, QString* err);

    // Returns true if the local fingerprint matches the cached center fingerprint.
    bool isConsistent(const QString& table, const QString& pk, const QByteArray& localFp) const;

    // Record that center has confirmed this row's fingerprint.
    void stampFromAuthoritative(QSqlDatabase& db, const QString& table, const QString& pk,
                                const QByteArray& centerFp);

    // Invalidate all cached entries for a table (called on inbound apply).
    void invalidateTable(QSqlDatabase& db, const QString& table);

   private:
    // memCache_[table][pk] = centerFingerprint
    QHash<QString, QHash<QString, QByteArray>> memCache_;
    bool durable_ = true;

    bool loadFromDb(QSqlDatabase& db, QString* err);
    bool persistStamp(QSqlDatabase& db, const QString& table, const QString& pk,
                      const QByteArray& fp);
    bool deleteTable(QSqlDatabase& db, const QString& table);
};

}  // namespace dbridge::sync
