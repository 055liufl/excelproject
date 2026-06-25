#pragma once
#include <QSqlDatabase>

#include <sqlite3.h>

namespace dbridge::sync {

class SqliteHandle {
   public:
    // Extract sqlite3* from a QSqlDatabase (must be called on db's owning thread).
    static sqlite3* of(QSqlDatabase& db);

    // Runtime check: SQLITE_ENABLE_SESSION + SQLITE_ENABLE_PREUPDATE_HOOK both compiled in.
    static bool sessionAvailable(sqlite3* h);

    // Return SQLITE library version string for logging.
    static const char* libVersion();
};

}  // namespace dbridge::sync
