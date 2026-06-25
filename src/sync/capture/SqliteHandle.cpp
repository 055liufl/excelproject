#include "SqliteHandle.h"

#include <QSqlDriver>
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

}  // namespace dbridge::sync
