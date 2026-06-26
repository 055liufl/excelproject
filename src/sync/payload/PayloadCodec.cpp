#include "PayloadCodec.h"

#include <QBuffer>
#include <QDataStream>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace dbridge::sync {

// ---------------------------------------------------------------------------
// private helpers
// ---------------------------------------------------------------------------

void PayloadCodec::writeHeader(QDataStream& ds, const PayloadHeader& h) {
    ds << h.origin << h.originSeq << h.parentSeq << h.schemaFingerprint << h.schemaVer
       << h.streamEpoch << h.routeTag << h.pushId << static_cast<qint32>(h.chunkSeq)
       << static_cast<qint32>(h.totalChunks)
       << h.senderPeer;  // C-05 fix: physical sender so receivers can ACK back to the right node
}

bool PayloadCodec::readHeader(QDataStream& ds, PayloadHeader* h, QString* err) {
    qint32 cs = 0, tc = 0;
    ds >> h->origin >> h->originSeq >> h->parentSeq >> h->schemaFingerprint >> h->schemaVer >>
        h->streamEpoch >> h->routeTag >> h->pushId >> cs >> tc;
    h->chunkSeq = cs;
    h->totalChunks = tc;
    // C-05 fix: senderPeer was added after the original fields; read it gracefully.
    // If the stream is already at end (old payload), senderPeer remains empty — that is fine
    // because old payloads always had origin == sender (no forwarding).
    if (ds.status() == QDataStream::Ok)
        ds >> h->senderPeer;
    if (ds.status() != QDataStream::Ok && ds.status() != QDataStream::ReadPastEnd) {
        if (err)
            *err = QStringLiteral("payload header read error");
        return false;
    }
    // Reset status so callers see Ok after optional field read.
    if (ds.status() == QDataStream::ReadPastEnd) {
        // Tolerate old payloads that lack senderPeer.
        h->senderPeer.clear();
    }
    return true;
}

// ---------------------------------------------------------------------------
// public: encode
// ---------------------------------------------------------------------------

QByteArray PayloadCodec::encodeChangeset(const PayloadHeader& h, const QByteArray& changeset) {
    QByteArray buf;
    QDataStream ds(&buf, QIODevice::WriteOnly);
    ds.setVersion(QDataStream::Qt_5_12);
    ds << kMagic << kVersion << static_cast<quint8>(KChangeset);
    writeHeader(ds, h);
    ds << qCompress(changeset);
    return buf;
}

QByteArray PayloadCodec::encodeSelectionPush(const PayloadHeader& h,
                                             const SelectionPushBody& body) {
    // Serialise rows as a JSON array for simplicity / human-readability.
    QJsonArray rowsArr;
    for (const QVariantMap& row : body.rows) {
        rowsArr.append(QJsonObject::fromVariantMap(row));
    }
    QJsonArray frozenArr;
    for (const FrozenEntry& fe : body.frozenEntries) {
        QJsonObject o;
        o[QStringLiteral("table")] = fe.table;
        o[QStringLiteral("primaryKey")] = fe.primaryKey;
        o[QStringLiteral("pkHash")] = fe.pkHash;
        o[QStringLiteral("recordKind")] = fe.recordKind;
        o[QStringLiteral("topoIndex")] = fe.topoIndex;
        o[QStringLiteral("fingerprint")] = QString::fromLatin1(fe.fingerprint.toBase64());
        frozenArr.append(o);
    }
    QJsonObject root;
    root[QStringLiteral("frozen")] = frozenArr;
    root[QStringLiteral("rows")] = rowsArr;
    root[QStringLiteral("pushId")] = body.pushId;
    root[QStringLiteral("chunkSeq")] = body.chunkSeq;
    root[QStringLiteral("totalChunks")] = body.totalChunks;
    QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);

    QByteArray buf;
    QDataStream ds(&buf, QIODevice::WriteOnly);
    ds.setVersion(QDataStream::Qt_5_12);
    ds << kMagic << kVersion << static_cast<quint8>(KSelectionPush);
    writeHeader(ds, h);
    ds << qCompress(json);
    return buf;
}

QByteArray PayloadCodec::encodeBaselineRequest(const PayloadHeader& h,
                                               const BaselineRequestPayload& body) {
    QJsonArray tablesArr;
    for (const QString& table : body.requestedTables)
        tablesArr.append(table);

    QJsonObject root;
    root[QStringLiteral("origin")] = body.origin;
    root[QStringLiteral("streamEpoch")] = QString::number(body.streamEpoch);
    root[QStringLiteral("requestedTables")] = tablesArr;
    root[QStringLiteral("fromSeq")] = QString::number(body.fromSeq);
    root[QStringLiteral("pendingArtifactName")] = body.pendingArtifactName;
    QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);

    QByteArray buf;
    QDataStream ds(&buf, QIODevice::WriteOnly);
    ds.setVersion(QDataStream::Qt_5_12);
    ds << kMagic << kVersion << static_cast<quint8>(KBaselineRequest);
    writeHeader(ds, h);
    ds << qCompress(json);
    return buf;
}

