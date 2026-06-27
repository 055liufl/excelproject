#include "SqliteHandle.h"

#include <QSqlDriver>
#include <QStringList>
#include <QVariant>

namespace dbridge::sync {

sqlite3* SqliteHandle::of(QSqlDatabase& db) {
    QVariant v = db.driver()->handle();
    if (!v.isValid())
        return nullptr;
    return *static_cast<sqlite3**>(v.data());
}

bool SqliteHandle::sessionAvailable(sqlite3* h) {
    if (!h)
        return false;
    // sqlite3_compileoption_used is always available regardless of build flags.
    return sqlite3_compileoption_used("ENABLE_SESSION") &&
           sqlite3_compileoption_used("ENABLE_PREUPDATE_HOOK");
}

const char* SqliteHandle::libVersion() {
    return sqlite3_libversion();
}

bool SqliteHandle::exerciseSession(sqlite3* h, const QStringList& tables, QString* err) {
    if (!h) {
        if (err)
            *err = QStringLiteral("null sqlite3 handle");
        return false;
    }
    sqlite3_session* s = nullptr;
    int rc = sqlite3session_create(h, "main", &s);
    if (rc != SQLITE_OK) {
        if (err)
            *err = QStringLiteral("sqlite3session_create failed: rc=%1").arg(rc);
        return false;
    }
    for (const QString& table : tables) {
        const QByteArray name = table.toUtf8();
        rc = sqlite3session_attach(s, name.constData());
        if (rc != SQLITE_OK) {
            sqlite3session_delete(s);
            if (err)
                *err = QStringLiteral("sqlite3session_attach failed for '%1': rc=%2")
                           .arg(table)
                           .arg(rc);
            return false;
        }
    }
    int n = 0;
    void* p = nullptr;
    rc = sqlite3session_changeset(s, &n, &p);
    sqlite3_free(p);
    sqlite3session_delete(s);
    if (rc != SQLITE_OK) {
        if (err)
            *err = QStringLiteral("sqlite3session_changeset self-check failed: rc=%1").arg(rc);
        return false;
    }
    return true;
}

}  // namespace dbridge::sync
