cmake_minimum_required(VERSION 3.16)

set(CMAKE_AUTOMOC ON)

find_package(Qt5 5.12 REQUIRED COMPONENTS Core Sql Test)

function(add_dbridge_test name sources)
  add_executable(${name} ${sources})
  target_link_libraries(${name} PRIVATE dbridge QXlsx Qt5::Test Qt5::Sql
                                        Qt5::Core)
  target_include_directories(
    ${name} PRIVATE ${CMAKE_SOURCE_DIR}/src
                    ${CMAKE_SOURCE_DIR}/3rdparty/QXlsx/src)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

add_dbridge_test(tst_profile_loader unit/tst_profile_loader.cpp)
add_dbridge_test(tst_schema_introspector unit/tst_schema_introspector.cpp)
add_dbridge_test(tst_validator_chain unit/tst_validator_chain.cpp)
add_dbridge_test(tst_sql_builder unit/tst_sql_builder.cpp)
add_dbridge_test(tst_topo_sorter unit/tst_topo_sorter.cpp)
add_dbridge_test(tst_router unit/tst_router.cpp)
add_dbridge_test(tst_auto_profile_builder unit/tst_auto_profile_builder.cpp)
add_dbridge_test(tst_fk_preflight unit/tst_fk_preflight.cpp)
add_dbridge_test(tst_import_single integration/tst_import_single.cpp)
