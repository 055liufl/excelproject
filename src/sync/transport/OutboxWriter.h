#pragma once
#include <QByteArray>
#include <QString>

namespace dbridge::sync {

// Atomically publishes artifacts to an outbox directory.
// Protocol: write .tmp → fsync → rename to .payload or .ack → write .ready marker.
class OutboxWriter {
   public:
    explicit OutboxWriter(const QString& outboxDir);

    // Write a payload artifact.  artifactName should come from SyncDDL helpers.
    bool write(const QString& artifactName, const QByteArray& data, QString* err);

    // Write an ACK artifact.
    bool writeAck(const QString& ackName, const QByteArray& data, QString* err);

   private:
    // Shared implementation: write to tmp, fsync, rename, write .ready.
    bool writeAtomic(const QString& finalName, const QByteArray& data, QString* err);

    QString dir_;
};

}  // namespace dbridge::sync
