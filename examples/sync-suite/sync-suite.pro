# ──────────────────────────────────────────────────────────────────────────────
# sync-suite — 《数据库同步设计2》两场景完整演示（含 GUI）
#
# 单一 Qt Widgets 程序，QTabWidget 提供两个标签页：
#   场景1：1 中心节点 + 3 子节点 + 指定数据库 的真实多节点 UDP 同步演示
#          （指定数据库为权威源，冲突时以指定数据为准 → 全域收敛）
#   场景2：类 Beyond Compare 的「子节点B ⇄ 中心节点A」差异比对与列级同步 GUI
#
# 构建方式（集成进顶层 dbridge.pro 后，在 shadow build 目录执行）：
#   cd build_qmake_demos
#   qmake ../dbridge.pro
#   make sub-examples-sync-suite-sync-suite-pro -j$(nproc)
# ──────────────────────────────────────────────────────────────────────────────

TEMPLATE = app
CONFIG  += c++17
CONFIG  -= app_bundle
TARGET   = sync-suite

# 需要 widgets（GUI）与 network（UDP 传输层）。
QT = core gui widgets sql network

DEFINES += DBRIDGE_STATIC_DEFINE
DEFINES += SQLITE_ENABLE_SESSION SQLITE_ENABLE_PREUPDATE_HOOK

ROOT_SRC   = $$PWD/../..
ROOT_BUILD = $$shadowed($$ROOT_SRC)

INCLUDEPATH += \
    $$ROOT_SRC/include \
    $$ROOT_BUILD/include \
    $$PWD/../sync-demo \
    /opt/Qt5.12.12/5.12.12/Src/qtbase/src/3rdparty/sqlite

LIBS += -L$$ROOT_BUILD/src            -ldbridge
LIBS += -L$$ROOT_BUILD/3rdparty/QXlsx -lQXlsx
LIBS += -L$$ROOT_BUILD/3rdparty/sqlite3 -ldbridge_sqlite3
LIBS += -lpthread -ldl

PRE_TARGETDEPS += \
    $$ROOT_BUILD/src/libdbridge.a \
    $$ROOT_BUILD/3rdparty/sqlite3/libdbridge_sqlite3.a

# ── 部署带 SQLITE_ENABLE_SESSION 的自定义 QSQLITE 插件到可执行文件旁 ──────────
# Qt 在 QCoreApplication::libraryPaths() 中查找插件；main.cpp 会把
# applicationDirPath() 置于首位，从而让此处的 sqldrivers/ 副本优先于系统插件。
QSQLITE_SESSION_LIB  = $$ROOT_BUILD/3rdparty/qsqlite_session/libqsqlite.so
QSQLITE_SESSION_DEST = $$OUT_PWD/sqldrivers
QMAKE_POST_LINK += mkdir -p $$QSQLITE_SESSION_DEST && cp $$QSQLITE_SESSION_LIB $$QSQLITE_SESSION_DEST/libqsqlite.so

# 复用 sync-demo 的 UDP 文件传输层（场景1 的真实网络传输）。
SOURCES = \
    main.cpp \
    MainWindow.cpp \
    Scenario1Runner.cpp \
    Scenario1Widget.cpp \
    Scenario2Model.cpp \
    Scenario2Widget.cpp \
    Scenario2SnapshotService.cpp \
    CompareDetailDialog.cpp \
    ../sync-demo/udp_transport.cpp

HEADERS = \
    MainWindow.h \
    Scenario1Runner.h \
    Scenario1Widget.h \
    Scenario2Model.h \
    Scenario2Widget.h \
    Scenario2SnapshotService.h \
    CompareDetailDialog.h \
    ../sync-demo/udp_transport.h
