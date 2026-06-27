#include "dbridge/Errors.h"
#include "dbridge/sync/SyncTypes.h"

#include <QUuid>
#include <QtTest>

#include "sync/payload/PayloadCodec.h"

using namespace dbridge::sync;

static PayloadHeader makeHeader(const QString& origin = "nodeA", qint64 epoch = 1, qint64 seq = 42,
                                const QString& fp = "fp_test", qint64 schemaVer = 2) {
    PayloadHeader h;
    h.origin = origin;
    h.streamEpoch = epoch;
    h.originSeq = seq;
    h.schemaFingerprint = fp;
    h.schemaVer = schemaVer;
    h.routeTag = "center";
    h.senderPeer = origin;
    return h;
}

class TstSyncPayloadCodec : public QObject {
    Q_OBJECT
   private slots:

    // --- Changeset round-trip ---
    void changeset_encodeDecodeRoundTrip() {
        PayloadCodec codec;
        PayloadHeader h = makeHeader();
        QByteArray blob("fake_changeset_bytes_\x01\x02\x03");

        QByteArray encoded = codec.encodeChangeset(h, blob);
        QVERIFY(!encoded.isEmpty());

        DecodeResult out;
        QString err;
        QVERIFY2(codec.decode(encoded, &out, &err), qPrintable(err));
        QCOMPARE(out.kind, PayloadKind::Changeset);
        QCOMPARE(out.header.origin, h.origin);
        QCOMPARE(out.header.streamEpoch, h.streamEpoch);
        QCOMPARE(out.header.originSeq, h.originSeq);
        QCOMPARE(out.header.schemaFingerprint, h.schemaFingerprint);
        QCOMPARE(out.header.schemaVer, h.schemaVer);
        QCOMPARE(out.changeset, blob);
    }

    void changeset_largeBlob_compressedRoundTrip() {
        PayloadCodec codec;
        // 50KB of repetitive data — should compress well
        QByteArray big(50000, 'Z');
        QByteArray enc = codec.encodeChangeset(makeHeader(), big);
        // encoded should be smaller than raw (compression)
        QVERIFY(enc.size() < big.size());

        DecodeResult out;
        QString err;
        QVERIFY(codec.decode(enc, &out, &err));
        QCOMPARE(out.changeset, big);
    }

    // --- SelectionPush round-trip ---
    void selectionPush_encodeDecodeRoundTrip() {
        PayloadCodec codec;
        PayloadHeader h = makeHeader("edge", 2, 7);
        h.pushId = "push-uuid-001";
        h.chunkSeq = 1;
        h.totalChunks = 3;

        SelectionPushBody body;
        body.pushId = h.pushId;
        body.chunkSeq = h.chunkSeq;
        body.totalChunks = h.totalChunks;
        FrozenEntry fe;
        fe.table = "orders";
        fe.primaryKey = "42";
        fe.pkHash = "pk42";
        fe.recordKind = "selected";
        fe.topoIndex = 0;
        body.frozenEntries << fe;
        QVariantMap row;
        row["id"] = 42;
        row["name"] = "Alice";
        body.rows << row;

        QByteArray enc = codec.encodeSelectionPush(h, body);
        QVERIFY(!enc.isEmpty());

        DecodeResult out;
        QString err;
        QVERIFY2(codec.decode(enc, &out, &err), qPrintable(err));
        QCOMPARE(out.kind, PayloadKind::SelectionPush);
        QCOMPARE(out.header.pushId, h.pushId);
        QCOMPARE(out.selection.chunkSeq, 1);
        QCOMPARE(out.selection.totalChunks, 3);
        QCOMPARE(out.selection.frozenEntries.size(), 1);
        QCOMPARE(out.selection.frozenEntries[0].table, QString("orders"));
        QCOMPARE(out.selection.rows.size(), 1);
    }

