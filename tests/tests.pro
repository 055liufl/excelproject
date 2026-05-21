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
    tst_import_single

tst_profile_loader.file          = unit/tst_profile_loader.pro
tst_schema_introspector.file     = unit/tst_schema_introspector.pro
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

OTHER_FILES += test-common.pri
