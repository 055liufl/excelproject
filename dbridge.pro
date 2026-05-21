# Top-level qmake project file.
# Mirrors CMakeLists.txt structure. Build via:
#   mkdir build && cd build && qmake ../dbridge.pro && make -j$(nproc)
#
# Sub-projects:
#   - 3rdparty/QXlsx : vendored static lib
#   - src            : libdbridge (static)
#   - examples/cli   : dbridge-cli example
#   - tests          : 17 Qt Test suites (CONFIG += testcase, run with `make check`)

TEMPLATE = subdirs
CONFIG  += ordered

SUBDIRS = \
    qxlsx \
    libdbridge \
    cli \
    tests

qxlsx.file         = 3rdparty/QXlsx/QXlsx.pro
libdbridge.file    = src/libdbridge.pro
cli.file           = examples/cli/dbridge-cli.pro
tests.file         = tests/tests.pro

# Dependency chain (qmake honours these when generating Makefile.tests etc.)
libdbridge.depends = qxlsx
cli.depends        = libdbridge
tests.depends      = libdbridge qxlsx

OTHER_FILES += \
    README.md \
    docs/adr/0001-time-format-in-profile.md \
    docs/adr/0002-export-column-order-in-exportspec.md \
    docs/adr/0003-export-reverse-lookup.md \
    docs/adr/0004-explicit-temporal-type.md \
    docs/validation/row-to-multitable.md \
    CMakeLists.txt
