include(../test-common.pri)
QT += network                                    # 必须写在 include 之后（覆盖式 QT=）
TARGET   = tst_udp_reassembly
SOURCES  = tst_udp_reassembly.cpp \
           ../../examples/sync-demo/udp_transport.cpp
HEADERS  = ../../examples/sync-demo/udp_transport.h
INCLUDEPATH += ../../examples/sync-demo
# 覆盖率插桩（仅本测试构建）
QMAKE_CXXFLAGS += --coverage -O0
QMAKE_LFLAGS   += --coverage
