cmake_minimum_required(VERSION 3.16)
project(dbridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
include(GenerateExportHeader)
include(CTest)

find_package(Qt5 5.12 REQUIRED COMPONENTS Core Sql Gui)

if(BUILD_TESTING)
  find_package(Qt5 5.12 REQUIRED COMPONENTS Test)
endif()

add_subdirectory(3rdparty/QXlsx)

set(DBRIDGE_SOURCES
    src/DataBridge.cpp
    src/profile/ProfileLoader.cpp
    src/profile/ProfileValidator.cpp
    src/profile/AutoProfileBuilder.cpp
    src/schema/SchemaIntrospector.cpp
    src/excel/ExcelReader.cpp
    src/excel/ExcelWriter.cpp
    src/validation/ValidatorChain.cpp
    src/validation/Validators.cpp
    src/validation/ForeignKeyPreflight.cpp
    src/mapping/Router.cpp
    src/mapping/Mapper.cpp
    src/mapping/TopoSorter.cpp
    src/mapping/FkInjector.cpp
    src/mapping/BatchUniqueness.cpp
    src/sql/SqlBuilder.cpp
    src/service/ErrorCollector.cpp
    src/service/ImportService.cpp
    src/service/ExportService.cpp)

add_library(dbridge ${DBRIDGE_SOURCES})

generate_export_header(dbridge BASE_NAME DBRIDGE EXPORT_FILE_NAME
                       ${CMAKE_CURRENT_BINARY_DIR}/include/dbridge/Export.h)

target_include_directories(
  dbridge
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_BINARY_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(
  dbridge
  PUBLIC Qt5::Core Qt5::Sql
  PRIVATE QXlsx Qt5::Gui)

target_compile_options(
  dbridge PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
                  $<$<CXX_COMPILER_ID:MSVC>:/W4>)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

option(DBRIDGE_BUILD_EXAMPLES "Build dbridge example programs" ON)
if(DBRIDGE_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
