# sync-demo —— dbridge 多节点增量同步范式演示（控制台程序）。
#
# 说明：本项目的主验证路径是 qmake（见 build_qmake_demos 与 sync-demo.pro）。 此 CMakeLists 提供 等价的
# CMake 构建定义，方便 IDE / CMake 用户使用，与 sync-demo.pro 保持一致。
#
# 需要 Network（基于 UDP 的文件传输层 udp_transport.* 用到 QHostAddress/QUdpSocket），故在此 局部补充
# find_package（顶层 CMakeLists 只 find Core/Sql/Gui，与 sync-suite 的做法一致）。
find_package(Qt5 5.12 REQUIRED COMPONENTS Network)

# udp_transport.cpp 必须一并编译（含 UdpFileTransport 的实现；其头文件带 Q_OBJECT，靠全局 AUTOMOC
# 处理），否则 main.cpp 里对 UdpFileTransport 的引用会在链接期变成 undefined reference。 这与
# sync-demo.pro 的 `SOURCES = main.cpp udp_transport.cpp` 对齐。
add_executable(sync-demo main.cpp udp_transport.cpp)
target_link_libraries(sync-demo PRIVATE dbridge Qt5::Core Qt5::Sql Qt5::Network)
