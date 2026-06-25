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

# ---------------------------------------------------------------------------
# Vendored SQLite3 with SESSION + PREUPDATE_HOOK extensions enabled. We compile
# the amalgamation from Qt's bundled copy so we control the flags.
# ---------------------------------------------------------------------------
set(QT_SQLITE_SRC
    "/opt/Qt5.12.12/5.12.12/Src/qtbase/src/3rdparty/sqlite/sqlite3.c"
    CACHE FILEPATH "Path to Qt-bundled sqlite3.c amalgamation")
set(QT_SQLITE_INC
    "/opt/Qt5.12.12/5.12.12/Src/qtbase/src/3rdparty/sqlite"
    CACHE PATH "Directory containing sqlite3.h")

add_library(dbridge_sqlite3 STATIC ${QT_SQLITE_SRC})
target_compile_definitions(dbridge_sqlite3 PUBLIC SQLITE_ENABLE_SESSION
                                                  SQLITE_ENABLE_PREUPDATE_HOOK)
target_include_directories(dbridge_sqlite3 PUBLIC ${QT_SQLITE_INC})
set_target_properties(dbridge_sqlite3 PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Silence strict warnings from third-party code
target_compile_options(dbridge_sqlite3
                       PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-w>)

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
    src/mapping/TemporalConvert.cpp
    src/sql/SqlBuilder.cpp
    src/service/ErrorCollector.cpp
    src/service/ImportService.cpp
    src/service/ExportService.cpp
    src/sync/WriteTxn.cpp
    src/sync/SyncContext.cpp
    src/sync/capture/SqliteHandle.cpp
    src/sync/schema/SchemaEligibility.cpp
    src/sync/schema/TableStateStore.cpp
    src/sync/apply/AppliedVectorStore.cpp
    src/sync/apply/RowWinnerStore.cpp
    src/sync/apply/ChangesetApplier.cpp
    src/sync/apply/CapturedWriteTemplate.cpp
    src/sync/capture/ChangelogStore.cpp
    src/sync/capture/SessionRecorder.cpp
    src/sync/schema/SchemaGuard.cpp
    src/sync/schema/QuarantineStore.cpp
    src/sync/payload/PayloadCodec.cpp
    src/sync/transport/OutboxWriter.cpp
    src/sync/transport/InboxLedger.cpp
    src/sync/transport/InboxWatcher.cpp
    src/sync/transport/AckChannel.cpp
    src/sync/anchor/OutboundAckStore.cpp
    # M1c: SyncWorker + SyncEngine
    src/sync/SyncWorker.cpp
    src/sync/SyncEngine.cpp
    # M2: conflict
    src/sync/conflict/RoutingTable.cpp
    src/sync/conflict/ConflictArbiter.cpp
    src/sync/conflict/RebaseEngine.cpp
    # M2: apply extras
    src/sync/apply/UpsertExecutor.cpp
    src/sync/apply/SelectionPushApplier.cpp
    # M2: selection
    src/sync/selection/SelectionResolver.cpp
    src/sync/selection/ConsistencyCache.cpp
    src/sync/selection/FkClosureBuilder.cpp
    src/sync/selection/FrozenManifest.cpp
    src/sync/selection/ChunkStreamer.cpp
    # M3: batch transfer
    src/batch/BatchTransfer.cpp
    # M4: diff / comparison
    src/sync/diff/DiffEngine.cpp
    src/sync/diff/InboundTableGate.cpp
    src/sync/diff/StagingBuffer.cpp
    src/sync/diff/ComparisonSession.cpp
    # M4: baseline + peer
    src/sync/baseline/BaselineManager.cpp
    src/sync/peer/DeadPeerEvictor.cpp)

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
  PRIVATE QXlsx Qt5::Gui dbridge_sqlite3)

target_compile_definitions(dbridge PRIVATE SQLITE_ENABLE_SESSION
                                           SQLITE_ENABLE_PREUPDATE_HOOK)

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
