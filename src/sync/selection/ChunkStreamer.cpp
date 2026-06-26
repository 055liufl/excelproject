#include "ChunkStreamer.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>

namespace dbridge::sync {

// Convert an Entry's row map to a stable FrozenEntry.
static FrozenEntry entryToFrozen(const FkClosureBuilder::Entry& e) {
    // pkHash: SHA-1 of "table\x1fpk"
    const QByteArray hashSrc = e.table.toUtf8() + '\x1f' + e.pk.toUtf8();
    const QString pkHash =
        QString::fromLatin1(QCryptographicHash::hash(hashSrc, QCryptographicHash::Sha1).toHex());

    FrozenEntry fe;
    fe.table = e.table;
    fe.primaryKey = e.pk;
    fe.pkHash = pkHash;
    fe.recordKind = e.isSelected ? QStringLiteral("selected") : QStringLiteral("dependency");
    fe.topoIndex = e.topoIndex;
    fe.fingerprint = QByteArray();  // populated later if needed
    return fe;
}

// Estimate byte size of a single row (rough, avoids full JSON serialization per row).
static qint64 estimateRowBytes(const QVariantMap& row) {
    qint64 est = 0;
    for (auto it = row.constBegin(); it != row.constEnd(); ++it) {
        est += it.key().size() * 2 + 4;               // key overhead
        est += it.value().toString().size() * 2 + 4;  // value overhead
    }
    return qMax(est, static_cast<qint64>(64));  // floor
}

bool ChunkStreamer::stream(const QList<FkClosureBuilder::Entry>& manifest, const QString& origin,
                           const QString& targetPeer, qint64 chunkBudgetBytes, PayloadCodec& codec,
                           QList<Chunk>* chunks, QString* err) {
    Q_UNUSED(codec)  // used later if wire-encoding is needed here
    Q_UNUSED(targetPeer)
    Q_UNUSED(err)

    if (manifest.isEmpty())
        return true;

    const QString pushId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    // First pass: assign entries to chunks by byte budget.
    struct RawChunk {
        QList<FrozenEntry> entries;
        QList<QVariantMap> rows;
        qint64 estimatedBytes = 0;
    };

    QList<RawChunk> raw;
    raw.append(RawChunk{});

    for (const auto& e : manifest) {
        const qint64 rowEst = estimateRowBytes(e.row);

        // H-04 fix: a single row that exceeds the chunk budget on its own cannot be split further.
        // Emit E_SYNC_SELECTION_TOO_LARGE so the caller can surface a clear error rather than
        // silently producing an oversized chunk that may violate transport assumptions.
        if (chunkBudgetBytes > 0 && rowEst > chunkBudgetBytes) {
            if (err)
                *err = QStringLiteral(
                           "E_SYNC_SELECTION_TOO_LARGE: single row in table '%1' "
                           "estimated at %2 bytes exceeds chunk budget %3 bytes")
                           .arg(e.table)
                           .arg(rowEst)
                           .arg(chunkBudgetBytes);
            return false;
        }

        // If adding this entry would overflow the current chunk, start a new one.
        if (!raw.last().entries.isEmpty() &&
            raw.last().estimatedBytes + rowEst > chunkBudgetBytes) {
            raw.append(RawChunk{});
        }

        raw.last().entries.append(entryToFrozen(e));
        raw.last().rows.append(e.row);
        raw.last().estimatedBytes += rowEst;
    }

    const int total = raw.size();
    chunks->reserve(total);

    for (int i = 0; i < total; ++i) {
        Chunk c;
        c.pushId = pushId;
        c.chunkSeq = i;
        c.totalChunks = total;
        c.entries = std::move(raw[i].entries);
        c.rows = std::move(raw[i].rows);
        chunks->append(std::move(c));
    }

    return true;
}

}  // namespace dbridge::sync
