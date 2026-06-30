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
