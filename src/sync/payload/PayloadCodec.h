#pragma once
#include "dbridge/sync/SyncTypes.h"

#include <QByteArray>
#include <QString>

namespace dbridge::sync {

// Binary codec for all sync artifacts.
// Wire format: magic(4) "DBSY" + version(quint16=1) + kind(quint8) + body.
// Changeset bodies are qCompress-ed before writing.
class PayloadCodec {
   public:
    // Encode a changeset payload.  changeset is the raw SQLite changeset blob.
    QByteArray encodeChangeset(const PayloadHeader& h, const QByteArray& changeset);

    // Encode a selection-push payload.
    QByteArray encodeSelectionPush(const PayloadHeader& h, const SelectionPushBody& body);

    // Decode any payload artifact.  Returns false + *err on failure.
    bool decode(const QByteArray& data, DecodeResult* out, QString* err);

    // Encode/decode typed ACK artifacts.
    QByteArray encodeChangesetAck(const ChangesetAck& ack);
    QByteArray encodeChunkAck(const PushChunkAck& ack);
    bool decodeChangesetAck(const QByteArray& data, ChangesetAck* out);
    bool decodeChunkAck(const QByteArray& data, PushChunkAck* out);

   private:
    static constexpr quint32 kMagic = 0x44425359u;  // "DBSY"
    static constexpr quint16 kVersion = 1;

    enum KindByte : quint8 { KChangeset = 0, KSelectionPush = 1, KChangesetAck = 2, KChunkAck = 3 };

    void writeHeader(QDataStream& ds, const PayloadHeader& h);
    bool readHeader(QDataStream& ds, PayloadHeader* h, QString* err);
};

}  // namespace dbridge::sync