QByteArray PayloadCodec::encodeBaselineResponse(const PayloadHeader& h,
                                                const BaselineResponsePayload& body) {
    QJsonArray tablesArr;
    for (const QString& table : body.tables)
        tablesArr.append(table);

    // C-03 fix: serialize per-origin applied-vector cuts (origin + stream_epoch + appliedSeq).
    // Format: JSON array of {"o": origin, "e": epoch, "s": appliedSeq} objects.
    // The old "originMaxSeq" key (QHash without epoch) is no longer written.
    QJsonArray originCutsArr;
    for (const BaselineOriginCut& cut : body.originCuts) {
        QJsonObject obj;
        obj[QStringLiteral("o")] = cut.origin;
        obj[QStringLiteral("e")] = QString::number(cut.streamEpoch);
        obj[QStringLiteral("s")] = QString::number(cut.appliedSeq);
        originCutsArr.append(obj);
    }

    QJsonObject root;
    root[QStringLiteral("origin")] = body.origin;
    root[QStringLiteral("requestOrigin")] = body.requestOrigin;
    root[QStringLiteral("streamEpoch")] = QString::number(body.streamEpoch);
    root[QStringLiteral("tables")] = tablesArr;
    root[QStringLiteral("fromSeq")] = QString::number(body.fromSeq);
    root[QStringLiteral("pendingArtifactName")] = body.pendingArtifactName;
    root[QStringLiteral("baselineData")] = QString::fromLatin1(body.baselineData.toBase64());
    root[QStringLiteral("sourceMaxSeq")] = QString::number(body.sourceMaxSeq);
    root[QStringLiteral("originCuts")] = originCutsArr;
    QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);

    QByteArray buf;
    QDataStream ds(&buf, QIODevice::WriteOnly);
    ds.setVersion(QDataStream::Qt_5_12);
    ds << kMagic << kVersion << static_cast<quint8>(KBaselineResponse);
    writeHeader(ds, h);
    ds << qCompress(json);
    return buf;
}

// ---------------------------------------------------------------------------
// public: decode
// ---------------------------------------------------------------------------

bool PayloadCodec::decode(const QByteArray& data, DecodeResult* out, QString* err) {
    QDataStream ds(data);
    ds.setVersion(QDataStream::Qt_5_12);

    quint32 magic = 0;
    quint16 ver = 0;
    quint8 kind = 0;
    ds >> magic >> ver >> kind;
    if (magic != kMagic) {
        if (err)
            *err = QStringLiteral("bad magic number");
        return false;
    }
    if (ver != kVersion) {
        if (err)
            *err = QStringLiteral("unsupported codec version %1").arg(ver);
        return false;
    }
    if (!readHeader(ds, &out->header, err))
        return false;

    QByteArray compressed;
    ds >> compressed;
    if (ds.status() != QDataStream::Ok) {
        if (err)
            *err = QStringLiteral("payload body read error");
        return false;
    }
    QByteArray body = qUncompress(compressed);
    if (body.isEmpty() && !compressed.isEmpty()) {
        if (err)
            *err = QStringLiteral("payload decompression failed");
        return false;
    }

    if (kind == KChangeset) {
        out->kind = PayloadKind::Changeset;
        out->changeset = body;
        return true;
    }
    if (kind == KSelectionPush) {
        out->kind = PayloadKind::SelectionPush;
        QJsonParseError pe;
        QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
        if (doc.isNull()) {
            if (err)
                *err = QStringLiteral("selection JSON: %1").arg(pe.errorString());
            return false;
        }
        QJsonObject root = doc.object();
        SelectionPushBody& sb = out->selection;
        sb.pushId = root[QStringLiteral("pushId")].toString();
        sb.chunkSeq = root[QStringLiteral("chunkSeq")].toInt();
        sb.totalChunks = root[QStringLiteral("totalChunks")].toInt();
        for (const QJsonValue& rv : root[QStringLiteral("rows")].toArray())
            sb.rows.append(rv.toObject().toVariantMap());
        for (const QJsonValue& fv : root[QStringLiteral("frozen")].toArray()) {
            QJsonObject fo = fv.toObject();
            FrozenEntry fe;
            fe.table = fo[QStringLiteral("table")].toString();
            fe.primaryKey = fo[QStringLiteral("primaryKey")].toString();
            fe.pkHash = fo[QStringLiteral("pkHash")].toString();
            fe.recordKind = fo[QStringLiteral("recordKind")].toString();
            fe.topoIndex = fo[QStringLiteral("topoIndex")].toInt();
            fe.fingerprint =
                QByteArray::fromBase64(fo[QStringLiteral("fingerprint")].toString().toLatin1());
            sb.frozenEntries.append(fe);
        }
        return true;
    }
    if (kind == KBaselineRequest) {
        out->kind = PayloadKind::BaselineRequest;
        QJsonParseError pe;
        QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
        if (doc.isNull()) {
            if (err)
                *err = QStringLiteral("baseline request JSON: %1").arg(pe.errorString());
            return false;
        }
        QJsonObject root = doc.object();
        BaselineRequestPayload& br = out->baselineRequest;
        br.origin = root[QStringLiteral("origin")].toString();
        br.streamEpoch = root[QStringLiteral("streamEpoch")].toString().toLongLong();
        br.fromSeq = root[QStringLiteral("fromSeq")].toString().toLongLong();
        br.pendingArtifactName = root[QStringLiteral("pendingArtifactName")].toString();
        for (const QJsonValue& tv : root[QStringLiteral("requestedTables")].toArray())
            br.requestedTables.append(tv.toString());
        return true;
    }
    if (kind == KBaselineResponse) {
        out->kind = PayloadKind::BaselineResponse;
        QJsonParseError pe;
        QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
        if (doc.isNull()) {
            if (err)
                *err = QStringLiteral("baseline response JSON: %1").arg(pe.errorString());
            return false;
        }
        QJsonObject root = doc.object();
        BaselineResponsePayload& br = out->baselineResponse;
        br.origin = root[QStringLiteral("origin")].toString();
        br.requestOrigin = root[QStringLiteral("requestOrigin")].toString();
        br.streamEpoch = root[QStringLiteral("streamEpoch")].toString().toLongLong();
        br.fromSeq = root[QStringLiteral("fromSeq")].toString().toLongLong();
        br.pendingArtifactName = root[QStringLiteral("pendingArtifactName")].toString();
        br.baselineData =
            QByteArray::fromBase64(root[QStringLiteral("baselineData")].toString().toLatin1());
        br.sourceMaxSeq = root[QStringLiteral("sourceMaxSeq")].toString().toLongLong();
        for (const QJsonValue& tv : root[QStringLiteral("tables")].toArray())
            br.tables.append(tv.toString());
        // C-03 fix: deserialize per-origin applied-vector cuts (includes stream_epoch).
        for (const QJsonValue& cv : root[QStringLiteral("originCuts")].toArray()) {
            const QJsonObject obj = cv.toObject();
            BaselineOriginCut cut;
            cut.origin = obj[QStringLiteral("o")].toString();
            cut.streamEpoch = obj[QStringLiteral("e")].toString().toLongLong();
            cut.appliedSeq = obj[QStringLiteral("s")].toString().toLongLong();
            br.originCuts.append(cut);
        }
        return true;
    }
    if (err)
        *err = QStringLiteral("unknown payload kind %1").arg(kind);
    return false;
}

