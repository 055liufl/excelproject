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
    tst_import_single

tst_profile_loader.file       = unit/tst_profile_loader.pro
tst_schema_introspector.file  = unit/tst_schema_introspector.pro
tst_validator_chain.file      = unit/tst_validator_chain.pro
tst_sql_builder.file          = unit/tst_sql_builder.pro
tst_topo_sorter.file          = unit/tst_topo_sorter.pro
tst_router.file               = unit/tst_router.pro
tst_auto_profile_builder.file = unit/tst_auto_profile_builder.pro
tst_fk_preflight.file         = unit/tst_fk_preflight.pro
tst_import_single.file        = integration/tst_import_single.pro

OTHER_FILES += test-common.pri
