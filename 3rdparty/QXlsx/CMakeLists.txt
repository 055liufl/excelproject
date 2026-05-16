cmake_minimum_required(VERSION 3.16)
project(QXlsx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(Qt5 5.12 REQUIRED COMPONENTS Core Gui)

file(GLOB QXLSX_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/source/*.cpp)
file(GLOB QXLSX_HEADERS CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/header/*.h)

add_library(QXlsx STATIC ${QXLSX_SOURCES} ${QXLSX_HEADERS})

target_include_directories(
  QXlsx
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/header
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/header)

# QXlsx uses Qt's private ZIP API (qzipreader_p.h / qzipwriter_p.h), which lives
# in QtGui's private include dir. Compute it from Qt5Gui_PRIVATE_INCLUDE_DIRS
# (set by find_package(Qt5 COMPONENTS Gui)).
target_include_directories(QXlsx PRIVATE ${Qt5Gui_PRIVATE_INCLUDE_DIRS})

target_link_libraries(QXlsx PUBLIC Qt5::Core Qt5::Gui)

# Silence -Wall/-Wextra noise from QXlsx — it's an external library we don't
# control
target_compile_options(QXlsx PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-w>
                                     $<$<CXX_COMPILER_ID:MSVC>:/W0>)

set_target_properties(QXlsx PROPERTIES POSITION_INDEPENDENT_CODE ON)
