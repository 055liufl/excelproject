# Tests subdirs project. Mirrors tests/CMakeLists.txt.
# Run via:
#   make check         # all suites
#   make sub-tst_fk_preflight-check   # one suite

TEMPLATE = subdirs
CONFIG  += ordered

# Each .pro alongside its tst_*.cpp; subdirs entries are pro filenames sans .pro.
SUBDIRS = \
    tst_profile_loader \
    tst_schema_introspector \
    tst_databridge_schema \
    tst_validator_chain \
    tst_sql_builder \
    tst_topo_sorter \
    tst_router \
    tst_auto_profile_builder \
    tst_fk_preflight \
    tst_profile_validator \
    tst_lookup_prefetch \
    tst_lookup_semantics \
    tst_export_helpers \
    tst_reverse_lookup_export \
    tst_temporal_import \
    tst_temporal_export \
    tst_column_order_export \
    tst_import_single \
    tst_sync_conflict_arbiter \
    tst_sync_routing_table \
    tst_sync_foreground_gate \
    tst_write_txn \
    tst_sync_applied_vector \
    tst_sync_row_winner \
    tst_sync_schema_guard \
    tst_sync_quarantine_store \
    tst_sync_inbox_ledger \
    tst_sync_outbound_ack \
    tst_sync_changelog_store \
    tst_sync_table_state \
    tst_sync_payload_codec \
    tst_sync_schema_eligibility \
    tst_sync_inbound_table_gate \
    tst_sync_staging_buffer \
    tst_sync_upsert_executor \
    tst_sync_consistency_cache \
    tst_sync_dead_peer_evictor

tst_profile_loader.file          = unit/tst_profile_loader.pro
tst_schema_introspector.file     = unit/tst_schema_introspector.pro
tst_databridge_schema.file       = unit/tst_databridge_schema.pro
tst_validator_chain.file         = unit/tst_validator_chain.pro
tst_sql_builder.file             = unit/tst_sql_builder.pro
tst_topo_sorter.file             = unit/tst_topo_sorter.pro
tst_router.file                  = unit/tst_router.pro
tst_auto_profile_builder.file    = unit/tst_auto_profile_builder.pro
tst_fk_preflight.file            = unit/tst_fk_preflight.pro
tst_profile_validator.file       = unit/tst_profile_validator.pro
tst_lookup_prefetch.file         = unit/tst_lookup_prefetch.pro
tst_lookup_semantics.file        = unit/tst_lookup_semantics.pro
tst_export_helpers.file          = unit/tst_export_helpers.pro
tst_reverse_lookup_export.file   = unit/tst_reverse_lookup_export.pro
tst_temporal_import.file         = unit/tst_temporal_import.pro
tst_temporal_export.file         = unit/tst_temporal_export.pro
tst_column_order_export.file     = unit/tst_column_order_export.pro
tst_import_single.file           = integration/tst_import_single.pro

tst_sync_conflict_arbiter.file   = unit/tst_sync_conflict_arbiter.pro
tst_sync_routing_table.file      = unit/tst_sync_routing_table.pro
tst_sync_foreground_gate.file    = unit/tst_sync_foreground_gate.pro
tst_write_txn.file               = unit/tst_write_txn.pro
tst_sync_applied_vector.file     = unit/tst_sync_applied_vector.pro
tst_sync_row_winner.file         = unit/tst_sync_row_winner.pro
tst_sync_schema_guard.file       = unit/tst_sync_schema_guard.pro
tst_sync_quarantine_store.file   = unit/tst_sync_quarantine_store.pro
tst_sync_inbox_ledger.file       = unit/tst_sync_inbox_ledger.pro
tst_sync_outbound_ack.file       = unit/tst_sync_outbound_ack.pro
tst_sync_changelog_store.file    = unit/tst_sync_changelog_store.pro
tst_sync_table_state.file        = unit/tst_sync_table_state.pro
tst_sync_payload_codec.file      = unit/tst_sync_payload_codec.pro
tst_sync_schema_eligibility.file = unit/tst_sync_schema_eligibility.pro
tst_sync_inbound_table_gate.file = unit/tst_sync_inbound_table_gate.pro
tst_sync_staging_buffer.file     = unit/tst_sync_staging_buffer.pro
tst_sync_upsert_executor.file    = unit/tst_sync_upsert_executor.pro
tst_sync_consistency_cache.file  = unit/tst_sync_consistency_cache.pro
tst_sync_dead_peer_evictor.file  = unit/tst_sync_dead_peer_evictor.pro

OTHER_FILES += test-common.pri
