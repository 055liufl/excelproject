# dbridge-cli example.
# Mirrors examples/cli/CMakeLists.txt.

TEMPLATE = app
CONFIG  += c++17 console
CONFIG  -= app_bundle
TARGET   = dbridge-cli

QT       = core sql gui

DEFINES += DBRIDGE_STATIC_DEFINE

ROOT_SRC   = $$PWD/../..
ROOT_BUILD = $$shadowed($$ROOT_SRC)

INCLUDEPATH += \
    $$ROOT_SRC/include \
    $$ROOT_BUILD/include \
    $$ROOT_SRC/3rdparty/QXlsx/header

# Link against libdbridge and QXlsx in shadow build dirs
LIBS += -L$$ROOT_BUILD/src       -ldbridge
LIBS += -L$$ROOT_BUILD/3rdparty/QXlsx -lQXlsx
# dbridge_sqlite3 (SESSION-enabled) — the CLI itself only uses the Excel API, but
# libdbridge.a now carries sync .o; link it so any pulled-in session symbol
# resolves. Unreferenced archive members are simply dropped, so this is a no-op
# for a pure import/export CLI. MUST follow -ldbridge.
LIBS += -L$$ROOT_BUILD/3rdparty/sqlite3 -ldbridge_sqlite3

PRE_TARGETDEPS += \
    $$ROOT_BUILD/src/libdbridge.a \
    $$ROOT_BUILD/3rdparty/QXlsx/libQXlsx.a \
    $$ROOT_BUILD/3rdparty/sqlite3/libdbridge_sqlite3.a

SOURCES = main.cpp
