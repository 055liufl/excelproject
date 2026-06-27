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

    // Runtime session exercise: create a short-lived session on h, attach syncTables,
    // and collect an empty changeset. Verifies that PREUPDATE hooks actually work with
    // the linked SQLite symbols — not just the compile-option flag. H-03 fix.
    static bool exerciseSession(sqlite3* h, const QStringList& tables, QString* err);

    // Return SQLITE library version string for logging.
    static const char* libVersion();
};

}  // namespace dbridge::sync
