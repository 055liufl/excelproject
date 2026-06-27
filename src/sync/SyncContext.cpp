#include "SyncContext.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
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
        // M-04 fix: database file must exist before we can obtain an OS file identity.
        // A path-string fallback could let two aliases create separate SyncContexts,
        // breaking the single-writer guarantee (design §4.3/G-07).
        if (err)
            *err =
                QStringLiteral("Cannot open database file for identity resolution: %1").arg(path);
        return {};
    }
    BY_HANDLE_FILE_INFORMATION info;
    bool ok = GetFileInformationByHandle(h, &info);
    CloseHandle(h);
    if (!ok) {
        if (err)
            *err = QStringLiteral("GetFileInformationByHandle failed for: %1").arg(path);
        return {};
    }
    return QStringLiteral("win:%1:%2")
        .arg(info.dwVolumeSerialNumber)
        .arg(((quint64)info.nFileIndexHigh << 32) | info.nFileIndexLow);
#else
    struct stat st;
    if (::stat(path.toLocal8Bit().constData(), &st) != 0) {
        // M-04 fix: no path fallback — require the file to exist so OS inode is the key.
        if (err)
            *err = QStringLiteral("Database file not found or not accessible: %1 (errno=%2)")
                       .arg(path)
                       .arg(errno);
        return {};
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

std::shared_ptr<SyncContext> SyncContextRegistry::getExisting(const QString& sqlitePath) {
    // J-10: Look up an existing context without creating one and without touching refCount.
    QString key = canonicalKey(sqlitePath, nullptr);
    if (key.isEmpty())
        return nullptr;

    QMutexLocker lk(&mutex_);
    auto it = registry_.find(key);
    if (it == registry_.end())
        return nullptr;
    // Return a copy of the shared_ptr — shared ownership keeps the object alive for the
    // duration of the caller's inspection, but we do NOT increment refCount (the internal
    // counter used by release()). The caller must not call release() for this pointer.
    return it.value();
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

bool SyncContextRegistry::ensureContextUuid(QSqlDatabase& db, QString* uuid, QString* err) {
    // H-01 fix: read-or-write the context UUID in __sync_context_meta.
    // On restart the in-memory UUID is freshly generated; if the DB already holds a UUID
    // from a previous run, we adopt that stored value so restart works correctly.
    if (!uuid) {
        if (err)
            *err = QStringLiteral("uuid pointer is null");
        return false;
    }

    QSqlQuery ddl(db);
    if (!ddl.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS __sync_context_meta("
                                 "k TEXT NOT NULL PRIMARY KEY, v TEXT NOT NULL)"))) {
        if (err)
            *err = QStringLiteral("cannot create __sync_context_meta: ") + ddl.lastError().text();
        return false;
    }

    QSqlQuery sel(db);
    sel.prepare(QStringLiteral("SELECT v FROM __sync_context_meta WHERE k='context_uuid'"));
    if (!sel.exec()) {
        if (err)
            *err = QStringLiteral("cannot read context_uuid: ") + sel.lastError().text();
        return false;
    }
    if (sel.next()) {
        // UUID already stored — adopt the persisted value (supports restart across processes).
        *uuid = sel.value(0).toString();
        return true;
    }

    // No row yet — write the current in-memory UUID (generate one if empty).
    if (uuid->isEmpty())
        *uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);

    QSqlQuery ins(db);
    ins.prepare(QStringLiteral("INSERT INTO __sync_context_meta(k, v) VALUES('context_uuid', ?)"));
    ins.addBindValue(*uuid);
    if (!ins.exec()) {
        if (err)
            *err = QStringLiteral("cannot persist context_uuid: ") + ins.lastError().text();
        return false;
    }
    return true;
}

}  // namespace dbridge::sync
