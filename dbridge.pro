# Top-level qmake project file.
# Mirrors CMakeLists.txt structure. Build via:
#   mkdir build && cd build && qmake ../dbridge.pro && make -j$(nproc)
#
# Sub-projects:
#   - 3rdparty/QXlsx   : vendored static lib (Excel I/O)
#   - 3rdparty/sqlite3 : SQLite amalgamation with SESSION + PREUPDATE_HOOK,
#                        required by the SQLite 同步工具 subsystem (src/sync/**)
#   - src              : libdbridge (static; Excel I/O + sync subsystem)
#   - examples/cli     : dbridge-cli example
#   - examples/sync-demo : 多节点增量同步范式演示
#   - examples/diff-demo : Beyond Compare 差异比对范式演示
#   - tests            : Qt Test suites (Excel + sync; CONFIG += testcase, `make check`)

TEMPLATE = subdirs
CONFIG  += ordered

SUBDIRS = \
    qxlsx \
    sqlite3 \
    libdbridge \
    cli \
    syncdemo \
    diffdemo \
    tests

qxlsx.file         = 3rdparty/QXlsx/QXlsx.pro
sqlite3.file       = 3rdparty/sqlite3/dbridge_sqlite3.pro
libdbridge.file    = src/libdbridge.pro
cli.file           = examples/cli/dbridge-cli.pro
syncdemo.file      = examples/sync-demo/sync-demo.pro
diffdemo.file      = examples/diff-demo/diff-demo.pro
tests.file         = tests/tests.pro

# Dependency chain (qmake honours these when generating Makefile.tests etc.).
# libdbridge only needs sqlite3's *header* (compile-time), but cli/tests *link*
# libdbridge_sqlite3.a, so they must depend on the sqlite3 sub-project.
libdbridge.depends = qxlsx
cli.depends        = libdbridge sqlite3
syncdemo.depends   = libdbridge sqlite3
diffdemo.depends   = libdbridge sqlite3
tests.depends      = libdbridge qxlsx sqlite3

OTHER_FILES += \
    README.md \
    qsqlite3.pri \
    docs/adr/0001-time-format-in-profile.md \
    docs/adr/0002-export-column-order-in-exportspec.md \
    docs/adr/0003-export-reverse-lookup.md \
    docs/adr/0004-explicit-temporal-type.md \
    docs/validation/row-to-multitable.md \
    CMakeLists.txt
