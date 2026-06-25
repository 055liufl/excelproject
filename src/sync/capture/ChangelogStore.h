#pragma once
#include <QByteArray>
#include <QList>
#include <QSqlDatabase>
#include <QString>

namespace dbridge::sync {

// Persistence layer for __sync_changelog.
class ChangelogStore {
   public:
    bool init(QSqlDatabase& db, QString* err);

    // Branch B/C: write a locally-captured changeset (fresh capture or
    // re-encoded incoming). authoritative=true for own captures.
    bool append(QSqlDatabase& db, const QString& kind, const QString& origin,
                const QString& sourcePeer, qint64 originSeq, qint64 parentSeq, qint64 epoch,
                qint64 schemaVer, const QString& schemaFp, const QByteArray& changeset,
                bool authoritative, qint64* localSeqOut, QString* err);

    // Branch A: store incoming raw blob verbatim (forwarded changeset).
    bool appendForward(QSqlDatabase& db, const QString& origin, const QString& sourcePeer,
                       qint64 originSeq, qint64 epoch, qint64 schemaVer, const QString& schemaFp,
                       const QByteArray& changesetBlob, qint64* localSeqOut, QString* err);

    // Read entries after a peer anchor for broadcasting.
    struct Entry {
        qint64 localSeq;
        QString origin;
        qint64 originSeq;
        QByteArray changeset;
        qint64 byteSize;
    };
    QList<Entry> readRange(QSqlDatabase& db, const QString& peer, qint64 afterOriginSeq,
                           int limit = 1000);

    // Delete entries with local_seq < beforeLocalSeq (GC / compaction).
    bool truncate(QSqlDatabase& db, qint64 beforeLocalSeq, QString* err);

   private:
    // Shared INSERT helper; returns auto-assigned local_seq via lastInsertId.
    bool insertRow(QSqlDatabase& db, const QString& kind, const QString& origin,
                   const QString& sourcePeer, qint64 originSeq, qint64 parentSeq, qint64 epoch,
                   qint64 schemaVer, const QString& schemaFp, const QByteArray& changeset,
                   bool authoritative, qint64* localSeqOut, QString* err);

    // Compute SHA-256 hex of a changeset blob (for payload_checksum).
    static QString blobChecksum(const QByteArray& data);
};

}  // namespace dbridge::sync
