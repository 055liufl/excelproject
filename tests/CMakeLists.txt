cmake_minimum_required(VERSION 3.16)

set(CMAKE_AUTOMOC ON)

find_package(Qt5 5.12 REQUIRED COMPONENTS Core Sql Test)

function(add_dbridge_test name sources)
  add_executable(${name} ${sources})
  target_link_libraries(${name} PRIVATE dbridge QXlsx Qt5::Test Qt5::Sql
                                        Qt5::Core)
  target_include_directories(
    ${name}
    PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/3rdparty/QXlsx/src
            ${CMAKE_SOURCE_DIR}/tests)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

add_dbridge_test(tst_profile_loader unit/tst_profile_loader.cpp)
add_dbridge_test(tst_schema_introspector unit/tst_schema_introspector.cpp)
add_dbridge_test(tst_databridge_schema unit/tst_databridge_schema.cpp)
add_dbridge_test(tst_validator_chain unit/tst_validator_chain.cpp)
add_dbridge_test(tst_sql_builder unit/tst_sql_builder.cpp)
add_dbridge_test(tst_topo_sorter unit/tst_topo_sorter.cpp)
add_dbridge_test(tst_router unit/tst_router.cpp)
add_dbridge_test(tst_auto_profile_builder unit/tst_auto_profile_builder.cpp)
add_dbridge_test(tst_fk_preflight unit/tst_fk_preflight.cpp)
add_dbridge_test(tst_profile_validator unit/tst_profile_validator.cpp)
add_dbridge_test(tst_lookup_prefetch unit/tst_lookup_prefetch.cpp)
add_dbridge_test(tst_lookup_semantics unit/tst_lookup_semantics.cpp)
add_dbridge_test(tst_export_helpers unit/tst_export_helpers.cpp)
add_dbridge_test(tst_reverse_lookup_export unit/tst_reverse_lookup_export.cpp)
add_dbridge_test(tst_temporal_import unit/tst_temporal_import.cpp)
add_dbridge_test(tst_temporal_export unit/tst_temporal_export.cpp)
add_dbridge_test(tst_column_order_export unit/tst_column_order_export.cpp)
add_dbridge_test(tst_import_single integration/tst_import_single.cpp)

# ── sync unit tests ─────────────────────────────────────────────────────────
add_dbridge_test(tst_sync_conflict_arbiter unit/tst_sync_conflict_arbiter.cpp)
add_dbridge_test(tst_sync_routing_table unit/tst_sync_routing_table.cpp)
add_dbridge_test(tst_sync_foreground_gate unit/tst_sync_foreground_gate.cpp)
add_dbridge_test(tst_write_txn unit/tst_write_txn.cpp)
add_dbridge_test(tst_sync_applied_vector unit/tst_sync_applied_vector.cpp)
add_dbridge_test(tst_sync_row_winner unit/tst_sync_row_winner.cpp)
add_dbridge_test(tst_sync_schema_guard unit/tst_sync_schema_guard.cpp)
add_dbridge_test(tst_sync_quarantine_store unit/tst_sync_quarantine_store.cpp)
add_dbridge_test(tst_sync_inbox_ledger unit/tst_sync_inbox_ledger.cpp)
add_dbridge_test(tst_sync_outbound_ack unit/tst_sync_outbound_ack.cpp)
add_dbridge_test(tst_sync_changelog_store unit/tst_sync_changelog_store.cpp)
add_dbridge_test(tst_sync_table_state unit/tst_sync_table_state.cpp)
add_dbridge_test(tst_sync_payload_codec unit/tst_sync_payload_codec.cpp)
add_dbridge_test(tst_sync_schema_eligibility
                 unit/tst_sync_schema_eligibility.cpp)
add_dbridge_test(tst_sync_inbound_table_gate
                 unit/tst_sync_inbound_table_gate.cpp)
add_dbridge_test(tst_sync_staging_buffer unit/tst_sync_staging_buffer.cpp)
add_dbridge_test(tst_sync_upsert_executor unit/tst_sync_upsert_executor.cpp)
add_dbridge_test(tst_sync_consistency_cache unit/tst_sync_consistency_cache.cpp)
add_dbridge_test(tst_sync_dead_peer_evictor unit/tst_sync_dead_peer_evictor.cpp)
