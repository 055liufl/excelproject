#include "OutboxWriter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

#include <fcntl.h>
#include <unistd.h>

namespace dbridge::sync {

OutboxWriter::OutboxWriter(const QString& outboxDir) : dir_(outboxDir) {
}

bool OutboxWriter::write(const QString& artifactName, const QByteArray& data, QString* err) {
    return writeAtomic(artifactName, data, err);
}

bool OutboxWriter::writeAck(const QString& ackName, const QByteArray& data, QString* err) {
    return writeAtomic(ackName, data, err);
}

bool OutboxWriter::writeAtomic(const QString& finalName, const QByteArray& data, QString* err) {
    QDir d(dir_);
    if (!d.exists()) {
        if (!d.mkpath(QStringLiteral("."))) {
            if (err)
                *err = QStringLiteral("cannot create outbox dir: %1").arg(dir_);
            return false;
        }
    }

    const QString finalPath = d.filePath(finalName);
    const QString tmpPath = finalPath + QStringLiteral(".tmp");

    // 1. Write to .tmp
    {
        QFile f(tmpPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (err)
                *err = QStringLiteral("cannot open tmp file: %1").arg(f.errorString());
            return false;
        }
        if (f.write(data) != data.size()) {
            if (err)
                *err = QStringLiteral("partial write to tmp: %1").arg(f.errorString());
            f.remove();
            return false;
        }
        // 2. flush + fsync
        if (!f.flush()) {
            if (err)
                *err = QStringLiteral("flush failed: %1").arg(f.errorString());
            f.remove();
            return false;
        }
        int fd = static_cast<int>(f.handle());
        if (fd >= 0)
            ::fsync(fd);
        f.close();
    }

    // 3. Atomic rename (tmp → final)
    if (QFile::exists(finalPath))
        QFile::remove(finalPath);
    if (!QFile::rename(tmpPath, finalPath)) {
        if (err)
            *err = QStringLiteral("rename failed: %1 → %2").arg(tmpPath, finalPath);
        QFile::remove(tmpPath);
        return false;
    }

    // 4. Write .ready marker (empty file signals receiver)
    const QString readyPath = finalPath + QStringLiteral(".ready");
    QFile rf(readyPath);
    if (!rf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err)
            *err = QStringLiteral("cannot write .ready: %1").arg(rf.errorString());
        return false;
    }
    rf.close();
    return true;
}

}  // namespace dbridge::sync
