# Shared SQLite-with-SESSION configuration for the qmake build.
#
# The SQLite 同步工具 subsystem (src/sync/**) needs the SQLite *session* and
# *rebaser* APIs, which Qt's bundled SQLite does NOT expose by default. The CMake
# build compiles Qt's sqlite3.c amalgamation into a dedicated static lib
# (dbridge_sqlite3) with SQLITE_ENABLE_SESSION + SQLITE_ENABLE_PREUPDATE_HOOK.
# This .pri mirrors the two CMake cache variables so every qmake sub-project
# resolves the same source / header / flags.
#
# Override on the command line if your Qt is installed elsewhere, e.g.:
#   qmake QT_SQLITE_SRC=/path/to/sqlite3.c QT_SQLITE_INC=/path/to/dir ../dbridge.pro

isEmpty(QT_SQLITE_SRC) {
    QT_SQLITE_SRC = /opt/Qt5.12.12/5.12.12/Src/qtbase/src/3rdparty/sqlite/sqlite3.c
}
isEmpty(QT_SQLITE_INC) {
    QT_SQLITE_INC = /opt/Qt5.12.12/5.12.12/Src/qtbase/src/3rdparty/sqlite
}

# The two macros that turn on the session/preupdate/rebaser surface. Consumers
# that compile sqlite3.c OR include <sqlite3.h> for session use must define both.
DBRIDGE_SQLITE_DEFINES = SQLITE_ENABLE_SESSION SQLITE_ENABLE_PREUPDATE_HOOK
