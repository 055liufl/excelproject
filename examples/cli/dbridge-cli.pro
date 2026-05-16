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

PRE_TARGETDEPS += \
    $$ROOT_BUILD/src/libdbridge.a \
    $$ROOT_BUILD/3rdparty/QXlsx/libQXlsx.a

SOURCES = main.cpp
