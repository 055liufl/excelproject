# sync-demo: dbridge 多节点增量同步范式演示

TEMPLATE = app
CONFIG  += c++17 console
CONFIG  -= app_bundle
TARGET   = sync-demo

QT = core gui sql

DEFINES += DBRIDGE_STATIC_DEFINE
DEFINES += SQLITE_ENABLE_SESSION SQLITE_ENABLE_PREUPDATE_HOOK

ROOT_SRC   = $$PWD/../..
ROOT_BUILD = $$shadowed($$ROOT_SRC)

INCLUDEPATH += \
    $$ROOT_SRC/include \
    $$ROOT_BUILD/include \
    /opt/Qt5.12.12/5.12.12/Src/qtbase/src/3rdparty/sqlite

LIBS += -L$$ROOT_BUILD/src           -ldbridge
LIBS += -L$$ROOT_BUILD/3rdparty/QXlsx -lQXlsx
LIBS += -L$$ROOT_BUILD/3rdparty/sqlite3 -ldbridge_sqlite3
LIBS += -lpthread -ldl

PRE_TARGETDEPS += \
    $$ROOT_BUILD/src/libdbridge.a \
    $$ROOT_BUILD/3rdparty/sqlite3/libdbridge_sqlite3.a

SOURCES = main.cpp
