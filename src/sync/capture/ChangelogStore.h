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
    // M-04 fix: accepts optional pushId (empty for plain changesets, non-empty for selection push
    // changesets) so the broadcast layer can filter entries by push_id barrier.
    bool appendForward(QSqlDatabase& db, const QString& origin, const QString& sourcePeer,
                       qint64 originSeq, qint64 epoch, qint64 schemaVer, const QString& schemaFp,
                       const QByteArray& changesetBlob, qint64* localSeqOut, QString* err,
                       const QString& pushId = QString());

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

    // Full entry including origin, used for broadcast fan-out (J-01 fix).
    struct EntryFull {
        qint64 localSeq = 0;
        QString origin;
        qint64 originSeq = 0;
        qint64 streamEpoch = 0;  // C-04 fix: preserve original stream_epoch per origin
        QByteArray changeset;
        qint64 byteSize = 0;
        QString pushId;  // H-01 fix: non-empty for selection-push changesets; used by broadcast
                         // barrier to skip entries only when their specific push is still pending
    };

    // Read all entries with local_seq > afterLocalSeq whose origin != excludeOrigin.
    // Ordered by local_seq ASC (FIFO send order).  Used for broadcasting to a peer
    // so that we never echo a peer's own changes back to it, and always include our
    // own local changes (J-01 fix).
    QList<EntryFull> readRangeAll(QSqlDatabase& db, const QString& excludeOrigin,
                                  qint64 afterLocalSeq, int limit = 1000);

    // Return the maximum local_seq in the changelog, or -1 if empty.
    // Used to initialise last_sent watermarks (J-01 fix).
    qint64 maxLocalSeq(QSqlDatabase& db);

    // Delete entries with local_seq < beforeLocalSeq (GC / compaction).
    bool truncate(QSqlDatabase& db, qint64 beforeLocalSeq, QString* err);

   private:
    // Shared INSERT helper; returns auto-assigned local_seq via lastInsertId.
    bool insertRow(QSqlDatabase& db, const QString& kind, const QString& origin,
                   const QString& sourcePeer, qint64 originSeq, qint64 parentSeq, qint64 epoch,
                   qint64 schemaVer, const QString& schemaFp, const QByteArray& changeset,
                   bool authoritative, qint64* localSeqOut, QString* err,
                   const QString& pushId = QString());  // M-04 fix

    // Compute SHA-256 hex of a changeset blob (for payload_checksum).
    static QString blobChecksum(const QByteArray& data);
};

}  // namespace dbridge::sync
