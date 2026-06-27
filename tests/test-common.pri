# Shared config for all dbridge test executables.
# Each test .pro does:
#   include(../test-common.pri)
#   TARGET   = tst_xxx
#   SOURCES  = tst_xxx.cpp

TEMPLATE = app
CONFIG  += c++17 console testcase
CONFIG  -= app_bundle

QT       = core sql gui testlib

DEFINES += DBRIDGE_STATIC_DEFINE

# Compute project roots. test-common.pri lives at tests/, so:
ROOT_SRC   = $$PWD/..
ROOT_BUILD = $$shadowed($$ROOT_SRC)

# SQLite session API: many tst_sync_* sources include sync headers that pull in
# <sqlite3.h>, whose session declarations are gated on SQLITE_ENABLE_SESSION.
include($$PWD/../qsqlite3.pri)
DEFINES += $$DBRIDGE_SQLITE_DEFINES

INCLUDEPATH += \
    $$ROOT_SRC/include \
    $$ROOT_BUILD/include \
    $$ROOT_SRC/src \
    $$ROOT_SRC/3rdparty/QXlsx/header \
    $$QT_SQLITE_INC

# Link static libs (build order enforced via tests/tests.pro depends + the
# top-level dbridge.pro depends chain).
LIBS += -L$$ROOT_BUILD/src       -ldbridge
LIBS += -L$$ROOT_BUILD/3rdparty/QXlsx -lQXlsx
# sqlite3 (with SESSION) supplies sqlite3session_*/sqlite3rebaser_*/changeset_*
# referenced by the sync .o pulled from libdbridge. MUST follow -ldbridge.
LIBS += -L$$ROOT_BUILD/3rdparty/sqlite3 -ldbridge_sqlite3

PRE_TARGETDEPS += \
    $$ROOT_BUILD/src/libdbridge.a \
    $$ROOT_BUILD/3rdparty/QXlsx/libQXlsx.a \
    $$ROOT_BUILD/3rdparty/sqlite3/libdbridge_sqlite3.a
