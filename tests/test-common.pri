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

INCLUDEPATH += \
    $$ROOT_SRC/include \
    $$ROOT_BUILD/include \
    $$ROOT_SRC/src \
    $$ROOT_SRC/3rdparty/QXlsx/header

# Link static libs (build order enforced via tests/tests.pro depends + the
# top-level dbridge.pro depends chain).
LIBS += -L$$ROOT_BUILD/src       -ldbridge
LIBS += -L$$ROOT_BUILD/3rdparty/QXlsx -lQXlsx

PRE_TARGETDEPS += \
    $$ROOT_BUILD/src/libdbridge.a \
    $$ROOT_BUILD/3rdparty/QXlsx/libQXlsx.a
