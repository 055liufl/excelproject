# Vendored QXlsx (static library).
# Mirrors 3rdparty/QXlsx/CMakeLists.txt.

TEMPLATE = lib
CONFIG  += staticlib c++17
TARGET   = QXlsx

QT       = core gui gui-private

# Position-independent code (parity with PROPERTIES POSITION_INDEPENDENT_CODE ON)
CONFIG  += create_prl object_parallel_to_source
QMAKE_CXXFLAGS += -fPIC

# Silence warnings — QXlsx is upstream code we don't own
QMAKE_CXXFLAGS_WARN_ON =
QMAKE_CXXFLAGS_WARN_OFF = -w
CONFIG  -= warn_on
CONFIG  += warn_off

INCLUDEPATH += $$PWD/header
DEPENDPATH  += $$PWD/header

# Glob sources / headers (mirrors file(GLOB ...) in CMake)
SOURCES += $$files($$PWD/source/*.cpp, false)
HEADERS += $$files($$PWD/header/*.h, false)

# QXlsx pulls in Qt's private ZIP API (qzipreader_p.h / qzipwriter_p.h) which
# lives in QtGui's private include dir; `QT += gui-private` above adds it.

CONFIG += skip_target_version_ext
