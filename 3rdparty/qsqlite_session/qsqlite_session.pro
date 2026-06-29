# Custom QSQLITE plugin rebuilt with SQLITE_ENABLE_SESSION + SQLITE_ENABLE_PREUPDATE_HOOK.
#
# Problem: Qt's pre-built libqsqlite.so embeds SQLite compiled WITHOUT the
# preupdate-hook flag.  The dbridge sync subsystem compiles sqlite3.c WITH that
# flag (via dbridge_sqlite3.a), which adds three pointer fields to the sqlite3
# struct.  Any call from dbridge_sqlite3 code into a sqlite3* handle allocated
# by the vanilla QSQLITE plugin therefore reads fields at wrong offsets → SIGSEGV
# in sqlite3ErrorFinish / sqlite3_exec.
#
# Fix: rebuild Qt's own QSQLITE plugin source against the same SQLite source
# (Qt's bundled 3.36.0) with the same SQLITE_ENABLE_SESSION +
# SQLITE_ENABLE_PREUPDATE_HOOK flags so both libraries share one struct layout.
#
# This plugin is deployed to sqldrivers/ next to each dbridge executable by a
# QMAKE_POST_LINK step in sync-demo.pro / diff-demo.pro / dbridge-cli.pro.
# The executable's main() calls QCoreApplication::addLibraryPath(appDirPath())
# to ensure this local copy is found before the system-wide Qt plugin.

TEMPLATE = lib
CONFIG  += plugin c++17
CONFIG  -= app_bundle
TARGET   = qsqlite

QT = sql sql-private core-private

PLUGIN_TYPE       = sqldrivers
PLUGIN_CLASS_NAME = QSQLiteDriverPlugin

include($$PWD/../../qsqlite3.pri)

# ── SQLite compile-time flags ────────────────────────────────────────────────
# Mirror the flags from Qt 5.12's own sqlite.pro, PLUS the session/preupdate
# flags from qsqlite3.pri (DBRIDGE_SQLITE_DEFINES).
DEFINES += \
    SQLITE_ENABLE_COLUMN_METADATA \
    SQLITE_OMIT_LOAD_EXTENSION \
    SQLITE_OMIT_COMPLETE \
    SQLITE_ENABLE_FTS3 \
    SQLITE_ENABLE_FTS3_PARENTHESIS \
    SQLITE_ENABLE_FTS5 \
    SQLITE_ENABLE_RTREE \
    HAVE_USLEEP=1 \
    $$DBRIDGE_SQLITE_DEFINES

# ── Source / include paths ───────────────────────────────────────────────────
QT_SQLITE_PLUGIN_SRC = /opt/Qt5.12.12/5.12.12/Src/qtbase/src/plugins/sqldrivers/sqlite

!exists($$QT_SQLITE_PLUGIN_SRC/qsql_sqlite.cpp) {
    error("qsqlite_session: Qt QSQLITE plugin source not found at '$$QT_SQLITE_PLUGIN_SRC'. \
           Check that Qt 5.12 sources are installed.")
}

HEADERS += $$QT_SQLITE_PLUGIN_SRC/qsql_sqlite_p.h

SOURCES += \
    $$QT_SQLITE_PLUGIN_SRC/qsql_sqlite.cpp \
    $$QT_SQLITE_PLUGIN_SRC/smain.cpp \
    $$QT_SQLITE_SRC

INCLUDEPATH += \
    $$QT_SQLITE_INC \
    $$QT_SQLITE_PLUGIN_SRC

# Silence all warnings from third-party / Qt-internal code.
QMAKE_CFLAGS   += -w
QMAKE_CXXFLAGS += -w

CONFIG += skip_target_version_ext
