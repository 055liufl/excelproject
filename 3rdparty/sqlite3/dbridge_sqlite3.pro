# Vendored SQLite3 compiled with SESSION + PREUPDATE_HOOK.
#
# Mirrors the `add_library(dbridge_sqlite3 STATIC ${QT_SQLITE_SRC})` block in
# CMakeLists.txt. The amalgamation source lives in the Qt source tree (outside
# this repo); override QT_SQLITE_SRC / QT_SQLITE_INC via qmake args if needed.

TEMPLATE = lib
CONFIG  += staticlib
CONFIG  -= qt              # pure C third-party code, no Qt needed
TARGET   = dbridge_sqlite3

include($$PWD/../../qsqlite3.pri)

!exists($$QT_SQLITE_SRC) {
    error("dbridge_sqlite3: sqlite3.c not found at '$$QT_SQLITE_SRC'. \
Pass QT_SQLITE_SRC=/path/to/sqlite3.c (and QT_SQLITE_INC=/dir) to qmake.")
}

SOURCES     += $$QT_SQLITE_SRC
INCLUDEPATH += $$QT_SQLITE_INC

DEFINES += $$DBRIDGE_SQLITE_DEFINES

# Position-independent: this archive is linked into PIE executables (cli, tests).
# Mirrors set_target_properties(... POSITION_INDEPENDENT_CODE ON).
QMAKE_CFLAGS += -fPIC
# Silence strict warnings from third-party code (mirrors target_compile_options ... -w).
QMAKE_CFLAGS += -w

CONFIG += skip_target_version_ext
