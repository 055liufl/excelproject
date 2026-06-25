#include "sync/conflict/RebaseEngine.h"

#include <sqlite3.h>

namespace dbridge::sync {

bool RebaseEngine::rebase(const QByteArray& rebaseBuffer, const QByteArray& changeset,
                          QByteArray* rebased, QString* err) {
    sqlite3_rebaser* pRebaser = nullptr;
    int rc = sqlite3rebaser_create(&pRebaser);
    if (rc != SQLITE_OK) {
        if (err)
            *err = "sqlite3rebaser_create failed: " + QString::number(rc);
        return false;
    }

    rc = sqlite3rebaser_configure(pRebaser, rebaseBuffer.size(), rebaseBuffer.constData());
    if (rc != SQLITE_OK) {
        sqlite3rebaser_delete(pRebaser);
        if (err)
            *err = "sqlite3rebaser_configure failed: " + QString::number(rc);
        return false;
    }

    int nOut = 0;
    void* pOut = nullptr;
    rc = sqlite3rebaser_rebase(pRebaser, changeset.size(), changeset.constData(), &nOut, &pOut);
    sqlite3rebaser_delete(pRebaser);
    if (rc != SQLITE_OK) {
        if (err)
            *err = "sqlite3rebaser_rebase failed: " + QString::number(rc);
        return false;
    }

    *rebased = QByteArray(static_cast<const char*>(pOut), nOut);
    sqlite3_free(pOut);
    return true;
}

}  // namespace dbridge::sync