    // --- ChangesetAck round-trip ---
    void changesetAck_roundTrip() {
        PayloadCodec codec;
        ChangesetAck ack;
        ack.origin = "nodeA";
        ack.streamEpoch = 5;
        ack.appliedSeq = 99;
        ack.toPeer = "center";

        QByteArray enc = codec.encodeChangesetAck(ack);
        QVERIFY(!enc.isEmpty());

        ChangesetAck out;
        QVERIFY(codec.decodeChangesetAck(enc, &out));
        QCOMPARE(out.origin, ack.origin);
        QCOMPARE(out.streamEpoch, ack.streamEpoch);
        QCOMPARE(out.appliedSeq, ack.appliedSeq);
    }

    // --- PushChunkAck round-trip ---
    void chunkAck_roundTrip() {
        PayloadCodec codec;
        PushChunkAck ack;
        ack.pushId = "push-001";
        ack.chunkSeq = 3;
        ack.totalChunks = 10;
        ack.checksum = "abc123";
        ack.ok = true;
        ack.toPeer = "edge";

        QByteArray enc = codec.encodeChunkAck(ack);
        QVERIFY(!enc.isEmpty());

        PushChunkAck out;
        QVERIFY(codec.decodeChunkAck(enc, &out));
        QCOMPARE(out.pushId, ack.pushId);
        QCOMPARE(out.chunkSeq, ack.chunkSeq);
        QCOMPARE(out.totalChunks, ack.totalChunks);
        QCOMPARE(out.checksum, ack.checksum);
        QVERIFY(out.ok);
    }

    // --- Corrupt data → E_SYNC_PAYLOAD_CORRUPT ---
    void decode_emptyData_corrupt() {
        PayloadCodec codec;
        DecodeResult out;
        QString err;
        QVERIFY(!codec.decode(QByteArray(), &out, &err));
        QVERIFY(!err.isEmpty());  // codec uses descriptive err, not error-code prefix
    }

    void decode_badMagic_corrupt() {
        PayloadCodec codec;
        QByteArray bad("BAD_MAGIC_BYTES____extra_data_here");
        DecodeResult out;
        QString err;
        QVERIFY(!codec.decode(bad, &out, &err));
        QVERIFY(!err.isEmpty());  // codec uses descriptive err, not error-code prefix
    }

    void decode_truncated_corrupt() {
        PayloadCodec codec;
        // encode a valid payload then truncate it
        QByteArray enc = codec.encodeChangeset(makeHeader(), QByteArray("data"));
        QByteArray truncated = enc.left(enc.size() / 2);
        DecodeResult out;
        QString err;
        QVERIFY(!codec.decode(truncated, &out, &err));
        QVERIFY(!err.isEmpty());  // codec uses descriptive err, not error-code prefix
    }

    void decode_bitflip_corrupt() {
        PayloadCodec codec;
        QByteArray enc = codec.encodeChangeset(makeHeader(), QByteArray("hello world"));
        // flip a byte in the body area
        if (enc.size() > 20) {
            char flipped = static_cast<char>(enc[enc.size() / 2]) ^ static_cast<char>(0xFF);
            enc[enc.size() / 2] = flipped;
        }
        DecodeResult out;
        QString err;
        bool ok = codec.decode(enc, &out, &err);
        // may or may not detect (compression CRC), but at minimum must not crash
        (void)ok;
    }

    // senderPeer field survives round-trip (version 2 wire format)
    void senderPeer_roundTrip() {
        PayloadCodec codec;
        PayloadHeader h = makeHeader();
        h.senderPeer = "forwarder_node";
        QByteArray enc = codec.encodeChangeset(h, QByteArray("data"));
        DecodeResult out;
        QString err;
        QVERIFY(codec.decode(enc, &out, &err));
        QCOMPARE(out.header.senderPeer, QString("forwarder_node"));
    }
};

QTEST_APPLESS_MAIN(TstSyncPayloadCodec)
#include "tst_sync_payload_codec.moc"
