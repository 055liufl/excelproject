#include "SyncContext.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QUuid>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace dbridge::sync {

SyncContextRegistry& SyncContextRegistry::instance() {
    static SyncContextRegistry reg;
    return reg;
}

QString SyncContextRegistry::canonicalKey(const QString& path, QString* err) {
    if (path.isEmpty()) {
        if (err)
            *err = QStringLiteral("empty path");
        return {};
    }

#ifdef Q_OS_WIN
    HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(path.utf16()), 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        // File doesn't exist yet — fall back to normalised path as temp key.
        return QFileInfo(path).absoluteFilePath().toLower();
    }
    BY_HANDLE_FILE_INFORMATION info;
    bool ok = GetFileInformationByHandle(h, &info);
    CloseHandle(h);
    if (!ok) {
        return QFileInfo(path).absoluteFilePath().toLower();
    }
    return QStringLiteral("win:%1:%2")
        .arg(info.dwVolumeSerialNumber)
        .arg(((quint64)info.nFileIndexHigh << 32) | info.nFileIndexLow);
#else
    struct stat st;
    if (::stat(path.toLocal8Bit().constData(), &st) != 0) {
        // File doesn't exist yet — use normalised path as temporary key.
        return QFileInfo(path).absoluteFilePath();
    }
    return QStringLiteral("posix:%1:%2").arg((quint64)st.st_dev).arg((quint64)st.st_ino);
#endif
}

std::shared_ptr<SyncContext> SyncContextRegistry::getOrCreate(const QString& sqlitePath,
                                                              QString* canonicalKeyOut,
                                                              QString* err) {
    QString key = canonicalKey(sqlitePath, err);
    if (key.isEmpty())
        return nullptr;

    if (canonicalKeyOut)
        *canonicalKeyOut = key;

    QMutexLocker lk(&mutex_);
    auto it = registry_.find(key);
    if (it != registry_.end()) {
        it.value()->refCount++;
        return it.value();
    }

    auto ctx = std::make_shared<SyncContext>();
    ctx->refCount = 1;
    ctx->contextUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    registry_.insert(key, ctx);
    return ctx;
}

void SyncContextRegistry::release(const QString& canonicalKey_) {
    QMutexLocker lk(&mutex_);
    auto it = registry_.find(canonicalKey_);
    if (it == registry_.end())
        return;
    if (--it.value()->refCount <= 0) {
        registry_.erase(it);
    }
}

}  // namespace dbridge::sync