// ---------------------------------------------------------------------------
// ACK encode/decode
// ---------------------------------------------------------------------------

QByteArray PayloadCodec::encodeChangesetAck(const ChangesetAck& ack) {
    QByteArray buf;
    QDataStream ds(&buf, QIODevice::WriteOnly);
    ds.setVersion(QDataStream::Qt_5_12);
    // M-09 fix: include toPeer so the ACK artifact is self-describing (not file-name-only routed).
    ds << kMagic << kVersion << static_cast<quint8>(KChangesetAck) << ack.origin << ack.streamEpoch
       << ack.appliedSeq << ack.toPeer;
    return buf;
}

QByteArray PayloadCodec::encodeChunkAck(const PushChunkAck& ack) {
    QByteArray buf;
    QDataStream ds(&buf, QIODevice::WriteOnly);
    ds.setVersion(QDataStream::Qt_5_12);
    // M-09 fix: include toPeer so the chunk ACK is self-describing.
    ds << kMagic << kVersion << static_cast<quint8>(KChunkAck) << ack.pushId
       << static_cast<qint32>(ack.chunkSeq) << static_cast<qint32>(ack.totalChunks) << ack.checksum
       << ack.ok << ack.toPeer;
    return buf;
}

bool PayloadCodec::decodeChangesetAck(const QByteArray& data, ChangesetAck* out) {
    QDataStream ds(data);
    ds.setVersion(QDataStream::Qt_5_12);
    quint32 magic = 0;
    quint16 ver = 0;
    quint8 kind = 0;
    ds >> magic >> ver >> kind;
    if (magic != kMagic || kind != KChangesetAck)
        return false;
    ds >> out->origin >> out->streamEpoch >> out->appliedSeq >> out->toPeer;
    return ds.status() == QDataStream::Ok;
}

bool PayloadCodec::decodeChunkAck(const QByteArray& data, PushChunkAck* out) {
    QDataStream ds(data);
    ds.setVersion(QDataStream::Qt_5_12);
    quint32 magic = 0;
    quint16 ver = 0;
    quint8 kind = 0;
    ds >> magic >> ver >> kind;
    if (magic != kMagic || kind != KChunkAck)
        return false;
    qint32 cs = 0, tc = 0;
    ds >> out->pushId >> cs >> tc >> out->checksum >> out->ok >> out->toPeer;
    out->chunkSeq = cs;
    out->totalChunks = tc;
    return ds.status() == QDataStream::Ok;
}

}  // namespace dbridge::sync
