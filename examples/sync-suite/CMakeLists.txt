# sync-suite —《数据库同步设计2》两场景完整演示（GUI）。
#
# 说明：本项目的主验证路径是 qmake（见 build_qmake_demos 与 sync-suite.pro）。 此 CMakeLists 提供等价的
# CMake 构建定义，方便 IDE / CMake 用户使用。
#
# 需要 Widgets（GUI）与 Network（UDP 传输层），故在此局部补充 find_package。
find_package(Qt5 5.12 REQUIRED COMPONENTS Widgets Network)

add_executable(
  sync-suite
  main.cpp
  MainWindow.cpp
  Scenario1Runner.cpp
  Scenario1Widget.cpp
  Scenario2Model.cpp
  Scenario2Widget.cpp
  CompareDetailDialog.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../sync-demo/udp_transport.cpp)

# udp_transport.h 位于 sync-demo 目录，需加入包含路径（同时供 AUTOMOC 找到头文件）。
target_include_directories(
  sync-suite PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                     ${CMAKE_CURRENT_SOURCE_DIR}/../sync-demo)

target_link_libraries(sync-suite PRIVATE dbridge Qt5::Core Qt5::Gui
                                         Qt5::Widgets Qt5::Sql Qt5::Network)

# ── 部署 session 版 QSQLITE 插件到可执行文件旁（等价于 sync-suite.pro 的 QMAKE_POST_LINK）──
# dbridge 把 SQLite 编译为带 SESSION + PREUPDATE_HOOK，其 sqlite3 结构体布局与 Qt 自带的 QSQLITE
# 插件不一致；若运行时加载系统插件，dbridge 代码访问 sqlite3* 句柄会读到错位字段 → SIGSEGV。因此必须把项目自建的 session
# 版插件部署到可执行文件旁的 sqldrivers/（main() 已把 applicationDirPath() 置于 libraryPaths
# 首位以优先加载它）。
#
# 该插件目前仅由 qmake 子项目 qsqlite_session 构建，默认从其构建产物部署； 也可用
# -DDBRIDGE_QSQLITE_SESSION_PLUGIN=<路径> 覆盖。
set(DBRIDGE_QSQLITE_SESSION_PLUGIN
    "${CMAKE_SOURCE_DIR}/build_qmake_demos/3rdparty/qsqlite_session/libqsqlite.so"
    CACHE FILEPATH "session 版 QSQLITE 插件路径（由 qmake qsqlite_session.pro 构建）")

if(EXISTS "${DBRIDGE_QSQLITE_SESSION_PLUGIN}")
  add_custom_command(
    TARGET sync-suite
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:sync-suite>/sqldrivers"
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different "${DBRIDGE_QSQLITE_SESSION_PLUGIN}"
      "$<TARGET_FILE_DIR:sync-suite>/sqldrivers/libqsqlite.so"
    COMMENT "部署 session 版 QSQLITE 插件到 sync-suite 旁")
else()
  message(
    WARNING "未找到 session 版 QSQLITE 插件：${DBRIDGE_QSQLITE_SESSION_PLUGIN}\n"
            "  sync-suite 仍可编译，但运行时打开 SQLite 会 SIGSEGV。请先用 qmake 构建该插件\n"
            "  （cd build_qmake_demos && qmake ../dbridge.pro &&\n"
            "   make sub-3rdparty-qsqlite_session-qsqlite_session-pro），\n"
            "  或用 -DDBRIDGE_QSQLITE_SESSION_PLUGIN=<路径> 指定。")
endif()
