cmake_minimum_required(VERSION 3.16)
project(QXlsx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt5 5.12 REQUIRED COMPONENTS Core Gui)

# Stub library - compiles to a header-only static lib
add_library(QXlsx INTERFACE)
target_include_directories(QXlsx INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(QXlsx INTERFACE Qt5::Core Qt5::Gui)
