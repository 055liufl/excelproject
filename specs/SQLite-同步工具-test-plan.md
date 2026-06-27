# SQLite 同步工具测试计划

## 1. 测试目标与覆盖率预算

### 1.1 文档信息表

| 项 | 内容 |
| --- | --- |
| 文档名称 | SQLite 同步工具测试计划 |
| 版本 | v1.0（草案） |
| 日期 | 2026-06-27 |
| 依据文档 | `specs/SQLite-同步工具-设计文档.md` v0.5、`specs/SQLite-同步工具-plan.md` v0.5（需求 FR-1..FR-17、NFR-1..NFR-9、共识 C1..C17、设计评审不变量 G-01..G-10） |
| 适用代码 | `src/sync/**`（8,856 行 cpp，占全库 14,782 行的 60%，当前零测试）；ETL/Excel 侧约 5,900 行（已有 17 个 Qt Test 套件部分覆盖） |
| 技术栈 | Qt 5.12 / C++17 静态库；Qt Test 框架；内存 SQLite（`:memory:`）+ 临时文件 SQLite |
| 读者 | 同步子系统开发者、测试/QA 工程师、代码评审者、集成与发布负责人 |
| 目标（一句话） | 将零测试的同步子系统从 0 推进到可验收，并把全库整体行覆盖率拉到 ≥80%、分支覆盖率 ≥70%，同时为关键不变量与 ETL 重构提供可断言的回归守护。 |

### 1.2 测试目标

| 编号 | 目标 | 验收判据 |
| --- | --- | --- |
| O1 | 同步子系统从 0 到可验收 | `src/sync/**` 全部模块具备单元/集成测试，覆盖率达到 §1.5 预算；FR-1..FR-17 关键路径均有对应套件 |
| O2 | 覆盖率门槛 | 整体行覆盖率（line coverage）≥80%，分支覆盖率（branch coverage）≥70% |
| O3 | 错误码码级断言 | 所有 `E_SYNC_*` / `W_SYNC_*` 错误码均有产生该码的场景用例并以码值断言（而非仅断言"失败"） |
| O4 | 关键不变量可断言夹具 | FR-6 逐行胜者到达序无关、G-05 严格连续、FR-1 崩溃零窗口、G-06 判等三元组、G-07 单写者唯一，各自有专用可断言夹具 |
| O5 | ETL 重构回归守护 | 既有 ETL 重构（`UpsertExecutor` 提取）有回归测试守护，重构前后行为等价 |

不变量与夹具对应关系：

| 不变量 | 含义 | 夹具/套件 | 断言要点 |
| --- | --- | --- | --- |
| FR-6 | 逐行胜者到达序无关 | `tst_apply_order_independence` | 同一行多版本以任意到达序施加，最终胜者与状态一致（属性测试，全排列/随机序） |
| G-05 | 严格连续 | `tst_selection_strict_contiguity` | 序号/游标无空洞、无跳变；断点续传不丢不重 |
| FR-1 | 崩溃零窗口 | `tst_apply_crash_window` | 子进程在任意提交点被杀，重启后无半写状态、可恢复，零数据窗口 |
| G-06 | 判等三元组 | `tst_diff_equality_triple` | 判等基于 (主键, 内容指纹, 版本) 三元组，等价/不等价边界正确 |
| G-07 | 单写者唯一 | `tst_writetxn_single_writer` | 同一时刻仅一个写事务持锁，并发写被串行化/拒绝 |

### 1.3 测试范围与非范围

| 类别 | 范围（In Scope） | 非范围（Out of Scope） |
| --- | --- | --- |
| 同步引擎 | `src/sync/**` 全部模块（apply/diff/schema/selection/baseline/transport/capture/payload/anchor/conflict/peer 及顶层 SyncWorker/SyncEngine/SyncContext/WriteTxn） | 第三方传输工具本身（仅在边界处打桩验证契约） |
| 场景 | 场景 2 端到端、两节点、星型拓扑 | 跨进程并发同步、宿主 GUI 交互 |
| 门面 | 批量门面（batch facade） | DDL 变更同步、CRDT 合并算法 |
| 回归 | ETL 回归（`UpsertExecutor` 提取重构） | 宿主应用业务逻辑、Excel 渲染呈现 |

明确非范围：第三方传输工具的内部实现、宿主 GUI、跨进程并发同步、DDL/CRDT，均不在本计划覆盖目标之内。

### 1.4 分层测试策略

| 层 | 范围与手段 | 目标 | 占比（用例数估算） |
| --- | --- | --- | --- |
| 单元测试 | 纯逻辑模块 + 内存 SQLite（`:memory:`）；diff/payload/anchor/conflict/peer/baseline/selection 等 | 覆盖纯算法与码级分支，快速、可重复 | 约 60% |
| 集成测试 | 两节点 / 星型 / 场景 2 端到端；SyncWorker 编排路径 | 验证跨模块协作与 SyncWorker 编排（覆盖率最大风险点的主力手段） | 约 25% |
| 故障注入测试 | 崩溃零窗口子进程夹具；在提交点注入 kill | 验证 FR-1 崩溃零窗口与可恢复性 | 约 8% |
| 属性/不变量测试 | 到达序无关、幂等性（多序施加/重放） | 验证 FR-6、G-05 等顺序无关与幂等不变量 | 约 4% |
| 性能/字节预算测试 | NFR-5 载荷 ≤2Mbps 预算；payload 字节计量 | 验证载荷规模与速率预算不回退 | 约 3% |

### 1.5 覆盖率预算表（核心）

| 子系统/模块 | cpp 行数 | 目标行覆盖率% | 估算贡献行数 | 备注 |
| --- | ---: | ---: | ---: | --- |
| `src/sync/apply` | 1944 | 88 | 1711 | DB 交互 + 施加逻辑，集成+单元并重 |
| `src/sync` 顶层 SyncWorker | 2106 | 72 | 1516 | **覆盖率最大风险点**，主要靠集成测试覆盖 |
| `src/sync` 顶层 SyncEngine | 373 | 85 | 317 | 编排核心，集成为主 |
| `src/sync` 顶层 SyncContext | 163 | 88 | 143 | 上下文/状态，单元为主 |
| `src/sync` 顶层 WriteTxn（含其余） | 39 | 90 | 35 | G-07 单写者，单元夹具 |
| `src/sync/diff` | 916 | 93 | 852 | 纯逻辑，G-06 判等三元组 |
| `src/sync/schema` | 723 | 88 | 636 | DB 交互（schema 协商） |
| `src/sync/selection` | 628 | 90 | 565 | 选择/游标，G-05 严格连续 |
| `src/sync/baseline` | 536 | 88 | 472 | 基线快照，DB 交互 |
| `src/sync/transport` | 390 | 85 | 332 | 传输边界，打桩契约 |
| `src/sync/capture` | 376 | 88 | 331 | 变更捕获，DB 交互 |
| `src/sync/payload` | 352 | 93 | 327 | 纯逻辑 + NFR-5 字节预算 |
| `src/sync/anchor` | 155 | 92 | 143 | 纯逻辑锚点 |
| `src/sync/conflict` | 93 | 95 | 88 | 纯逻辑冲突裁决 |
| `src/sync/peer` | 62 | 92 | 57 | 纯逻辑对端 |
| **sync 小计** | **8856** | — | **7625** | 加权 ≈ 86% |
| service | 1873 | 80 | 1498 | 部分已有覆盖 + 补齐 |
| profile | 1710 | 80 | 1368 | 部分已有覆盖 + 补齐 |
| mapping | 564 | 85 | 479 | ETL 映射，已有套件 |
| validation | 471 | 88 | 415 | 纯逻辑校验，已有套件 |
| batch | 408 | 88 | 359 | 批量门面，新增覆盖 |
| schema（introspector） | 174 | 85 | 148 | 内省，DB 交互 |
| excel | 152 | 60 | 91 | Excel 呈现，端到端为主 |
| sql | 140 | 85 | 119 | SQL 构造，单元 |
| DataBridge 及其它 | 434 | 78 | 339 | 桥接/杂项 |
| **非 sync 小计** | **5926** | — | **5216** | 加权 ≈ 88%（受 excel 拉低后约 80%+） |
| **全库合计** | **14782** | — | **≈12300** | **加权行覆盖率 ≈ 83%（≥80% 留有余量）** |

风险点说明：`SyncWorker`（2106 行，占 sync 子系统约 24%）编排逻辑分支密集、状态机复杂，难以用纯单元测试穷举，目标行覆盖率定为约 72%，主要依赖两节点/星型/场景 2 端到端集成测试覆盖，是整张预算表的**最大覆盖率风险点**；若集成测试搭建不充分，将直接拖累全库合计指标。

### 1.6 覆盖率达成逻辑

当前全库覆盖率仅来自 ETL 侧的 17 个 Qt Test 套件，估算整体行覆盖率约 35–40%——因为占全库 60% 的 `src/sync/**`（8,856 行）处于**零测试**状态，无论 ETL 侧覆盖多高，整体被这 60% 的未覆盖面拉低封顶。

达成路径如下：

| 阶段动作 | 新增覆盖行（估算） | 对全库行覆盖率的拉动 |
| --- | ---: | --- |
| 起点：仅 ETL 侧现状 | ≈5,200–5,900 中部分命中 | ≈35–40% |
| 补齐 sync 纯逻辑/DB 交互单元测试（diff/payload/anchor/conflict/peer/baseline/selection/schema/capture/apply 等） | ≈6,100 | sync 内大部分模块达 85–95% |
| 补齐两节点/星型/场景 2 集成测试，覆盖 SyncWorker/SyncEngine 编排 | ≈1,500 | SyncWorker 达 ≈72%，闭合 sync 编排面 |
| 补齐非 sync 缺口（batch 门面、service/profile 补测） | ≈1,000 | 非 sync 加权抬至 ≈80%+ |
| **合计** | **≈12,300 / 14,782** | **≈83%（≥80%）** |

逻辑结论：占比 60% 的 sync 子系统从 0 提升到加权约 86%，单这一项就可为全库贡献约 7,600 命中行（占全库 51 个百分点的可达覆盖），叠加既有 ETL 侧覆盖与少量非 sync 补测，即可把整体行覆盖率从约 35–40% 拉到 ≥80%，并保留约 3 个百分点的余量以吸收 SyncWorker 集成测试不及预期的风险。
## 2. 测试环境与工具链

本节定义「SQLite 同步工具」测试所需的软硬件依赖、双轨（CMake / qmake）构建与运行方式、新测试套件接入规范、覆盖率工具链接入与统计口径、CI 集成闸门，以及保证用例确定性与隔离的硬性约定。所有规范以仓库现有的 `tests/CMakeLists.txt`、`tests/tests.pro`、`tests/test-common.pri` 与根 `CMakeLists.txt` 为准。

### 2.1 软硬件与依赖矩阵

| 类别 | 组件 | 版本/要求 | 说明 |
| --- | --- | --- | --- |
| 语言标准 | C++ | C++17（`CMAKE_CXX_STANDARD 17`，`CONFIG += c++17`） | 根 CMake 与 `test-common.pri` 均已强制 |
| 框架 | Qt | 5.12.x（基线 5.12.12） | 模块：`Core` `Sql` `Gui` `Test`（testlib） |
| 构建系统 | CMake | ≥ 3.16 | 根 `CMakeLists.txt`，`include(CTest)`，`if(BUILD_TESTING) add_subdirectory(tests)` |
| 构建系统 | qmake | 随 Qt 5.12 | `tests/tests.pro` `TEMPLATE=subdirs`；`make check` |
| 编译器 | GCC / Clang | 支持 C++17（GCC ≥ 7 / Clang ≥ 6） | Linux 为主平台 |
| 数据库 | SQLite | Qt 自带 amalgamation 编译 | 必须开启 `SQLITE_ENABLE_SESSION`、`SQLITE_ENABLE_PREUPDATE_HOOK`（根 CMake 的 `dbridge_sqlite3` 静态库已配置；测试经 `dbridge` 链接间接获得） |
| 第三方库 | QXlsx | 仓库内 `3rdparty/QXlsx`（静态库） | 测试链接 `-lQXlsx` |
| 被测库 | dbridge | 仓库内 `src`（静态库 `libdbridge.a`） | 测试链接 `-ldbridge`，定义 `DBRIDGE_STATIC_DEFINE` |
| 覆盖率 | lcov / genhtml | ≥ 1.14 | GCC 工具链覆盖率采集与 HTML 报告 |
| 覆盖率 | gcovr | ≥ 5.0 | 跨 GCC/Clang，支持 `--fail-under-line` 闸门 |
| 平台 | Linux | x86_64（主） | `OutboxWriter` 的 POSIX `fsync` 位于 `#ifdef Q_OS_UNIX` 分支，确定性持久化测试以 Linux 为准 |

> 说明：测试可执行文件**不直接**链接 `dbridge_sqlite3`；SESSION/PREUPDATE_HOOK 宏由 `dbridge` 库依赖链传递。新增直接调用 SQLite C API（如 `sqlite3session_*`、`sqlite3changeset_*`）的测试，需确保编译期可见上述宏定义。

### 2.2 构建与运行测试

#### 2.2.1 CMake 路径（推荐，CI 主轨）

```bash
# 在仓库根目录
mkdir -p build && cd build
cmake -DBUILD_TESTING=ON ..
cmake --build . -j
# 运行全部测试套件，并行、失败即输出日志
ctest -j --output-on-failure
```

```bash
# 仅运行匹配某正则的套件（例如同步相关）
ctest -R 'tst_(temporal|reverse_lookup|column_order)' --output-on-failure
# 列出已注册的测试名（来自 add_test(NAME ...)）
ctest -N
```

#### 2.2.2 qmake 路径（影子构建 shadow build）

```bash
# 在仓库根目录外新建影子构建目录
mkdir -p build_check && cd build_check
qmake ../dbridge.pro
make -j
# 运行全部 testcase（CONFIG += testcase 提供 check 目标）
make check
# 仅运行单个套件
make sub-tst_fk_preflight-check
```

> `test-common.pri` 通过 `$$shadowed(...)` 推导构建根，并以 `PRE_TARGETDEPS` 强制依赖 `libdbridge.a` 与 `libQXlsx.a`，因此 qmake 路径**必须**使用影子构建目录，且测试套件会等待静态库先行产出。

### 2.3 新测试套件接入规范

新增一个测试套件 `tst_<area>` 时，必须**同时**完成以下三步，保证 CMake 与 qmake 双轨均可发现并运行。

**① 新建源文件与 qmake 工程文件**

`tests/unit/tst_<area>.cpp`（最小骨架）：

```cpp
#include <QtTest>

class tst_<Area> : public QObject
{
    Q_OBJECT
private slots:
    void init();            // 每用例前：建独立临时库/独立 connName
    void cleanup();         // 每用例后：关闭连接、删除临时文件
    void someBehaviour();   // 具体断言
};

void tst_<Area>::init() {}
void tst_<Area>::cleanup() {}
void tst_<Area>::someBehaviour() { QVERIFY(true); }

QTEST_GUILESS_MAIN(tst_<Area>)
#include "tst_<area>.moc"
```

`tests/unit/tst_<area>.pro`（与现有套件完全一致的三行式）：

```pro
include(../test-common.pri)
TARGET  = tst_<area>
SOURCES = tst_<area>.cpp
```

**② 在 `tests/CMakeLists.txt` 注册**（追加一行，使用现有 `add_dbridge_test` 函数）：

```cmake
add_dbridge_test(tst_<area> unit/tst_<area>.cpp)
```

> `add_dbridge_test(name sources)` 会创建可执行文件、链接 `dbridge QXlsx Qt5::Test Qt5::Sql Qt5::Core`、加入 `src` 与 `QXlsx` 头路径，并 `add_test(NAME ... COMMAND ...)`。`CMAKE_AUTOMOC ON` 已在 `tests/CMakeLists.txt` 顶部开启。

**③ 在 `tests/tests.pro` 的 `SUBDIRS` 与 `.file` 映射各加一行**：

```pro
SUBDIRS += tst_<area>
tst_<area>.file = unit/tst_<area>.pro
```

> 集成测试放在 `integration/`，对应 `.file = integration/tst_<area>.pro`（参见 `tst_import_single`）。

### 2.4 覆盖率工具链接入（关键）

#### 2.4.1 新增 CMake 选项

在根 `CMakeLists.txt` 增加 `DBRIDGE_COVERAGE` 选项，仅在开启时对**被测库 `dbridge`** 与**测试目标**注入插桩标志。示例片段：

```cmake
option(DBRIDGE_COVERAGE "Enable code coverage instrumentation" OFF)

if(DBRIDGE_COVERAGE)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "DBRIDGE_COVERAGE requires GCC or Clang")
  endif()
  # 关闭优化、保留调试信息，保证行号与覆盖数据准确
  add_compile_options(--coverage -O0 -g -fprofile-arcs -ftest-coverage)
  add_link_options(--coverage)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    link_libraries(gcov)
  endif()
endif()
```

> 仅对 `dbridge` 与测试启用插桩即可；`3rdparty`、`examples` 不应被统计（见 2.4.3 口径）。如需精确控制，可改用 `target_compile_options(dbridge PRIVATE ...)` / `target_link_options(dbridge PRIVATE ...)` 而非全局 `add_compile_options`。

配置与构建：

```bash
mkdir -p build-cov && cd build-cov
cmake -DBUILD_TESTING=ON -DDBRIDGE_COVERAGE=ON ..
cmake --build . -j
```

#### 2.4.2 采集流程

运行测试以产生 `.gcda` 数据，再用 lcov 采集、过滤、生成 HTML：

```bash
# 1) 跑测试，生成 .gcda
ctest -j --output-on-failure

# 2) 采集
lcov --capture --directory . --output-file cov.info

# 3) 过滤非被测代码（3rdparty / 测试 / moc / 构建目录 / 系统头 / 示例）
lcov --remove cov.info \
  '*/3rdparty/*' '*/tests/*' '*/moc_*' '*/build*/*' '/usr/*' '*/examples/*' \
  --output-file cov.filtered.info

# 4) 生成 HTML 报告
genhtml cov.filtered.info --output-directory coverage-html
```

或使用 gcovr 一步到位（更适合 CI 文本/XML 输出）：

```bash
gcovr -r . \
  --exclude '3rdparty' --exclude 'tests' --exclude 'examples' \
  --exclude '.*moc_.*' \
  --print-summary
```

#### 2.4.3 覆盖率统计口径

| 纳入统计 | 排除 |
| --- | --- |
| `src/**`（被测库 `dbridge` 实现代码） | `3rdparty/**`（QXlsx、SQLite amalgamation 等） |
| | `tests/**`（测试自身代码与夹具） |
| | `moc_*` / `*.moc`（Qt 元对象生成代码） |
| | `build*/**`（构建产物） |
| | `/usr/**`（系统与 Qt 头文件） |
| | `examples/**`（示例程序） |

> 口径原则：**只衡量被测库代码 `src/**` 的覆盖**。任何生成代码、第三方代码与测试代码均不计入分母，避免稀释或虚高。

### 2.5 CI 集成

CI 流水线的合并闸门由两道组成，任一不满足即判定失败：

1. **功能闸门**：`ctest -j --output-on-failure` 全绿（所有套件退出码为 0）。
2. **覆盖率闸门**：行覆盖率（line coverage）≥ **80%**，否则 CI 失败。

参考流水线步骤：

```bash
# 配置（开启测试与覆盖率）
cmake -S . -B build-cov -DBUILD_TESTING=ON -DDBRIDGE_COVERAGE=ON
cmake --build build-cov -j

# 功能闸门
ctest --test-dir build-cov -j --output-on-failure

# 覆盖率闸门：行覆盖率 < 80% 时 gcovr 以非零退出码使 CI 失败
gcovr -r . --object-directory build-cov \
  --exclude '3rdparty' --exclude 'tests' --exclude 'examples' --exclude '.*moc_.*' \
  --fail-under-line 80 \
  --xml-pretty -o coverage.xml --html-details coverage.html
```

> `--fail-under-line 80` 直接提供阈值闸门；同时产出 `coverage.xml`（供 CI 覆盖率插件解析）与 `coverage.html`（人工审阅产物归档）。

### 2.6 确定性与隔离

为消除用例间耦合与偶发性失败，所有同步工具测试**必须**遵守以下硬性约定：

| 维度 | 约定 |
| --- | --- |
| 数据库隔离 | 每个用例使用**独立临时库**（`QTemporaryDir`/`QTemporaryFile` 下的独立 `.db` 文件），用例结束即删除 |
| 连接隔离 | 每个用例使用**独立 `connName`**，以 `QUuid::createUuid().toString(QUuid::WithoutBraces)` 生成，避免 `QSqlDatabase` 同名连接复用 |
| 禁止共享 | 禁止用例间共享数据库文件、共享连接或共享全局状态；`init()`/`cleanup()` 负责逐例建/拆 |
| 时间确定性 | 时间相关逻辑一律通过可注入的 `nowMs`（见第 3 节夹具）提供固定/可控时钟，禁止直接读取真实墙钟，保证 Outbox 时间戳、changeset 顺序等可重现 |
| 持久化确定性 | 涉及 `OutboxWriter` 的 `fsync`（`#ifdef Q_OS_UNIX`）路径在 Linux 上验证；非 POSIX 平台分支以条件编译方式跳过对应断言 |

```cpp
// 典型 init()/cleanup() 隔离骨架
void tst_<Area>::init() {
    m_tmpDir.reset(new QTemporaryDir);
    QVERIFY(m_tmpDir->isValid());
    m_dbPath   = m_tmpDir->filePath("t.db");
    m_connName = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto db = QSqlDatabase::addDatabase("QSQLITE", m_connName);
    db.setDatabaseName(m_dbPath);
    QVERIFY(db.open());
}

void tst_<Area>::cleanup() {
    QSqlDatabase::database(m_connName).close();
    QSqlDatabase::removeDatabase(m_connName);  // 释放后再移除，避免 "connection still in use" 告警
    m_tmpDir.reset();                          // QTemporaryDir 析构即删除临时库
}
```
## 3. 测试夹具与公共基础设施

本节定义贯穿整个测试计划的可复用夹具与基础设施。所有夹具均基于 Qt Test 框架，遵循「写均经唯一写线程 `wconn` + `WriteTxn(BEGIN IMMEDIATE)`」与「已提交=已落 changelog 原子」的核心不变量。夹具按职责分层：底层库管理（`SqliteFixture`）→ 数据集（`tests/data/sync/`）→ 真实 changeset 录制（`ChangesetFixture`）→ 端到端拓扑（`TwoNodeHarness`）→ 崩溃语义（`CrashHarness`）→ 时间/制品注入与公共断言。

### 3.1 SQLite 测试夹具 `SqliteFixture`

**职责**：管理单个被测数据库的生命周期——建临时库、设置必需 PRAGMA、提供执行与种子入口、cleanup 时删库删连接。它是其余所有夹具的底座。

**关键决策**：

- **临时文件优先**：默认创建临时文件库（`QTemporaryDir` 下生成唯一 `.db` 路径），因为 WAL 模式与 SQLite session 扩展在 `:memory:` 上行为受限（WAL 对内存库不可用；session 需可重放磁盘表），且崩溃注入必须有真实磁盘文件可被重启进程重新打开。
- **`connName=QUuid`**：每个连接用 `QUuid::createUuid().toString()` 作为 Qt SQL 连接名，避免并发用例间连接名冲突导致 `QSqlDatabase` 句柄被意外复用。
- **PRAGMA**：建库即 `PRAGMA foreign_keys=ON;` 与 `PRAGMA journal_mode=WAL;`（临时文件库），保证 FK 链/FK 环测试与崩溃零窗口测试在生产同构的设置下运行。
- **`:memory:` 适用边界**：仅用于不依赖 WAL/session/崩溃重启的**纯 store 单测**（如 SQL 语句拼装、行映射、eligibility 静态判定的解析逻辑），此时 `:memory:` 更快且无需清理磁盘。凡涉及 session 录制、WAL 检查点、跨进程重启的用例**必须**用临时文件库。

**接口草案**：

```cpp
class SqliteFixture {
public:
    enum class Backend { TempFile, Memory };

    // 建库：默认临时文件 + WAL + foreign_keys=ON；Memory 仅用于纯 store 单测
    explicit SqliteFixture(Backend backend = Backend::TempFile);
    ~SqliteFixture();                       // cleanup：关闭连接、removeDatabase、删临时文件

    QSqlDatabase db() const;                // 取底层连接（已 open）
    QString connName() const;               // QUuid 连接名
    QString filePath() const;               // 临时文件路径（Memory 返回 ":memory:"）

    bool exec(const QString& sql, QString* err = nullptr);     // 单语句执行
    bool seed(const QString& sqlFilePath, QString* err = nullptr); // 从 .sql 脚本批量种子

    qint64 rowCount(const QString& table) const;               // 断言辅助
private:
    void applyPragmas();                    // foreign_keys=ON; WAL（临时文件库）
};
```

**用法**：

```cpp
SqliteFixture fx;                                   // 临时文件 + WAL
fx.seed(QFINDTESTDATA("data/sync/02_composite_pk.sql"));
QVERIFY(fx.exec("INSERT INTO ... VALUES (...)"));
// 用例结束析构自动清理
```

### 3.2 测试 schema 数据集 `tests/data/sync/`

集中存放覆盖各 eligibility 分支的建表脚本（`.sql`），由 `SqliteFixture::seed()` 加载。每个脚本聚焦一个判定分支，命名按下表编号前缀，便于在用例中用 `QFINDTESTDATA` 定位。

| 文件 | 内容 | 用途（覆盖的分支） |
| --- | --- | --- |
| `01_rowid_explicit_pk.sql` | 普通 rowid 表 + 显式非空单列 `PRIMARY KEY` | 合格基线表；单列 PK 作为冲突目标/winner 键 |
| `02_composite_pk.sql` | 复合主键表（`PRIMARY KEY(a,b)`） | 复合键的 PK 提取、row_winner 三元组、冲突目标拼装 |
| `03_without_rowid.sql` | `WITHOUT ROWID` 表 | session 对无 rowid 表的捕获；PK 即唯一行标识路径 |
| `04_no_explicit_pk.sql` | 无显式 PK 的 rowid 表（仅隐式 rowid） | **应被 `E_SYNC_UNSUPPORTED_SCHEMA` 拒绝**（隐式 rowid 不可跨节点稳定） |
| `05_view.sql` | 一个普通表 + 其上的 `CREATE VIEW` | 视图不可同步：eligibility 应跳过/拒绝视图对象 |
| `06_fts5_vtab.sql` | FTS5 虚表 + 其影子表（`*_data`/`*_idx` 等） | 虚表与影子表均不应被独立纳入同步（避免重复/损坏） |
| `07_generated_column.sql` | 含 `GENERATED ALWAYS AS (...) [STORED|VIRTUAL]` 列的表 | 生成列在 changeset/模板里须被正确排除或只读处理 |
| `08_fk_chain.sql` | 父子 FK 链 `orders` → `order_items(order_id REFERENCES orders)` | FK 拓扑排序、应用顺序（父先于子）、级联约束 |
| `09_fk_cycle.sql` | FK 环（A→B→A，借 deferred FK 建成） | **触发 `E_SYNC_FK_CYCLE_UNSUPPORTED`**：拓扑排序无解 |
| `10_partial_expr_unique.sql` | 仅含 partial unique index 与表达式 unique index 的表（无普通 UNIQUE/PK 可作冲突目标） | **无可用冲突目标**：upsert 冲突子句无法构造，应被拒绝或降级 |

**约定**：每个 `.sql` 文件首行用 `-- purpose:` 注释声明用途；脚本只建 schema 与必要静态种子行，动态写入由用例或 `ChangesetFixture` 完成，保证脚本可被多个用例共享。

### 3.3 changeset 构造辅助 `ChangesetFixture`

**职责**：用**真实**启用了 SESSION 扩展的连接对种子表做真实写，并经 `sqlite3session_changeset()` 录制出真实 changeset blob。供 `ChangesetApplier`、`CapturedWriteTemplate`、`PayloadCodec` 等用真实字节流测试，杜绝手搓字节导致的与 SQLite 内部格式不一致。

**关键点**：

- 内部使用与生产同构的**短命 session**：`sqlite3session_create` → `attach(表)` → 执行 `mutateFn` 内的写 → `sqlite3session_changeset` → `delete`。
- 录制在调用方提供的 `db` 上进行，使 changeset 的表结构与 `tests/data/sync` 数据集一致。
- 同时提供 patchset 录制变体（`recordPatchset`），用于验证两种格式分支。

**接口草案**：

```cpp
class ChangesetFixture {
public:
    using MutateFn = std::function<bool(QSqlDatabase&, QString* err)>;

    // 在 db 上对 tables 开短命 session，执行 mutateFn 的写，返回 changeset blob
    static QByteArray recordChangeset(QSqlDatabase& db,
                                      const QStringList& tables,
                                      const MutateFn& mutateFn,
                                      QString* err = nullptr);

    // patchset 变体（仅含变更后的值，用于幂等/最终态语义测试）
    static QByteArray recordPatchset(QSqlDatabase& db,
                                     const QStringList& tables,
                                     const MutateFn& mutateFn,
                                     QString* err = nullptr);

    // 反向：把 changeset 取反，构造回滚/逆向用例
    static QByteArray invert(const QByteArray& changeset, QString* err = nullptr);
};
```

**用法**：

```cpp
SqliteFixture fx;
fx.seed(QFINDTESTDATA("data/sync/02_composite_pk.sql"));
QByteArray cs = ChangesetFixture::recordChangeset(
    fx.db(), {"t_composite"},
    [](QSqlDatabase& db, QString* err) {
        QSqlQuery q(db);
        return q.exec("INSERT INTO t_composite(a,b,v) VALUES(1,2,'x')");
    });
// cs 直接喂给 ChangesetApplier / PayloadCodec 编解码用例
```

### 3.4 两节点 harness `TwoNodeHarness`

**职责**：构造两个独立节点 A（center）与 B（edge），各自拥有独立库 + `wconn` + outbox/inbox 目录，交叉指向（`A.outboxDir == B.inboxDir`，`B.outboxDir == A.inboxDir`），用于端到端收敛测试。可扩展为三/四节点用于星型拓扑。

**关键点**：

- **手动驱动**：不依赖真实定时器/线程调度，由用例显式调用 `drain`/`deliver` 推进，使测试确定可重放。
- **`drain(node)`**：在指定节点上执行一轮——扫该节点 inbox 的 `.ready` 哨兵 → 应用 payload（经 `wconn` + `WriteTxn`）→ 把本节点新变更打包写入自己的 outbox（`<origin>__<epoch>__<kind>__<seq>__<uuid>.payload` + 同名 `.ready`）。
- **`deliver()`**：模拟第三方传输——把每个节点 outbox 中已 `.ready` 的制品搬到对端 inbox（先搬 `.payload` 再落 `.ready`，保证哨兵原子可见）。
- **`converged()`**：断言两库 `table_state` 三元组（key, value/版本, winner）逐表逐行一致，作为收敛终态判据。

**接口草案**：

```cpp
class TwoNodeHarness {
public:
    struct Node {
        QString name;                       // "A"(center) / "B"(edge)
        std::unique_ptr<SqliteFixture> store;
        WriteConn* wconn;                   // 唯一写线程连接
        QString outboxDir;
        QString inboxDir;                   // 交叉指向：A.inbox==B.outbox
    };

    TwoNodeHarness();                       // 建 A/B，交叉绑定 outbox/inbox
    Node& node(const QString& name);

    void drain(const QString& name);        // 扫 inbox→应用→打包发 outbox
    void deliver();                         // outbox 制品搬到对端 inbox（模拟传输）

    // 反复 drain+deliver 直到无新制品或达到 maxRounds
    void runUntilQuiescent(int maxRounds = 8);

    bool converged(QString* diff = nullptr) const; // table_state 三元组一致

protected:
    void addNode(const QString& name, NodeRole role); // 供三/四节点星型扩展
};

// 星型扩展示例：center C 连接多个 edge D/E
class StarHarness : public TwoNodeHarness { /* center C/D... */ };
```

**用法**：

```cpp
TwoNodeHarness h;
applyWrite(h.node("A"), /* 某变更 */);
h.runUntilQuiescent();
QString diff;
QVERIFY2(h.converged(&diff), qPrintable(diff));
```

### 3.5 崩溃注入夹具 `CrashHarness`（FR-1 零窗口，plan T1.0a）

**职责**：以**子进程模型**验证「已提交=已落 changelog」原子不变量——被测程序在指定注入点被 `SIGKILL`，父进程重启同一库后断言所有相关结构原子一致。

**子进程模型**：

- 父进程（测试用例）派生子进程运行被测「一次写入」流程，通过环境变量 `DBRIDGE_FAULT_INJECT` 指定注入点：
  - `before_commit`：`COMMIT` 前 kill —— 期望**全未发生**。
  - `after_seal`：`sealInto __sync_changelog` 后、`COMMIT` 前 kill —— 因 seal 与业务写同事务，仍期望**全未发生**（事务未提交）。
  - `after_commit`：`COMMIT` 后、后续标记前 kill —— 期望**全发生**（业务行 + changelog 已原子落盘）。
  - `after_markconsumed`：标记 inbox 已消费后 kill —— 期望全发生且 ledger 已更新。
- 子进程在注入点调用 `raise(SIGKILL)`（或 `_exit` 模拟硬崩），父进程 `waitpid` 后重启进程打开同库做断言。

**编译期 hook（对实现的测试性要求）**：注入点须由源码侧的编译期 hook 提供，被测程序在 `DBRIDGE_FAULT_INJECTION` 定义时插入崩溃点：

```cpp
#ifdef DBRIDGE_FAULT_INJECTION
  #define DBRIDGE_FAULT_POINT(name) ::dbridge::faultPoint(name)
#else
  #define DBRIDGE_FAULT_POINT(name) ((void)0)
#endif
// 实现处：
//   WriteTxn txn(wconn);
//   applyBusinessRows(...);
//   sealInto(changelog, ...);
//   DBRIDGE_FAULT_POINT("after_seal");
//   txn.commit();
//   DBRIDGE_FAULT_POINT("after_commit");
```

`dbridge::faultPoint(name)` 读取 `DBRIDGE_FAULT_INJECT`，命中则 `SIGKILL` 自身。**此项列为对被测实现的明确测试性要求**：实现必须在上述四个注入点埋设 `DBRIDGE_FAULT_POINT`，并通过 `DBRIDGE_FAULT_INJECTION` 编译宏门控（生产构建关闭，零开销）。

**接口草案**：

```cpp
class CrashHarness {
public:
    enum class Point { BeforeCommit, AfterSeal, AfterCommit, AfterMarkConsumed };

    explicit CrashHarness(const QString& dbPath); // 复用同一临时文件库

    // 派生子进程跑一次写入流程，在 point 处被 SIGKILL；返回子进程退出状态
    int runChildUntilCrash(Point point, const QString& workloadSpec);

    // 重启后断言五结构原子一致：要么全发生要么全未发生
    struct AtomicityView {
        bool businessRowsPresent;
        bool changelogPresent;
        bool appliedVectorAdvanced;
        bool rowWinnerPresent;
        bool inboxLedgerConsumed;
    };
    AtomicityView reopenAndInspect() const;       // 重启同库读取五结构状态

    // 便捷断言：所有标志一致（全 true 或全 false）
    static bool isAtomic(const AtomicityView& v);
};
```

**断言对象（五结构原子集）**：业务行、`__sync_changelog`、`__sync_applied_vector`、`__sync_row_winner`、`__sync_inbox_ledger`。`before_commit`/`after_seal` 期望五者均「未发生」；`after_commit`/`after_markconsumed` 期望均「已发生」。

**用法**：

```cpp
CrashHarness ch(fx.filePath());
ch.runChildUntilCrash(CrashHarness::Point::AfterSeal, "insert_one");
auto v = ch.reopenAndInspect();
QVERIFY(CrashHarness::isAtomic(v));
QVERIFY(!v.changelogPresent);   // after_seal：事务未提交，全未发生
```

### 3.6 ACK 制品与时间注入

**职责**：构造 `ChangesetAck`/`PushChunkAck` 制品 helper，并提供可控时钟 `nowMs` 注入，避免真实 `sleep` 拖慢套件、消除时序抖动。

**可控时钟**：被测时间相关组件（`AckChannel` 的 `ackMaxDelayMs`、`InboxLedger` 的 stalePending `gapTimeout`、`DeadPeerEvictor` 阈值）须接受可注入的时钟函数（`std::function<qint64()> nowMs`），测试通过 `FakeClock` 推进虚拟时间，无需真实等待。

**接口草案**：

```cpp
class FakeClock {
public:
    qint64 nowMs() const { return cur_; }
    void advance(qint64 ms) { cur_ += ms; }
    std::function<qint64()> fn() { return [this]{ return cur_; }; }
private:
    qint64 cur_ = 0;
};

struct AckArtifacts {
    // 构造一个 ChangesetAck 制品（确认已应用某 epoch/seq 范围）
    static QByteArray changesetAck(const QString& origin, qint64 epoch,
                                   qint64 ackedSeq, QString* err = nullptr);
    // 构造一个 PushChunkAck 制品（确认收到某分块）
    static QByteArray pushChunkAck(const QString& origin, qint64 chunkSeq,
                                   bool ok, QString* err = nullptr);
};
```

**用法**：

```cpp
FakeClock clk;
AckChannel ch(clk.fn(), /*ackMaxDelayMs=*/500);
ch.onReceive(AckArtifacts::changesetAck("A", 7, 42));
clk.advance(600);                 // 越过 ackMaxDelayMs，无需真实 sleep
QVERIFY(ch.shouldFlush());
```

### 3.7 公共断言宏

**职责**：统一错误码断言写法——断言被测调用返回 `false` 且回填的 `*err` 含指定 `E_SYNC_XXX` 码，避免在每个用例里重复 `QVERIFY(!ok) + QVERIFY(err.contains(...))`。

**接口草案**：

```cpp
// 约定：被测 API 形如 bool fn(..., QString* err)，失败时 *err 含错误码字符串
#define ASSERT_ERR_CODE(call, code)                                    \
    do {                                                               \
        QString _err;                                                  \
        bool _ok = (call);                                             \
        QVERIFY2(!_ok,                                                 \
            qPrintable(QStringLiteral("expected failure for %1, got success") \
                .arg(#call)));                                         \
        QVERIFY2(_err.contains(QStringLiteral(code)),                  \
            qPrintable(QStringLiteral("expected %1 in error, got: %2") \
                .arg(code, _err)));                                    \
    } while (0)
// 注意：被测调用需以局部名 _err 取地址，见下例
```

**用法**：

```cpp
// 评估 04_no_explicit_pk.sql 的表应被拒
ASSERT_ERR_CODE(checkEligibility(fx.db(), "t_no_pk", &_err),
                "E_SYNC_UNSUPPORTED_SCHEMA");

// 09_fk_cycle.sql 的 FK 环应被拒
ASSERT_ERR_CODE(topoSortTables(fx.db(), &_err),
                "E_SYNC_FK_CYCLE_UNSUPPORTED");
```

### 3.8 夹具用途汇总表

| 夹具 | 主要职责 | 典型适用测试 | 库后端 |
| --- | --- | --- | --- |
| `SqliteFixture` | 临时库/PRAGMA/exec/seed/cleanup | 全部需要真实库的用例底座 | 临时文件（默认）/`:memory:`（纯 store） |
| `tests/data/sync/` | eligibility 分支建表脚本 | 模式判定、冲突目标、FK 拓扑 | 配合 `SqliteFixture::seed` |
| `ChangesetFixture` | 真实 session 录制 changeset/patchset | `ChangesetApplier`/`CapturedWriteTemplate`/`PayloadCodec` | 临时文件（session 需磁盘表） |
| `TwoNodeHarness` | 双/星型节点交叉 outbox/inbox + 收敛断言 | 端到端收敛、传输、星型拓扑 | 每节点独立临时文件 |
| `CrashHarness` | 子进程注入点 SIGKILL + 重启原子断言 | FR-1 零窗口、五结构原子一致 | 临时文件（跨进程重开） |
| `FakeClock`/`AckArtifacts` | 可控时钟 + ACK 制品构造 | `AckChannel`/`InboxLedger`/`DeadPeerEvictor` | 与所属组件一致 |
| `ASSERT_ERR_CODE` | 错误码断言宏 | 所有期望失败 + 指定 `E_SYNC_XXX` 的用例 | 不限 |
## 4. 阶段0可行性闸门与 capture 子系统单元测试

本节覆盖 `src/sync/capture/**`（376 行，目标行覆盖率 **≈88%**）以及位于其上游的**阶段 0 可行性闸门**（设计 §13.1，硬验收：任一不过 → 阶段 0 失败、停止实施、**无运行时降级**）。capture 子系统是整个同步链路的「第一公里」——它把 SQLite session 扩展的真实字节流（changeset blob）录入 `__sync_changelog`，并以「已提交=已落 changelog 原子」为不变量。因此本节用例分两类：

- **闸门类**（`tst_phase0_session_feasibility`）：验证底层 SQLite 构建与句柄穿透是否真正具备 session/rebaser 能力，是后续一切 capture/apply 测试的前置条件，不计入 capture 覆盖率分母但**必须先绿**。
- **单元类**（`tst_sqlite_handle`、`tst_session_recorder`、`tst_changelog_store`）：逐方法验证 `SqliteHandle` / `SessionRecorder` / `ChangelogStore` 的契约分支，是 capture 覆盖率的主要来源。

被测三件套与公共契约：

| 文件 | 公共方法 | 核心契约 |
| --- | --- | --- |
| `SqliteHandle.{h,cpp}` | `static sqlite3* of(QSqlDatabase&)`、`static bool sessionAvailable(sqlite3*)`、`exerciseSession(...)`、`libVersion()` | 从 Qt 连接取裸句柄；运行期探测 `ENABLE_SESSION`/`ENABLE_PREUPDATE_HOOK` 编译选项 |
| `SessionRecorder.{h,cpp}` | `begin`、`sealInto`、`abort`、`isActive`、`collectChangeset`(private) | **不拥有事务**（由 `WriteTxn` 持）；`sealInto` 在 `COMMIT` 前同事务写 `__sync_changelog`；seal 前先 detach session 避免把 changelog INSERT 自录进 changeset |
| `ChangelogStore.{h,cpp}` | `init`、`append`、`appendForward`、`readRange`、`readRangeAll`、`maxLocalSeq`、`truncate` | `append`=本地 fresh 捕获（`authoritative=true`）；`appendForward`=原 blob 转发（`kind="forward"`、`authoritative=false`、**origin 不重铸**）；`local_seq` PK AUTOINCREMENT 单调；`UNIQUE(origin,stream_epoch,origin_seq)` 去重 |

> 库后端约定：凡触及 session 录制（`SessionRecorder`、`ChangesetFixture`、`exerciseSession`）的用例**必须**用 `SqliteFixture::Backend::TempFile`（WAL + 磁盘表，见 §3.1）；纯 store 行（`ChangelogStore` 的 SQL 拼装、读区间过滤）可用 `:memory:` 加速。所有用例遵循 §2.6 的「独立临时库 + 独立 `connName`」隔离约定；期望失败的断言统一用 §3.7 的 `ASSERT_ERR_CODE`；真实 changeset 一律经 §3.3 `ChangesetFixture::recordChangeset` 录制，禁止手搓字节。

---

### 4.1 阶段 0 可行性闸门 `tst_phase0_session_feasibility`

**定位**：设计 §13.1 五条硬验收清单的可执行化。它不验证 dbridge 业务逻辑，而验证「Qt 5.12 的 QSQLITE 插件实际链接的 SQLite 是否真正启用了 session/preupdate/rebaser」。任一条失败即证明 SQLite 构建方案（§2.1：经 `dbridge` 依赖链传递 `-DSQLITE_ENABLE_SESSION -DSQLITE_ENABLE_PREUPDATE_HOOK`）未落地，阶段 0 不通过、停止实施。

**前置说明（与 §13.1 第 5 条对应）**：测试可执行文件不直接链接 `dbridge_sqlite3`，session 符号经 `dbridge` 依赖链传递。本套件直接调用 SQLite C API（`sqlite3session_*` / `sqlite3changeset_*` / `sqlite3rebaser_*`），编译期须可见上述宏；若链接到的是系统 QSQLITE（几乎肯定未启用 session），`sessionAvailable` 即返回假，初始化路径须报 `E_SYNC_SESSION_UNAVAILABLE`（设计 §4.6，「compile_options 缺宏 → 必报」）。

| private slot | 验收点(§13.1) | 断言要点 |
| --- | --- | --- |
| `compileOptionsContainSessionMacros()` | ①②（构建+运行期探测） | 遍历 `PRAGMA compile_options` 结果集，`QVERIFY` 含 `ENABLE_SESSION` **且** 含 `ENABLE_PREUPDATE_HOOK`；`QCOMPARE(SqliteHandle::sessionAvailable(h), true)`；日志打印 `SqliteHandle::libVersion()` 以备 CI 归档。**这条是其余各条的守门**——若不绿，本套件其余 slot 直接 `QSKIP` 并标记阶段 0 失败 |
| `handlePassthroughCreateAttachChangeset()` | ③（同一 QSqlDatabase 取裸句柄可调 session API） | `sqlite3* h = SqliteHandle::of(fx.db())`，`QVERIFY(h != nullptr)`；`sqlite3session_create(h,"main",&s)==SQLITE_OK`；对种子表 `sqlite3session_attach(s,"t_pk")==SQLITE_OK`；INSERT 一行后 `sqlite3session_changeset(s,&n,&p)==SQLITE_OK` 且 `n>0 && p!=nullptr`；`sqlite3_free(p)`/`sqlite3session_delete(s)` 无泄漏 |
| `applyV2WithConflictCallbackRunsThrough()` | ④（apply_v2 + 冲突回调跑通） | 用 `ChangesetFixture::recordChangeset` 在源连接录一段对 `t_pk` 的 changeset；目标连接预置冲突行（同 PK 不同值）；`sqlite3changeset_apply_v2(h, n, p, /*filter=*/nullptr, conflictCb, ctx, &rebase, &nRebase, 0)==SQLITE_OK`；冲突回调被调用次数 `>=1` 且收到 `SQLITE_CHANGESET_DATA`/`_CONFLICT`；回调返回 `SQLITE_CHANGESET_REPLACE` 后目标行被改为入站值 |
| `rebaserChainConvergesOnTwoWayConflict()` | ④（rebaser 链路：apply_v2 收集 rebase buffer → `sqlite3rebaser_*`） | apply_v2 产出非空 rebase buffer（`nRebase>0`）；`sqlite3rebaser_create`→`sqlite3rebaser_configure(rebase buffer)`→`sqlite3rebaser_rebase(待广播 changeset)`→`sqlite3rebaser_delete` 全程 `SQLITE_OK`；变基后的 changeset 重放到第二目标库，与中心终态逐行一致 |
| `rebaserConvergesUnderReorderedArrival()` | ④（反序到达收敛） | 两路冲突变更 B、D 以 `(B,D)` 与 `(D,B)` 两种到达序分别施加于两个独立目标库；各经 apply_v2+rebaser 后 `converged()`（§3.4 三元组判据）为真，**两序终态一致**——证明 rebaser 链路对到达序无关 |
| `rowWinnerPrototypeLowRankLateNoOverwrite()` | ⑤（`__sync_row_winner` 最小原型） | 最小原型表 `__sync_row_winner(table_name,pk_hash,winning_rank,winning_origin_seq,...)`；先施加高 rank 来源（`rank(B)=2`）写入胜者；再**跨批后到**低 rank 来源（`rank(D)=1`），按 §5.6 裁决式 `(in_rank,in_seq) >= (cur.rank,cur.seq)` 判定 → `QVERIFY` 低 rank D **不**覆盖、行值仍为 B、`winning_rank` 仍为 2、`winning_origin` 仍为 B；反向（先低后高）则 B 后到时覆盖 D。验证「低 rank 跨批后到不覆盖高 rank」（FR-6 到达序无关的最小证据） |
| `missingMacroReportsSessionUnavailable()` | 负向（缺宏即拒） | 模拟/构造 `sessionAvailable(h)==false` 场景（系统 QSQLITE 或注入），`ASSERT_ERR_CODE(initializeProbe(fx.db(), &_err), "E_SYNC_SESSION_UNAVAILABLE")`；断言探测发生在 attach/进入同步模式**之前**（设计 §5.1：先于一切 attach） |

**关联需求/错误码**：FR-1（捕获）、FR-6（多源仲裁到达序无关）、FR-9（广播/rebase）；设计 §13.1（D-03/D-13/D-17）、§5.6（G-01 逐行胜者）、§7.4（下行 rebase）。错误码：`E_SYNC_SESSION_UNAVAILABLE`（缺宏必报）、`E_SYNC_REBASE_FAILED`（rebaser 链路失败，G-09，在 `rebaserChain*` slot 中以注入失败旁证一次）。

**目标覆盖率**：不计入 capture 88% 分母（属底层库/原型探测）；作为**闸门**，要求全 slot 绿（`compileOptionsContainSessionMacros` 失败时其余 `QSKIP`，但 CI 闸门判该套件失败 → 阻断合并）。

---

### 4.2 `tst_sqlite_handle`

**被测**：`SqliteHandle::of()`、`sessionAvailable()`（及辅助 `exerciseSession`/`libVersion`）。聚焦「裸句柄穿透」与「编译选项探测真/假分支」两条契约。

| private slot | 断言要点 | 覆盖目标 |
| --- | --- | --- |
| `ofReturnsNonNullForOpenDb()` | `SqliteHandle::of(fx.db())` 返回非空 `sqlite3*`；对返回句柄直接 `sqlite3session_create(h,"main",&s)==SQLITE_OK`（证明句柄确为该连接底层句柄、可调 session API） | `of()` 正常分支 |
| `ofReturnsNullForInvalidHandle()` | 对未 open / driver `handle()` 返回 invalid `QVariant` 的连接，`of()` 返回 `nullptr`（命中 `!v.isValid()` 早退分支） | `of()` 异常分支 |
| `sessionAvailableTrueWhenMacrosPresent()` | 在阶段 0 已通过的构建下，`QCOMPARE(SqliteHandle::sessionAvailable(of(fx.db())), true)`（命中两个 `compileoption_used` 均真的与路径） | `sessionAvailable` 真分支 |
| `sessionAvailableFalseForNullHandle()` | `QCOMPARE(SqliteHandle::sessionAvailable(nullptr), false)`（命中 `!h` 早退分支，覆盖空句柄假分支，无需重编缺宏的 SQLite） | `sessionAvailable` 假分支（空句柄） |
| `exerciseSessionSucceedsAndFailsPerTable()` | 正向：`exerciseSession(h,{"t_pk"},&err)` 返回真；负向：对不存在表 `exerciseSession(h,{"no_such"},&err)` 返回假且 `err` 含 `sqlite3session_attach failed`（覆盖 attach 失败 detach 清理分支） | `exerciseSession` 真/假分支（H-03 运行期探测，证明 PREUPDATE hook 真实可用而非仅看编译标志） |

**关联需求/错误码**：FR-2（同步表/句柄）；设计 §2.4 SqliteHandle、§13.1 ③。`sessionAvailable` 假分支与缺宏路径最终汇入 `E_SYNC_SESSION_UNAVAILABLE`（由调用方 `initialize` 报，本套件只验证探测返回值，不重复报码断言）。

**目标覆盖率**：`SqliteHandle.cpp` 行覆盖 **≥90%**（短文件，分支少，唯一难达的是「缺宏使 `compileoption_used` 返回假」的真实重编路径——以空句柄假分支替代覆盖，不要求重编无 session 的 SQLite）。

---

### 4.3 `tst_session_recorder`

**被测**：`SessionRecorder` 的 `begin`/`sealInto`/`abort`/`isActive`/`collectChangeset` 全生命周期。核心契约：录制器**不拥有事务**（`WriteTxn` 由用例显式持有并 `begin/commit`），`sealInto` 在 `COMMIT` 前同事务把真实 changeset 写入 `__sync_changelog`；seal 前先 `sqlite3session_delete` detach，避免把 changelog INSERT 自录进 changeset。真实 changeset 经 §3.3 `ChangesetFixture`／或直接在录制器活跃期间执行真实写产生。

| private slot | 断言要点 | 关联契约 |
| --- | --- | --- |
| `beginActivatesSession()` | `begin(h,{"t_pk"},&err)` 返回真，`isActive()==true`；重复 `begin` 返回假且 `err` 含 `already active`（命中 `session_!=nullptr` 早退） | begin 正常 + 重入保护 |
| `beginRejectsNullHandle()` | `begin(nullptr,{"t_pk"},&err)` 返回假、`err` 含 `null sqlite3 handle`、`isActive()==false` | begin 空句柄分支 |
| `beginRejectsBadTableDetaches()` | `begin(h,{"no_such"},&err)` 返回假、`err` 含 `attach failed`、`isActive()==false`（attach 失败后 `sqlite3session_delete` + 置空清理） | begin attach 失败清理分支 |
| `sealIntoProducesRealChangesetSameTxn()` | `WriteTxn txn(wconn); txn.begin();` → `begin(h,...)` → 用例真实 INSERT 两行 → `sealInto(h,store,db,txn,origin="A",epoch,schemaVer,schemaFp,parentSeq,originSeq,&seq,&err)` 返回真、`seq>0`；**`txn.commit()` 之前** 在同事务内 `readRange(db,"A",0)` 已能读到该条 changelog（证明同事务写、未提交可见）；commit 后 `changeset` blob 非空且可被 `sqlite3changeset_apply_v2` 重放复现两行 | sealInto 同事务写 changelog（FR-1） |
| `sealIntoExposesRawChangesetOut()` | 传入 `outChangeset` 非空指针，`sealInto` 回填的 blob 与 `__sync_changelog.changeset` 字节一致（M-01：供增量 table_state 更新） | sealInto outChangeset 旁路 |
| `sealIntoEmptyWriteReturnsSeqZero()` | `begin` 后**无任何业务写**直接 `sealInto`：返回真、`*outLocalSeq==0`、`__sync_changelog` 行数不变（H-06：`collectChangeset` 返回非空 empty QByteArray，走「无变更」快路径，不写 changelog） | 空写无变更分支（isNull=错误 vs isEmpty=无变更 的区分） |
| `sealIntoRequiresActiveSessionAndTxn()` | 未 `begin` 即 `sealInto` → 假、`err` 含 `not active`；`begin` 后但 `txn` 未 `begin` 即 `sealInto` → 假、`err` 含 `WriteTxn not active`（两条前置守卫分支） | sealInto 前置校验 |
| `abortLeavesNoTrace()` | `begin` + 真实写后 `abort()`：`isActive()==false`、`__sync_changelog` 行数为 0、业务行因 `txn.rollback()` 亦回滚（abort 只丢弃 session，不触事务；用例显式 rollback 验证「无痕」） | abort 不留痕 |
| `dtorAbortsActiveSession()` | 作用域内 `begin` 后不 `seal`/`abort` 直接析构：析构调用 `abort()`，后续在同 `h` 上 `begin` 成功（证明前一 session 已释放，无「already active」残留） | 析构兜底 abort |
| `multiTableAttachCapturesAll()` | `begin(h,{"t_pk","t_composite"},&err)`，对两表各写一行后 `sealInto`：复现的 changeset 同时含两表变更（多表 attach 全覆盖） | 多表 attach |

**关联需求/错误码**：FR-1（捕获/changelog 同事务，崩溃后无「已提交未捕获」）；设计 §5.2 `SessionRecorder.sealInto`、§6.1 `__sync_changelog`。本套件不直接断错误码（`SessionRecorder` 失败回填的是描述性 `err`，非 `E_SYNC_*`），故用 `QVERIFY(!ok) + err.contains(...)` 而非 `ASSERT_ERR_CODE`。`sealInto` 内部 `store.append` 失败（如 UNIQUE 冲突）透传 `err` 由 `tst_changelog_store` 覆盖。

**目标覆盖率**：`SessionRecorder.cpp` 行覆盖 **≥90%**；含 `collectChangeset` 的三条返回（错误 null / 无变更 empty / 有变更）、`begin` 三分支、`sealInto` 两守卫 + 空写快路径 + 正常路径、`abort`/析构。

---

### 4.4 `tst_changelog_store`

**被测**：`ChangelogStore` 的 `init`/`append`/`appendForward`/`readRange`/`readRangeAll`/`maxLocalSeq`/`truncate`。本套件不依赖 session，可用 `:memory:`（先建 `__sync_changelog` schema，DDL 见设计 §6.1）。changeset blob 用 `ChangesetFixture` 录制的真实字节或任意非空字节占位（store 层不解析内容，只持久化）。核心区分点：**`append` 与 `appendForward` 的语义差异**——前者记本地 fresh 捕获（`kind="changeset"`、`authoritative=true`、可带 `parentSeq`/`pushId`），后者原 blob 转发（`kind="forward"`、`authoritative=false`、`parentSeq=0`、**origin 原样保留不重铸**，FR-9/G-03 防止入站 changeset 被重捕成本节点 origin）。

| private slot | 断言要点 | 关联契约 |
| --- | --- | --- |
| `initSucceedsOnExistingSchema()` | 预建 `__sync_changelog` 后 `init(db,&err)` 返回真；缺表时返回假、`err` 非空（`SELECT ... WHERE 0` 探测分支） | init 真/假 |
| `appendRecordsAuthoritativeChangeset()` | `append(db,"changeset","A",sourcePeer="",originSeq=1,parentSeq=0,epoch=7,...,authoritative=true,&seq,&err)` 返回真、`seq>0`；查回行 `kind="changeset"`、`origin="A"`、`authoritative=1`、`payload_checksum` 为 changeset 的 SHA-256 hex、`byte_size==changeset.size()`、`parent_seq` 因传 0 存为 NULL | append 本地捕获语义 |
| `appendForwardPreservesOriginAndBlob()` | 入站来自 origin `B` 的 blob，本节点 `appendForward(db,origin="B",sourcePeer="C",originSeq=5,epoch=3,...,blob,&seq,&err)`：查回 `kind="forward"`、`origin="B"`（**不重铸为本节点**）、`source_peer="C"`、`authoritative=0`、`parent_seq` 为 NULL、`changeset` 字节与入站 blob **逐字节相等**（原 blob 转发，非 fresh 捕获）——这是 `append` 与 `appendForward` 的核心区别断言 | appendForward origin 不重铸（FR-9/G-03） |
| `appendVsForwardDistinctKindAuthoritative()` | 对同一 blob 分别 `append`（A 本地）与 `appendForward`（B 转发）写入：两行 `kind`/`authoritative` 互异（`changeset/1` vs `forward/0`），证明二者走不同语义分支 | 二者差异汇总 |
| `readRangeFiltersByOriginAndAfterSeq()` | 写入 origin A 的 `origin_seq` 1/2/3 与 origin B 的 1/2；`readRange(db,"A",afterOriginSeq=1,limit)` 仅返回 A 的 seq 2、3（按 `origin=? AND origin_seq>?` 过滤、`ORDER BY origin_seq ASC`），不含 A.seq1、不含任何 B 行 | readRange (origin,afterOriginSeq) 过滤 |
| `readRangeAllExcludesOriginAntiEcho()` | 写入 A、B、C 多行；本节点 origin=B 调 `readRangeAll(db,excludeOrigin="B",afterLocalSeq=0,limit)`：结果**不含任何 B 行**（防回声 C2/J-01），含 A、C 行，按 `local_seq ASC` FIFO，且 `EntryFull.streamEpoch`/`pushId` 随各 origin 原值回填（C-04/H-01） | readRangeAll 排除 excludeOrigin（防回声 C2） |
| `readRangeAllAfterLocalSeqWatermark()` | 以上一轮 `maxLocalSeq` 为 `afterLocalSeq`，仅返回其后新增行（`local_seq > afterLocalSeq`），验证增量水位续读不重发 | readRangeAll 水位续读 |
| `uniqueOriginEpochSeqRejectsDuplicate()` | 先成功 `append(origin="A",epoch=7,originSeq=1)`；再以同 `(A,7,1)` 三元组 `append`：返回假、`err` 含约束冲突（plain INSERT 非 OR IGNORE，H-01：重复触发真实错误而非静默吞）；改 `originSeq=2` 或 `epoch=8` 则成功（验证去重键为 `(origin,stream_epoch,origin_seq)`） | UNIQUE(origin,epoch,origin_seq) 去重 |
| `maxLocalSeqMonotonicAndEmpty()` | 空表 `maxLocalSeq` 返回 **-1**；连续 `append` 后 `maxLocalSeq` 单调递增且等于最后一次 `*localSeqOut`（`local_seq` PK AUTOINCREMENT 保证单调，即使中间 `truncate` 删除低位行 AUTOINCREMENT 不回退） | maxLocalSeq 单调 + 空表 |
| `readRangeEmptyReturnsEmptyList()` | 对不存在的 origin、或 `afterOriginSeq` 超过最大 seq、或 `afterLocalSeq` 超过 `maxLocalSeq`：`readRange`/`readRangeAll` 返回空 `QList`（非错误），`isEmpty()==true` | 空区间返回空 |

**关联需求/错误码**：FR-9（广播/转发不重铸 origin）、FR-1（changelog 落盘）、共识 C2（防回声）、G-03（入站 changeset 原样保留 origin 供转发/rebase）、J-01（`readRangeAll` 不回声 + 含本地变更）、H-01/C-04（push_id/stream_epoch 随原值保留）。`uniqueOriginEpochSeqRejectsDuplicate` 的约束错误以 `QVERIFY(!ok)+err.contains` 断言（SQLite 约束文案，非 `E_SYNC_*`）。

**目标覆盖率**：`ChangelogStore.cpp` 行覆盖 **≥88%**；含 `init` 真假、`insertRow`（被 `append`/`appendForward` 共用，覆盖 NULL 绑定分支：`sourcePeer`/`parentSeq`/`pushId` 空→NULL）、`readRange`/`readRangeAll`/`maxLocalSeq`/`truncate` 的成功与空结果分支、`blobChecksum`。`truncate` 由 `maxLocalSeqMonotonicAndEmpty` 顺带覆盖（删低位后 AUTOINCREMENT 不回退）。

---

### 4.5 capture 子系统覆盖率小结

| 套件 | 被测文件 | 计入 capture 分母 | 目标行覆盖率 | 主要未覆盖项（可接受残留） |
| --- | --- | --- | ---: | --- |
| `tst_phase0_session_feasibility` | 底层 SQLite + rebaser 原型 | 否（闸门） | 全 slot 绿 | 缺宏真实重编路径（以负向 slot 旁证） |
| `tst_sqlite_handle` | `SqliteHandle.cpp` | 是 | ≥90% | 缺宏使 `compileoption_used` 返假的真实重编 |
| `tst_session_recorder` | `SessionRecorder.cpp` | 是 | ≥90% | `sqlite3session_changeset` 返回非 OK 的硬错误（极难诱发） |
| `tst_changelog_store` | `ChangelogStore.cpp` | 是 | ≥88% | `q.exec` 因磁盘满/IO 错误失败的极端分支 |
| **capture 合计** | 376 行 | — | **≈88%** | 与 §1.5 预算（capture 88% / 331 命中行）一致 |

> 闸门优先级：CI 流水线中 `tst_phase0_session_feasibility` 须**先于**其余三个 capture 套件运行（`ctest -R tst_phase0_session_feasibility` 单独前置）；其失败应短路后续 capture/apply 套件的判定，因为「session 不可用」会使所有真实 changeset 用例无意义地连环失败，应直接归因到阶段 0 闸门而非各单元套件。
## 5. payload 编解码与 anchor 锚点单元测试

本节覆盖 `src/sync/payload`（352 行，目标行覆盖率 ≈92%）与 `src/sync/anchor`（155 行，目标行覆盖率 ≈92%）两个纯逻辑/位点存储模块。前者是所有线上制品（changeset / selection-push / baseline 请求与响应 / 两类 ACK）在线缆上的二进制编解码层，是跨节点字节兼容与防篡改的第一道闸；后者是**发送端**水位锚点（`__sync_outbound_ack`），与接收端 `__sync_applied_vector` 严格分层（F-05 / D-19），驱动 changelog 截断与广播下界推进。

两套件均为纯逻辑/单库单元测试：`tst_payload_codec` 不需真实库（`PayloadCodec` 仅做字节编解码），但 ② selection-push 与 ④ ACK 往返用 `ChangesetFixture` 录制的**真实** changeset blob 作为载荷，杜绝手搓字节与 SQLite 内部格式不一致（见 §3.3）；`tst_outbound_ack` 用 `SqliteFixture`（`:memory:` 后端足够，无 WAL/session 依赖，见 §3.1）建库并 seed `__sync_outbound_ack` DDL。错误码断言统一走 `ASSERT_ERR_CODE`（见 §3.7）。

### 5.1 被测线缆格式与不变量

`PayloadCodec` 的统一线缆布局（`writeHeader`/`readHeader` 与各 `encode*`）：

```
magic(quint32 = 0x44425359 "DBSY") | version(quint16) | kind(quint8) | header | qCompress(body)
```

- 常量：`kMagic = 0x44425359`、`kVersion = 2`、`kVersionMin = 1`。`decode` 仅接受 `[kVersionMin, kVersion]` 即 `ver ∈ {1, 2}`；`ver < 1` 或 `ver > 2` → 拒绝。
- `header` 字段顺序（`writeHeader`）：`origin, originSeq, parentSeq, schemaFingerprint, schemaVer, streamEpoch, routeTag, pushId, (qint32)chunkSeq, (qint32)totalChunks, senderPeer`。其中 `senderPeer` **仅 v≥2 存在**（M-03）：`readHeader(version<2)` 不读 `senderPeer` 并清空它，从而保证 v1 旧节点 payload 可被新节点解（向后兼容，对应 ⑦）。
- body 一律 `qCompress` 压缩后写入；changeset body 是原始 SQLite changeset blob，其余 body 为 `QJsonDocument(...).toJson(Compact)`。`decode` 先 `qUncompress`，若 `compressed` 非空而解压结果为空 → `payload decompression failed`。
- 两类 ACK（`encodeChangesetAck`/`encodeChunkAck`）走**独立的非压缩**字节布局，复用 `magic|version|kind` 三元前缀但**不带 header**；`decodeChangesetAck`/`decodeChunkAck` 仅校验 `magic` 与 `kind`，字段顺序见下。

> 错误码约定：`decode` 的失败均归一到 `E_SYNC_PAYLOAD_CORRUPT`（载荷损坏/不可解析/版本不支持）。当前实现的 `*err` 文案为英文短语（`bad magic number`、`unsupported codec version N`、`payload header read error`、`payload body read error`、`payload decompression failed`、`payload header senderPeer read error`）。**本测试计划要求实现把这些失败统一冠以 `E_SYNC_PAYLOAD_CORRUPT` 码前缀**（与 §3.7 的 `ASSERT_ERR_CODE` 码级断言口径一致，满足 O3）；下表用例以该码断言，文案作为辅助匹配。

### 5.2 套件 `tst_payload_codec`

**被测**：`src/sync/payload/PayloadCodec.{h,cpp}` —— `encodeChangeset`、`encodeSelectionPush`、`encodeBaselineRequest`、`encodeBaselineResponse`、`decode`、`encodeChangesetAck`、`encodeChunkAck`、`decodeChangesetAck`、`decodeChunkAck`，及私有 `writeHeader`/`readHeader`（经公有 API 间接覆盖）。

**夹具**：`ChangesetFixture::recordChangeset`（真实 changeset blob，§3.3）+ `SqliteFixture(Memory)`（仅为录制提供连接与种子表）+ `ASSERT_ERR_CODE`（§3.7）。`tst_payload_codec` 本体大多数用例为纯字节往返，不持库。

**公共断言辅助**（在套件内定义）：

```cpp
// 断言两个 PayloadHeader 全字段相等（含 senderPeer / chunkSeq / totalChunks）
static void assertHeaderEqual(const PayloadHeader& a, const PayloadHeader& b);

// 取一个填满全字段的样例 header（origin/seq/parentSeq/指纹/schemaVer/epoch/routeTag/pushId/chunk/sender）
static PayloadHeader sampleHeader();

// 把已编码 payload 的第 pos 字节按位翻转，构造篡改样本
static QByteArray flipByte(QByteArray buf, int pos);
```

#### 5.2.1 测试用例表

| slot 名 | 场景 | 关键断言 |
| --- | --- | --- |
| `changesetRoundTrip` | ① 真实 changeset 编/解码往返 | `encodeChangeset(h, cs)` → `decode` 返回 `true`；`out.kind == Changeset`；`out.changeset == cs`（解压后逐字节相等）；`assertHeaderEqual(out.header, h)`（origin/originSeq/parentSeq/schemaFingerprint/schemaVer/streamEpoch/routeTag/pushId/chunkSeq/totalChunks/senderPeer 全等） |
| `changesetHeaderAllFields` | ① header 全字段穿透 | 用 `sampleHeader()` 把每个字段置非默认值（如 `originSeq=42`、`parentSeq=41`、`streamEpoch=7`、`chunkSeq=3`、`totalChunks=5`、`senderPeer="C"`、`routeTag="rt-9"`），往返后逐字段相等；特别校验 `chunkSeq/totalChunks` 经 `qint32` 收窄再还原无误 |
| `changesetRawPayloadPreserved` | ① 原始 payload 留存（C-5） | `decode` 后 `out.rawPayload == data`（编码后原始字节），保证隔离区可经 `codec->decode()` 重放 |
| `selectionPushRoundTrip` | ② selection-push 往返 | 构造 `SelectionPushBody{frozenEntries, rows, pushId, chunkSeq, totalChunks}`，`encodeSelectionPush`→`decode`：`out.kind == SelectionPush`；`selection.pushId/chunkSeq/totalChunks` 相等；`rows` 逐 `QVariantMap` 相等（键值与类型）；`frozenEntries` 逐项 `table/primaryKey/pkHash/recordKind/topoIndex/fingerprint` 全等（`fingerprint` 经 base64 往返字节相等） |
| `selectionPushEmptyAndMulti` | ② 边界：空集与多条 | 空 `frozenEntries`+空 `rows` 往返得空列表；多条（rows 与 frozenEntries 等长、含 NULL/blob/中文字符串值）往返保持顺序与逐项对齐（`rows[i]` 对 `frozenEntries[i]`） |
| `baselineRequestRoundTrip` | ③ baseline 请求往返 | `BaselineRequestPayload{origin, streamEpoch, requestedTables, fromSeq, pendingArtifactName}` 往返：`out.kind == BaselineRequest`；各标量（含 `streamEpoch/fromSeq` 经字符串数值往返）与 `requestedTables` 列表顺序全等 |
| `baselineResponseRoundTrip` | ③ baseline 响应往返 | `BaselineResponsePayload{origin, requestOrigin, streamEpoch, tables, fromSeq, pendingArtifactName, baselineData(blob), sourceMaxSeq, originCuts[]}` 往返：标量全等；`baselineData` 经 base64 字节相等；`originCuts` 逐项 `{origin, streamEpoch, appliedSeq}` 全等（C-03 含 epoch 不丢） |
| `changesetAckRoundTrip` | ④ ChangesetAck 往返 | `encodeChangesetAck({origin,streamEpoch,appliedSeq,toPeer})`→`decodeChangesetAck` 返回 `true`，四字段全等；字段顺序 `origin→streamEpoch→appliedSeq→toPeer` |
| `chunkAckRoundTrip` | ④ PushChunkAck 往返 | `encodeChunkAck({pushId,chunkSeq,totalChunks,checksum,ok,toPeer})`→`decodeChunkAck` 返回 `true`，六字段全等；`chunkSeq/totalChunks` 经 `qint32` 往返无误；分别覆盖 `ok=true` 与 `ok=false` |
| `ackWrongKindRejected` | ④ ACK 串扰防护 | 把 `encodeChunkAck(...)` 的字节喂 `decodeChangesetAck` → `false`（`kind != KChangesetAck`）；反向亦 `false`，证明两类 ACK 互不误解 |
| `corruptBadMagic` | ⑤ 坏 magic | 篡改头 4 字节（`flipByte`/置 0）→ `decode` 返回 `false`，`ASSERT_ERR_CODE(..., "E_SYNC_PAYLOAD_CORRUPT")`（文案 `bad magic number`） |
| `corruptVersionBelowMin` | ⑤ 版本 < kVersionMin | 编码后改写 version 字段为 `0`（< 1）→ `decode` `false` + `E_SYNC_PAYLOAD_CORRUPT`（`unsupported codec version 0`） |
| `corruptVersionAboveMax` | ⑤ 版本 > kVersion | 改写 version 为 `3`（> 2）→ `decode` `false` + `E_SYNC_PAYLOAD_CORRUPT`（`unsupported codec version 3`） |
| `corruptTruncatedBody` | ⑤ 截断 body | 取合法 changeset payload，截去尾部若干字节（body 中段）→ `decode` `false` + `E_SYNC_PAYLOAD_CORRUPT`（`payload body read error` 或 `payload decompression failed`） |
| `corruptChecksumMismatch` | ⑤ 校验和不符/压缩块损坏 | 在 `qCompress` 块内部翻转一字节（破坏 zlib 校验）→ `qUncompress` 得空而 `compressed` 非空 → `decode` `false` + `E_SYNC_PAYLOAD_CORRUPT`（`payload decompression failed`） |
| `corruptMissingHeaderField` | ⑤ 缺头字段 | 在 header 中途截断（如写完 `routeTag` 即止），使 `readHeader` 读 `pushId/chunkSeq/...` 时 `ds.status() != Ok` → `decode` `false` + `E_SYNC_PAYLOAD_CORRUPT`（`payload header read error`）；另构造 v2 头缺 `senderPeer` 的变体 → `payload header senderPeer read error` |
| `corruptUnknownKind` | ⑤ 未知 kind 字节 | 头部合法但 `kind` 写为 `0xFF`（非 0..5）→ `decode` `false` + `E_SYNC_PAYLOAD_CORRUPT`（`unknown payload kind 255`） |
| `compressLargeBlobRoundTrip` | ⑥ 大 blob 压缩往返 | 用 `ChangesetFixture` 录制 ≥1 MiB 的 changeset（批量插入），`encodeChangeset`→`decode` 解压后逐字节相等；同时断言 `encodeChangeset` 输出长度 < 原始 changeset 长度（验证压缩生效，关联 NFR-5 字节预算） |
| `compressHighlyCompressible` | ⑥ 高可压缩边界 | 全 0 / 高重复内容 blob 往返一致，压缩比显著（断言编码后 < 原始的 10%），覆盖 `qCompress`/`qUncompress` 极端分支 |
| `v1BackwardCompatDecode` | ⑦ v1 兼容解码 | 手工拼一个 `version=1` 的 changeset payload（header 不含 `senderPeer`）→ 新节点 `decode` 返回 `true`，`out.header.senderPeer` 为空串，其余字段正确；证明 `readHeader(version=1)` 不越界吞 body |
| `v2WritesVersion2` | ⑦ 新节点写 v2 | 任意 `encode*` 输出的 version 字段读出为 `2`（`kVersion`），且 header 含非空 `senderPeer` 时可被 v2 `decode` 还原 |

#### 5.2.2 关联需求与错误码

| 维度 | 关联 |
| --- | --- |
| 需求 | **FR-3 载荷编解码**（①②③⑥⑦ 制品往返与压缩）、FR-4 ACK 往返（④，ACK 自描述含 `toPeer`，对应 J-01/M-09/H-04）、C6（线缆格式契约与跨版本兼容）、NFR-5 载荷字节预算（⑥ 压缩生效断言） |
| 不变量/修复点 | M-03（version=2 + `senderPeer` 仅 v≥2）、C-03（baseline `originCuts` 携带 `stream_epoch`）、C-05（`senderPeer` 物理发送者）、C-5（`rawPayload` 留存供隔离区重放） |
| 错误码 | `E_SYNC_PAYLOAD_CORRUPT`（⑤ 全部损坏/篡改用例：坏 magic、版本越界、截断 body、缺头字段、压缩块损坏、未知 kind） |

#### 5.2.3 覆盖率目标

`src/sync/payload` 目标行覆盖率 **≈92%**（与 §1.5 预算一致）。覆盖矩阵：全部 6 个 `encode*` + 2 个 ACK encode/decode + `decode` 五个 `kind` 分支（Changeset/SelectionPush/BaselineRequest/BaselineResponse + 未知 kind 拒绝）+ `readHeader` 的 v1/v2 两分支 + 六类损坏退出路径。预期未覆盖的 ≈8% 集中在防御性 `if (err)` 空指针守卫的 else 旁路与不可达的 `QDataStream` 内部错误组合。

### 5.3 套件 `tst_outbound_ack`

**被测**：`src/sync/anchor/OutboundAckStore.{h,cpp}` —— `init`、`updateAcked`、`ackedSeq`、`minAckedSeq`、`setPendingBaseline`、`lastSentLocalSeq`、`updateLastSent`、`minUnackedLocalSeq`。

**DDL 与分层语义**：表 `__sync_outbound_ack`，主键 `(peer, origin, stream_epoch)`，列含 `acked_seq`、`last_sent_seq`、`last_ack_ms`、`pending_baseline`。本表为**发送端水位**，与接收端 `__sync_applied_vector` 分层（F-05 / D-19）：发送端只记「对端确认到哪」与「已广播到哪」，不参与接收端去重判定。`minUnackedLocalSeq` 还跨 `__sync_changelog` 做 LEFT JOIN，故该套件需 seed `__sync_changelog` 的最小可用 schema（`local_seq, origin, origin_seq, stream_epoch`）。`origin == '__broadcast__'` 是 `updateLastSent`/`lastSentLocalSeq` 专用的广播哨兵行（J-01），与按 origin 的 `acked_seq` 隔离；`minAckedSeq`/`minUnackedLocalSeq` 显式排除该哨兵。

**夹具**：`SqliteFixture(Memory)`（§3.1，纯 store 单测无需 WAL/磁盘）；`init()` 内 seed `__sync_outbound_ack` 与 `__sync_changelog` DDL（来自被测库迁移脚本）；`ASSERT_ERR_CODE`（§3.7）。

#### 5.3.1 测试用例表

| slot 名 | 场景 | 关键断言 |
| --- | --- | --- |
| `initCreatesTable` | ① init 建表（探测可用） | 在已 seed DDL 的库上 `init(db,&err)` 返回 `true`；在缺表库上调用应返回 `false` 并回填 `*err`（`SELECT ... WHERE 0` 探测失败） |
| `updateAckedAdvancesAndReadBack` | ② 前移 + 读回 | `updateAcked("B","A",1,10)` 后 `ackedSeq("B","A",1) == 10`；再 `updateAcked(...,20)` → `== 20`（MAX 语义前移） |
| `ackedSeqMissingReturnsMinusOne` | ② 缺行读回 | 对未写过的 `(peer,origin,epoch)` `ackedSeq(...) == -1` |
| `minAckedSeqActivePeersMin` | ③ minAckedSeq = 活跃 peer 最小 | 同 `(origin="A",epoch=1)` 下 `B→acked 30`、`C→acked 10`、`D→acked 50` 且均 `pending_baseline=0` → `minAckedSeq("A",1) == 10`（截断水位取活跃 peer 最小） |
| `minAckedSeqExcludesPendingBaseline` | ③ 未活跃 peer 退出计算 | 上一场景把 `C`（最小者）`setPendingBaseline("C",true)` → `minAckedSeq("A",1) == 30`（`pending_baseline=0` 过滤，待基线 peer 不拖低截断水位，避免死/未追平 peer 阻塞 changelog 截断） |
| `minAckedSeqNoRowsMinusOne` | ③ 无行/全过滤 | 无任何 `(origin,epoch)` 行，或全部 `pending_baseline=1` → `MIN(acked_seq)` 为 NULL → `minAckedSeq == -1` |
| `setPendingBaselineToggle` | ④ pending 标志置位/复位 | `setPendingBaseline("B",true)` 后该 peer 所有行 `pending_baseline=1`（影响 `minAckedSeq` 过滤）；`setPendingBaseline("B",false)` 复位后重新计入 |
| `lastSentAndUpdateLastSent` | ⑤ 广播水位读写 | 初始 `lastSentLocalSeq("B",1) == -1`；`updateLastSent("B",1,100)` 后 `== 100`；写哨兵行 `(B,'__broadcast__',1)`，不污染按 origin 的 `acked_seq` |
| `lastSentForwardOnly` | ⑤ 广播水位仅前移 | `updateLastSent("B",1,100)` 后再 `updateLastSent("B",1,50)`（旧值）→ `lastSentLocalSeq("B",1) == 100`（MAX 语义，不回退） |
| `minUnackedLowerBound` | ⑥ 未确认下界 | seed `__sync_changelog`：origin A 的 `origin_seq ∈ {1..5}`、`local_seq ∈ {1..5}`；`updateAcked("B","A",1,3)`（A 已确认到 3）→ `minUnackedLocalSeq("B",1)` 返回首个未确认 changelog 行的 `local_seq - 1`（= 对应 `origin_seq=4` 行的 `local_seq` 减 1），保证未 ACK 变更恒可重发 |
| `minUnackedLeftJoinUnackedOrigin` | ⑥ 无 ACK 行的 origin（C-02） | changelog 含 origin `Z` 但 `outbound_ack` 无 `(B,Z,*)` 行 → LEFT JOIN 视为 `acked_seq=-1`，`Z` 的最小 `local_seq` 被纳入下界（不被静默漏掉） |
| `minUnackedAllAckedReturnsMinusOne` | ⑥ 全部已确认 | 所有 changelog `origin_seq` 均 ≤ 对应 `acked_seq` → 无未确认行 → `MIN` 为 NULL → `minUnackedLocalSeq == -1`（从头无需重发） |
| `minUnackedCrossEpoch` | ⑥ 跨 epoch 纳入（M-02） | changelog 含 epoch 1 与 epoch 2 的未 ACK 行，`updateAcked` 仅覆盖 epoch 1 → epoch 2 行仍计入下界（三段键 `(peer,origin,stream_epoch)` 配对，未 ACK 即纳入） |
| `multiPeerOriginEpochIsolation` | ⑦ 多 peer/origin/epoch 隔离 | 写 `(B,A,1)=10`、`(C,A,1)=20`、`(B,X,1)=5`、`(B,A,2)=99` 后：`ackedSeq` 四者互不串扰；`minAckedSeq("A",1)` 仅聚合 `(*,A,1)`（=10）；`(A,2)` 与 `(X,1)` 不影响 `(A,1)` |
| `anchorForwardOnlyIdempotent` | ⑧ 锚点仅前移（收旧 ACK 幂等） | `updateAcked("B","A",1,20)` 后重复 `updateAcked("B","A",1,5)`（旧 ACK 乱序到达）→ `ackedSeq == 20` 不回退；再 `updateAcked(...,20)` 幂等仍 `20`（MAX 语义保证收旧/重复 ACK 安全） |

#### 5.3.2 关联需求与错误码

| 维度 | 关联 |
| --- | --- |
| 需求 | **FR-4 ACK 与水位推进**（②⑤⑥⑧ updateAcked/lastSent/minUnacked 前移与幂等）、**F-05 位点分层**（发送端 `__sync_outbound_ack` 与接收端 `__sync_applied_vector` 严格分离）、C6（`minAckedSeq` 作为 changelog 截断水位的契约）、D-19（分层位点设计决策） |
| 不变量/修复点 | J-01（`__broadcast__` 哨兵独立追踪广播水位）、C-02（`minUnackedLocalSeq` LEFT JOIN 不漏无 ACK 行的 origin）、M-02（跨 epoch 未 ACK 行纳入下界）；锚点 MAX-only 单调前移（②⑤⑧） |
| 错误码 | `init` 在缺表库回填底层 `*err`（驱动错误透传，非 `E_SYNC_*` 专码）；本套件以行为/水位断言为主，无 `E_SYNC_*` 直接产生路径，错误码覆盖由上层 anchor 调用方套件承担 |

#### 5.3.3 覆盖率目标

`src/sync/anchor` 目标行覆盖率 **≈92%**（与 §1.5 预算一致）。覆盖矩阵：8 个公有方法全部命中，`updateAcked`/`updateLastSent` 的 UPSERT 插入与冲突更新两路径、`minAckedSeq` 的 `pending_baseline` 过滤与 NULL→-1 路径、`minUnackedLocalSeq` 的 LEFT JOIN 命中/未命中/全 NULL 三路径、`lastSentLocalSeq` 的有值/NULL 路径均覆盖。预期未覆盖的 ≈8% 集中在 `q.exec()` 失败时的 `if (err)` 错误回填旁路（需驱动级故障注入，留待集成层），及 `q.next()` 不可达组合。
## 6. apply 应用子系统单元测试

apply 应用子系统（`src/sync/apply/`，约 1944 行）是整个同步引擎的**收敛核心**：所有外来 changeset 的落库、所有冲突的仲裁、所有「已提交=已落 changelog」原子事务都汇聚于此。它把分散的不变量——严格连续水位（G-05）、行胜者全序（G-01）、低 rank DELETE 恢复（C-01）、应用三件套原子（changeset / applied_vector / table_state 同事务）、入站 blob 不重铸（appendForward）——固化为可单测的小单元。本节为该子系统的六个被测单元各设计一套 Qt Test 套件，给出 slot 级用例表、关联需求/错误码与目标行覆盖率。

本节所有套件遵循第 3 节夹具与约定：写均经唯一 `wconn` + `WriteTxn(BEGIN IMMEDIATE)`；真实 changeset 一律由 `ChangesetFixture::recordChangeset()` 经真实 SESSION 扩展录制，**严禁手搓 changeset 字节**；schema 由 `tests/data/sync/*.sql` 种子；期望失败的错误码断言统一用 `ASSERT_ERR_CODE(call, code)` 宏。错误码常量取自 `include/dbridge/Errors.h`（`E_SYNC_APPLY_FK` / `E_SYNC_APPLY_CONSTRAINT` / `E_SYNC_SCHEMA_MISMATCH` / `E_SYNC_PAYLOAD_CORRUPT` / `E_SYNC_TRANSPORT` / `E_SYNC_INIT` / `E_SYNC_GAP`）。

### 6.0 子系统单元映射与覆盖率预算

| 被测单元 | 文件（行） | 套件 | 库后端 | 目标行覆盖率 |
| --- | --- | --- | --- | --- |
| `AppliedVectorStore` | `AppliedVectorStore.{h,cpp}`（152） | `tst_applied_vector` | 临时文件 | ≥ 92% |
| `RowWinnerStore` | `RowWinnerStore.{h,cpp}`（176） | `tst_row_winner` | `:memory:` 可（纯 store） | ≥ 92% |
| `ChangesetApplier` | `ChangesetApplier.{h,cpp}`（620） | `tst_changeset_applier` | 临时文件（session 需磁盘） | ≥ 85% |
| `UpsertExecutor` | `UpsertExecutor.{h,cpp}`（134） | `tst_upsert_executor` | `:memory:` 可 | ≥ 90% |
| `SelectionPushApplier` | `SelectionPushApplier.{h,cpp}` | `tst_selection_push_applier` | 临时文件 | ≥ 88% |
| `CapturedWriteTemplate` | `CapturedWriteTemplate.{h,cpp}`（803，最大） | `tst_captured_write_template` | 临时文件 | ≥ 88% |
| **子系统合计** | **约 1944 行** | 六套件 | — | **≈ 88%** |

> 覆盖率口径：仅统计 `src/sync/apply/` 下被测 `.cpp`，按第 2.4.3 节过滤 moc/3rdparty/测试自身。未达 88% 的差额主要来自 `extractMutationsStatic` 与实例 `extractMutations` 的重复分支、`PRAGMA table_info` 失败回退路径（`_col_%1` 兜底）等防御性死角，由各套件「负向/兜底」用例补足。

---

### 6.1 套件 `tst_applied_vector`（AppliedVectorStore — 严格连续 G-05）

**被测核心**：`check()` 的严格连续三态判定与 `advance()` 的单调写入。`check()` 不修改库；`advance()` 用 `INSERT … ON CONFLICT DO UPDATE … WHERE excluded.applied_seq > applied_seq` 实现「只进不退」。关键防回归点是**缺口不得推高水位**——若 seq2 先到、seq1 后到，必须保证 seq2 被判 Gap 而非被错误 advance，否则 seq1 永久丢失（FR-6 / G-05）。

**前置**：`__sync_applied_vector` 表需先建（由 schema 初始化提供）；`init()` 仅做存在性自检（`SELECT … WHERE 0`）。

| slot | 断言要点 |
| --- | --- |
| `initOnFreshDb()` | 表存在时 `init()` 返回 true；表缺失时返回 false 且 `*err` 非空（探针 SQL 失败） |
| `firstSeenOriginSeq1_Apply()` | 无行记录且 `seq==1` → `SeqCheckResult::Apply`（首见 origin 起点） |
| `firstSeenSeqGt1_Gap()` | 无行记录且 `seq==2` → `Gap`，`*err` 含 "gap: no prior applied seq" |
| `firstSeenSeqLe0_NoOp()` | 无行记录且 `seq<=0` → `NoOp`（非法/基线占位序号幂等吞掉，不报 Gap） |
| `applyExactlyNextSeq()` | 已 `advance` 到 5，`check(seq==6)` → `Apply`（`seq==applied+1`） |
| `noOpOnEqualSeq()` | 已到 5，`check(seq==5)` → `NoOp`（幂等重放：`seq<=applied`） |
| `noOpOnLowerSeq()` | 已到 5，`check(seq==3)` → `NoOp`（旧 seq 重投，幂等吞掉） |
| `gapOnSkippedSeq()` | 已到 5，`check(seq==8)` → `Gap`，`*err` 含 "gap: applied=5 but seq=8" |
| `advanceThenCheckConsistent()` | `advance(6)` 后再 `check(6)`→`NoOp`、`check(7)`→`Apply`；`current()==6` |
| `advanceMonotonicNoRollback()` | 先 `advance(10)`，再 `advance(7)`；`current()` 仍为 10（`WHERE excluded>applied` 拒绝回退）；二次 `advance` 返回 true（SQL 成功，只是无行变更） |
| `advanceIdempotentSameSeq()` | 连续两次 `advance(10)`：均 true，`current()==10`，updated_ms 可被覆盖但 applied_seq 不变 |
| `gapDoesNotRaiseWatermark()` | 序列乱序到达：`check(2)`→Gap（**不 advance**）；随后 `check(1)`→Apply、`advance(1)`；再 `check(2)`→Apply。验证缺口期间水位停在 0，seq1 不丢（防回归核心） |
| `resetToZeroBaseline()` | `reset(origin,epoch,baselineGen=7)`：`applied_seq` 归 0、`baseline_generation`=7；随后 `check(seq==1)`→Apply |
| `resetToTruncationPoint()` | `resetTo(origin,epoch,originSeq=42,baselineGen=8)`：`applied_seq`=42（**非 0**，C-03 截断点）；随后 `check(43)`→Apply、`check(42)`→NoOp、`check(44)`→Gap |
| `resetBumpsBaselineGeneration()` | 连续 `reset(gen=1)`→`reset(gen=2)`：`readRow` 读出的 baseline_generation 单调更新为 2（基线代际推进） |
| `multiOriginIndependent()` | origin="A" advance 到 5、origin="B" advance 到 2 互不影响；各自 `current()` 独立 |
| `multiEpochIndependent()` | 同 origin 不同 `stream_epoch`（epoch=1 到 5；epoch=2 全新）：epoch=2 的 `check(seq==1)`→Apply（epoch 切换=独立水位线） |
| `currentReturnsMinusOneWhenAbsent()` | 无行记录时 `current()` 返回 -1（`readRow` 兜底 `*appliedSeq=-1`） |
| `advanceSqlErrorPropagates()` | 注入坏库句柄（关闭连接）→ `advance` 返回 false 且 `*err` 非空（`lastError().text()` 透传） |

**关联需求/错误码**：G-05（严格连续）、FR-6（不丢更）、C-03（`resetTo` 写真实 max origin_seq 而非 0）。本单元不直接产出 `E_SYNC_*` 错误码（Gap/NoOp 由上层 `CapturedWriteTemplate::branchA` 转译为 `GAP_PENDING` 或幂等成功）。**目标行覆盖率 ≥ 92%**：三态分支、首见三态、单调拒绝、resetTo/reset 双路径、多 origin/epoch、SQL 错误透传全覆盖。

---

### 6.2 套件 `tst_row_winner`（RowWinnerStore — 行胜者全序 G-01）

**被测核心**：`beats()` 的 (rank, originSeq, origin) 三级全序、`put()` / `putOrRefill()` 的「仅当胜出才写」语义、`pkHash()` 的规范化、`winningContent` 对低 rank DELETE 恢复的支撑（C-01）。`beats()` 全序保证 changeset 任意到达顺序收敛到同一终态。

**前置**：`pkHash()` 委托 `TableStateStore::rowHash()` 做类型标签化规范编码（M-01，防可构造碰撞）；该单元可用 `:memory:`。

| slot | 断言要点 |
| --- | --- |
| `initSelfCheck()` | `__sync_row_winner` 存在 → `init` true；缺失 → false |
| `beatsNoIncumbent()` | incumbent.rank==INT_MIN（哨兵，无胜者）→ 任何 challenger 胜出 |
| `beatsHigherRankWins()` | challenger.rank > incumbent.rank → true（跨源按 rank 比，rank 全局唯一→全序） |
| `beatsLowerRankLoses()` | challenger.rank < incumbent.rank → false |
| `beatsSameRankHigherSeqWins()` | 同 rank，challenger.originSeq > incumbent → true（同源按 seq） |
| `beatsSameRankLowerSeqLoses()` | 同 rank，challenger.originSeq < incumbent → false |
| `beatsTieBrokenByOriginId()` | rank 与 seq 全等 → 按 originId 字典序大者胜（H-01 确定性 tie-break，保证任意顺序同终态） |
| `putWritesWhenWins()` | 空表 `put(rank=10,seq=1)` 写入；`get()` 读回 origin/rank/seq/contentHash/winningContent 一致 |
| `putNoOpWhenLoses()` | 已存 rank=10，`put(rank=5)` 返回 true 但**不改库**（incumbent 仍 rank=10） |
| `putOverwritesWhenHigher()` | 已存 rank=5，`put(rank=10)` → REPLACE 覆盖（高 rank 后到取代） |
| `lateLowRankOmitted()` | 已存 rank=10，后到 rank=3 经 `put` 被静默忽略（**低 rank 后到 OMIT**），row_winner 不变 |
| `highRankReplaces()` | 已存 rank=3，后到 rank=10 → 覆盖（**高 rank REPLACE**） |
| `putOrRefillSameRankSeqEmptyContent()` | 已存 (rank=10,seq=1,winningContent="")（conflictCb 留下的半写记录），`putOrRefill` 同 rank/seq 但带非空 content → 允许补齐（H-01 sameRankSeq 补填） |
| `putOrRefillSameRankSeqNonEmptyNoOp()` | 已存非空 content，`putOrRefill` 同 rank/seq → no-op（不退化为重复写） |
| `putOrRefillStillRespectsBeats()` | `putOrRefill` 低 rank 仍被拒（补填仅放宽「同 rank/seq 空 content」一处） |
| `winningContentForDeleteRecovery()` | `put` 存入 JSON winningContent（含 `__i64:`/`__b64:` 标签值），`get()` 原样读回——供 ChangesetApplier 低 rank DELETE 恢复（C-01）使用 |
| `pkHashCanonicalStable()` | 同一 `QVariantMap` 多次 `pkHash()` 结果稳定；与 `TableStateStore::rowHash().toHex()` 一致 |
| `pkHashNoCollision()` | `{a:1,b:2}` 与 `{a:12,b:""}` 等可构造拼接歧义输入产出不同 hash（M-01 类型标签防碰撞） |
| `pkHashOrderInsensitive()` | `QVariantMap` 按 key 排序，插入顺序不影响 hash（列序无关） |
| `resetAllClearsTable()` | 写入多行后 `resetAll()` 清空整表（基线重置）；`get()` 全返回 rank==INT_MIN |
| `clearSingleRow()` | `clear(table,pkHash)` 仅删指定行（C-06：低 rank DELETE 抹掉胜者后单行清理），其余行保留 |
| `putSqlErrorPropagates()` | 坏库句柄 → `put`/`putOrRefill` 返回 false 且 `*err` 非空 |

**关联需求/错误码**：G-01（终态=所有来源 (rank,originSeq) 规范序极大元；rank 全局唯一→全序）、C-01（winningContent 供低 rank DELETE 恢复）、C-06（单行 clear）、M-01（pkHash 规范编码防碰撞）、H-01（同 rank/seq tie-break + sameRankSeq 补填）。注：本单元仅由 changeset 路径维护，**上行 UPSERT 不叠 rank**（C12，验证在 6.6 的 branch B/C 不写 row_winner）。**目标行覆盖率 ≥ 92%**。

---

### 6.3 套件 `tst_changeset_applier`（ChangesetApplier — apply_v2 + 冲突仲裁）

**被测核心**：`apply()` 经 `sqlite3changeset_apply_v2` 落库，`filterCb`（xFilter，仅同步表）与 `conflictCb`（消费 row_winner 决定 REPLACE/OMIT）协同，`updateWinnersFromChangeset` 在 apply 后推进胜者并执行低 rank DELETE 恢复（C-12）。所有 changeset 由 `ChangesetFixture::recordChangeset` 真实录制。

**前置**：需真实库（session 需磁盘表）；`syncTables` 非空时作为 xFilter 与 winner 更新的共享白名单（H-04）；`opts.authoritative` 区分下行强制 REPLACE。

| slot | 断言要点 |
| --- | --- |
| `applyInsertCountsApplied()` | 真实 INSERT changeset 应用到空表：行落库，`ApplyOutcome.applied` 计数正确、conflicts/ignored=0 |
| `applyUpdateAndDelete()` | UPDATE/DELETE changeset 正常应用，目标行更新/删除 |
| `applyEmptyChangesetNoOp()` | 空 blob：`apply` 成功，outcome 全 0 |
| `filterCbAcceptsSyncTable()` | `syncTables={"t_a"}`，changeset 含 t_a → 应用（`filterCb` 返回 1） |
| `filterCbRejectsNonSyncTable()` | `syncTables={"t_a"}`，changeset 含 t_b → t_b 行被跳过（`filterCb` 返回 0），t_a 仍应用 |
| `filterCbRejectsSyncMetaTable()` | changeset 含 `__sync_*` 表 → 无条件拒绝（M-04，先于白名单），即使白名单误列也不写元表 |
| `filterCbEmptyListAcceptsAll()` | `syncTables` 为空 → 接受全部表（测试模式） |
| `conflictReplaceWhenChallengerWins()` | 目标行已存（local），incumbent row_winner rank 低；入站 rank 高 → conflictCb 返回 REPLACE，`applied++`，行被覆盖 |
| `conflictOmitWhenIncumbentWins()` | incumbent rank 高，入站 rank 低 → conflictCb 返回 OMIT，`ignored++`，本地行保留 |
| `conflictTieBrokenByOrigin()` | rank 与 seq 全等 → 按 origin 字典序裁决，结果与到达顺序无关 |
| `conflictNotFoundOmitted()` | `SQLITE_CHANGESET_NOTFOUND`（UPDATE/DELETE 找不到目标）→ OMIT、`ignored++` |
| `authoritativeForcesReplace()` | `opts.authoritative=true`：冲突回调直接 REPLACE 不查 row_winner（`applied++`、`conflicts++`），且 **跳过 `updateWinnersFromChangeset`**（下行不叠 rank） |
| `conflictPolicyTargetWinsOmits()` | 非 authoritative 且 `ConflictPolicy::TargetWins` → 冲突 OMIT（本地胜，`ignored++`）不查 row_winner（M-01） |
| `conflictPolicyManualOmits()` | `ConflictPolicy::Manual` 同样 OMIT（待人工处理） |
| `foreignKeyAbortRollsBack()` | 子行引用不存在父行的 changeset → `SQLITE_CHANGESET_FOREIGN_KEY` → ABORT，`apply` 返回 false；上层据 "foreign"/"fk" 关键字映射 `E_SYNC_APPLY_FK`，整事务回滚无半提交（用 `08_fk_chain.sql`） |
| `constraintAbortRollsBack()` | 违反 UNIQUE/NOT NULL 的 changeset → `SQLITE_CHANGESET_CONSTRAINT` → ABORT，`apply` 返回 false → 映射 `E_SYNC_APPLY_CONSTRAINT` |
| `applyV2RcErrorPropagates()` | 损坏/截断 blob 使 `apply_v2` 返回非 OK/ROW → false，`*err` 含 "sqlite3changeset_apply_v2 rc=" |
| `updateWinnersInsertUpdate()` | INSERT/UPDATE 应用后 `updateWinnersFromChangeset` 经 `putOrRefill` 写入完整 RowWinner（含 winningContent JSON、类型标签 `__i64:`/`__b64:`） |
| `updateWinnersSkipsRejectedTables()` | 非白名单/`__sync_*` 表的行**不**写入 row_winner（H-01 共享白名单 `isAllowedSyncTable`） |
| `lowRankDeleteRestoresWinner()` | 高 rank 胜者行存在，低 rank DELETE 到达：apply_v2 先删，`updateWinnersFromChangeset` 检出 `dominated` 并用 winningContent 经 `ON CONFLICT DO UPDATE` 恢复该行；apply 整体成功，行仍在（C-01/C-12 核心） |
| `lowRankDeleteNoContentFailsRollback()` | 低 rank DELETE 命中的胜者 winningContent 为空 → `updateWinnersFromChangeset` 返回 false，`*err` 含 "low-rank DELETE … no stored content"，`apply` 返回 false → 上层回滚（DELETE 绝不静默胜出） |
| `dominatedDeleteRestoreUsesUpsertNotReplace()` | 恢复 SQL 走 `INSERT … ON CONFLICT DO UPDATE`（或全 PK 表 `INSERT OR IGNORE`），**不**用 `INSERT OR REPLACE`，避免 DELETE+INSERT 级联破坏 FK 子行（C-3） |
| `winnerTypeTagRoundTrip()` | int64 大值（>2^53）与 BLOB 列经 `__i64:`/`__b64:` 标签存入 winningContent，恢复时正确解码回 qlonglong/QByteArray（C-2/C-3 精度保真） |
| `isAllowedSyncTableStatic()` | 静态谓词：`__sync_*` → false；空白名单 → true；命中白名单 → true；未命中 → false（三路径共用一致拒绝） |
| `rebaseBufferProducedNonAuthoritative()` | 非 authoritative 且发生冲突时 `apply_v2` 产出 rebase，填入 `ApplyOutcome.rebaseBuffer`；authoritative 时不产出 rebaseBuffer |

**关联需求/错误码**：G-01、C-01/C-02/C-03/C-11/C-12、H-01/H-04/H-05、M-01/M-04；错误码 `E_SYNC_APPLY_FK`、`E_SYNC_APPLY_CONSTRAINT`（由 `branchA` 据 `*err` 关键字转译，本单元仅返回 false + `*err`）。**目标行覆盖率 ≥ 85%**（最大占比单元，冲突回调四类 conflict 分支、低 rank 恢复多分支、类型标签编解码全覆盖；`PRAGMA table_info` 失败的 `_col_%1` 兜底由负向用例触发）。

---

### 6.4 套件 `tst_upsert_executor`（UpsertExecutor — DO UPDATE / DO NOTHING + 行级错误）

**被测核心**：`apply()` 在**已开事务内**批量执行 RowMutation（**不自起事务**，由外层 `WriteTxn` 持有），`buildUpsertSql` 据 `UpsertMode` 生成 DO UPDATE / DO NOTHING SQL，prepared 语句以**整条 SQL 串为键**缓存（M-04），行级失败收集到 `RowError` 不中断后续行。提取自 `ImportService:683-731`。

**前置**：可用 `:memory:`；调用前由用例 `BEGIN`（模拟外层 WriteTxn），断言期间无内部 `BEGIN/COMMIT`。

| slot | 断言要点 |
| --- | --- |
| `doUpdateUpdatesExisting()` | `UpsertMode::DoUpdate`：行已存 → `ON CONFLICT … DO UPDATE SET` 更新非 PK 列；返回 true |
| `doUpdateInsertsWhenAbsent()` | DoUpdate：行不存 → 普通插入 |
| `doNothingSkipsExisting()` | `UpsertMode::DoNothing`：行已存 → `INSERT OR IGNORE` 跳过（依赖行不覆盖），原值不变 |
| `doNothingInsertsWhenAbsent()` | DoNothing：行不存 → 插入 |
| `compositePkConflictTarget()` | 复合 PK（`02_composite_pk.sql`）：`ON CONFLICT (a, b)` 冲突目标正确拼装，更新成功 |
| `allColumnsArePkFallsBackToIgnore()` | DoUpdate 且所有列均为 PK（SET 子句为空）→ 退化为 `INSERT OR IGNORE`（`buildUpsertSql` 兜底） |
| `emptyColumnsRecordsRowError()` | `m.columns` 为空 → 收集一条 `E_SYNC_APPLY_CONSTRAINT` RowError，`continue` 不中断；`apply` 仍返回 true |
| `preparedCacheReuse()` | 同表同列集多行：仅首行 `prepare`，后续命中 `cache_`（以 SQL 串为键复用），结果正确 |
| `differentColumnSetsDistinctCacheKeys()` | 同表不同列集 → 不同 SQL 串 → 不同缓存项，互不串用错误语句（M-04 修复点） |
| `quoteIdentEscapesColumns()` | 含嵌入双引号的列名/表名经 `SqlBuilder::quoteIdent` 正确转义（M-05） |
| `rowErrorOnConstraint()` | 违反 UNIQUE/NOT NULL 的单行 → `q.exec()` 失败 → 收集 RowError（code=`E_SYNC_APPLY_CONSTRAINT`，message=lastError），**后续行继续执行并成功** |
| `rowErrorOnTypeMismatch()` | 类型/约束错误行收集到 RowError，不抛、不中断批量 |
| `rowErrorRawValueFromOriginMeta()` | RowMutation.originMeta 含 "rawValue" → RowError.rawValue 被回填（行级错误上下文） |
| `batchPartialFailureCollectsAll()` | 批量含 2 好 + 1 坏：好行落库、坏行进 errors 列表，`apply` 返回 true，errors.size()==1 |
| `prepareFailureIsFatal()` | 表不存在导致 `prepare` 失败 → `apply` 返回 false 且 `*err` 非空（区别于行级 exec 失败的非致命） |
| `dbNotOpenFatal()` | 未 open 的库 → 立即返回 false，`*err`="database is not open" |
| `noImplicitTransaction()` | 在用例自起的 `BEGIN` 内调用 `apply`，断言执行期间 SQLite `autocommit==0` 全程不变（不自起/不提交事务，由 WriteTxn 持有） |
| `clearPreparedCacheResets()` | `clearPreparedCache()` 后再 `apply` 重新 `prepare`（缓存清空） |

**关联需求/错误码**：I-08（UPSERT 应用）、M-04（SQL 串为缓存键）、M-05（quoteIdent）；错误码 `E_SYNC_APPLY_CONSTRAINT`（行级与空列）。**目标行覆盖率 ≥ 90%**：DO UPDATE / DO NOTHING / 全 PK 退化三路径、prepared 缓存命中/未命中、行级错误收集、致命 vs 非致命区分全覆盖。

---

### 6.5 套件 `tst_selection_push_applier`（SelectionPushApplier — 分块直选/依赖应用）

**被测核心**：`applyChunk()` 把一个 SelectionPush 分块封装为 `WriteParams{kind=InboundSelectionPush}` 转交 `CapturedWriteTemplate::execute()`（即 6.6 的 branch B）。直选行用 DoUpdate（覆盖写），依赖行用 DoNothing（不覆盖已有）；多表按拓扑序应用；错误经 `WriteResult` 透传。本单元薄，主测**参数装配正确**与**结果/错误透传**。

**前置**：需真实库 + 构造好的 `CapturedWriteTemplate`（注入各 store 与 `wconn`/`h`）；`UpsertExecutor&` 虽入参但在本路径 `Q_UNUSED`（模板自持 mutation 路径，列入「不回归」断言）。

| slot | 断言要点 |
| --- | --- |
| `directSelectionDoUpdate()` | 直选行 `UpsertMode::DoUpdate`：已存行被覆盖为推送值 |
| `dependencyRowDoNothing()` | 依赖行 `UpsertMode::DoNothing`：已存行**不**被覆盖（`INSERT OR IGNORE`），保留本地值 |
| `mixedDirectAndDependency()` | 同一 chunk 混合直选 + 依赖行：直选覆盖、依赖跳过，互不影响 |
| `multiTableTopologicalOrder()` | 跨表（父 `08_fk_chain.sql` orders → 子 order_items）按拓扑序应用：父行先于子行落库，FK 不破裂 |
| `paramsAssembledCorrectly()` | 验证 `WriteParams` 装配：kind/origin/epoch/seq/schemaVer/schemaFp/originRank/pushId/chunkSeq/mutations/syncTables 全部正确透传给 `execute()` |
| `successLeavesErrorsUntouched()` | 应用成功时若调用方传入 `errors` 列表，列表保持不变（成功不写 RowError，见实现 `Q_UNUSED(errors)`） |
| `failurePropagatesErrorCode()` | `WriteResult.ok==false` 时 `applyChunk` 返回 false，`*err`=errorMsg（空则 errorCode），错误码透传（如 FK/约束/schema） |
| `errorMsgFallsBackToCode()` | `result.errorMsg` 为空 → `*err` 回退为 `result.errorCode` |
| `upsertExecutorMemberUnused()` | 注入可侦测的 `UpsertExecutor`，断言其 `apply` 在本路径未被调用（模板内部自建 UpsertExecutor，成员 `Q_UNUSED`，防回归） |
| `chunkConstraintErrorAbortsChunk()` | chunk 内某行违约 → 模板 branchBC 整块回滚，`applyChunk` 返回 false（C-09：不 ACK 半块） |

**关联需求/错误码**：I-08、C-09（行级错误整块回滚）；错误码经模板透传 `E_SYNC_APPLY_FK` / `E_SYNC_APPLY_CONSTRAINT` / `E_SYNC_SCHEMA_MISMATCH`。**目标行覆盖率 ≥ 88%**（薄封装，参数装配 + 成功/失败两支 + 透传回退全覆盖）。

---

### 6.6 套件 `tst_captured_write_template`（CapturedWriteTemplate — 三分支写模板，重点）

**被测核心**（最大、最关键单元，803 行）：`execute()` 据 `WriteKind` 路由到 `branchA`（入站 changeset）或 `branchBC`（入站 selectionpush / 本地写），实现设计 §5.4 的三分支。共性铁律：**任一步失败 `WriteTxn.rollback()` 全回退无半提交**；所有分支维护 `__sync_table_state`（无全表扫描，经 `extractMutations` 或预扫 PreScan 增量更新）；应用三件套（业务写 / applied_vector / changelog / table_state）严格同事务原子。本套件按分支分组，逐步骤验证。

#### 6.6.1 分支 A：入站 changeset（`branchA`，WriteKind::InboundChangeset）

步骤序：`check`（严格连续判）→ `verifyPayload`（SchemaGuard）→ `apply_v2`（消费 row_winner）→ `advance`（applied_vector）→ `applyMutations`（table_state，源自 `extractMutations`）→ `appendForward`（**原 blob 入 changelog，不 fresh 捕获，origin 不重铸**）→ `commit`。

| slot | 断言要点 |
| --- | --- |
| `branchA_happyPath()` | `seq==applied+1` 正常路径：行落库、applied_vector advance 到 seq、table_state 更新、changelog 追加原 blob；`commit` 成功，`WriteResult{ok, localChangelogSeq>0, tableMutations 非空}` |
| `branchA_noOpOnAlreadyApplied()` | `check`→NoOp（`seq<=applied`）：`txn.rollback()`、`result.ok==true`（幂等成功），**库无任何变更**，changelog 不追加 |
| `branchA_gapRollbackPending()` | `check`→Gap：`txn.rollback()`、`errorCode=="GAP_PENDING"`（非 Errors.h 码，供 processArtifact 保留 ledger 'seen'），`errorMsg` 含 "keeping artifact pending"；库无变更（缺口不推水位，呼应 6.1） |
| `branchA_schemaMismatch()` | `verifyPayload` 失败 → `txn.rollback()`、`errorCode==E_SYNC_SCHEMA_MISMATCH`、`errorMsg`=guard 错误；库无变更 |
| `branchA_appendForwardUsesRawBlob()` | 应用后 changelog 中存的字节 == 入参 `p.changesetBlob`（**不 fresh 捕获**）；`origin` 字段== `p.origin`（**不重铸为本地 nodeId**），转发保形（核心不变量） |
| `branchA_appliedVectorAdvancedSameTxn()` | `advance` 与 apply 同事务：commit 后 `current(origin,epoch)==seq`；若 advance 失败（注入）→ rollback、`errorCode=="AV_ADVANCE"`、applied_vector 不前进 |
| `branchA_tableStateFromExtractMutations()` | table_state 增量来自 `extractMutations`（INSERT→afterHash/isInsert、UPDATE→before+after、DELETE→beforeHash/isDelete），**无全表扫描** |
| `branchA_fkBreakRollsBackWhole()` | apply 返回 false 且 err 含 "foreign"/"fk" → `txn.rollback()`、`errorCode==E_SYNC_APPLY_FK`；business/applied_vector/changelog/table_state **四者均无变更**（三件套原子） |
| `branchA_constraintBreakRollsBack()` | apply 失败（非 FK 关键字）→ `errorCode==E_SYNC_APPLY_CONSTRAINT`，全回退 |
| `branchA_tableStateUpdateFailureRollsBack()` | `ts_.applyMutations` 失败（M-03）→ `txn.rollback()`、`errorCode=="TABLE_STATE_UPDATE"`；changeset/applied_vector/table_state 全回退（绝不留 applied 已进但 table_state 未更的撕裂态） |
| `branchA_appendForwardFailureRollsBack()` | `clog_.appendForward` 失败 → `txn.rollback()`、`errorCode=="CLOG_FORWARD"`，全回退 |
| `branchA_authoritativeSkipsWinners()` | `p.authoritative=true` 透传至 `ApplyOptions`：冲突强制 REPLACE 且 `updateWinnersFromChangeset` 跳过（下行不叠 rank，C12 呼应 6.3） |
| `branchA_lowRankDeleteRecoveryThenCommit()` | 入站含被支配的低 rank DELETE：apply 内恢复成功 → 整事务 commit，胜者行仍在；恢复失败 → apply false → rollback（端到端串联 6.3 的 C-12） |

#### 6.6.2 分支 B：入站 selectionpush（`branchBC`，isInbound=true）

步骤序：`(push_id, chunk_seq)` 幂等检查 → SchemaGuard → **fresh 捕获**（SessionRecorder.begin）→ PreScan（预扫 rowExists/beforeHash）→ UpsertExecutor → markChunk（push_chunk_progress='applied'，并在全分块到齐时 push_progress→'done'）→ `sealInto(origin=B)` → table_state 更新 → commit。

| slot | 断言要点 |
| --- | --- |
| `branchB_happyPath()` | 正常分块：行 UPSERT 落库、sealInto 入 changelog（origin==推送源 B、epoch==p.epoch）、push_chunk_progress 标 'applied'、table_state 更新；`commit` 成功 |
| `branchB_idempotentSameChecksum()` | `(push_id,chunk_seq)` 已 'applied' 且 checksum 相同 → `txn.rollback()`、`result.ok==true`（幂等跳过），无重复写 |
| `branchB_corruptOnChecksumMismatch()` | 已 'applied' 但 re-deliver 的 checksum 不同 → `txn.rollback()`、`errorCode==E_SYNC_PAYLOAD_CORRUPT`、errorMsg 含两 checksum（H-03 防错投/损坏） |
| `branchB_schemaMismatch()` | inbound SchemaGuard 失败 → rollback、`E_SYNC_SCHEMA_MISMATCH` |
| `branchB_freshCaptureNotForward()` | branch B 走 **fresh 捕获**（SessionRecorder 录本次 UPSERT 的真实 session），sealInto 重新封装，**非**转发原 blob（与分支 A 的 appendForward 对比） |
| `branchB_sealUsesPushOrigin()` | `sealInto` 的 origin==p.origin（推送源），epoch==p.epoch，schemaVer/Fp 用入站值；pushId 写入 changelog（广播屏障可按 push 过滤，H-01） |
| `branchB_markChunkApplied()` | commit 后 `__sync_push_chunk_progress(push_id,chunk_seq).status=='applied'`、checksum/applied_ms 落库 |
| `branchB_promotePushDoneWhenAllChunks()` | 末块应用后 applied_chunks>=total_chunks → `__sync_push_progress.status` 升 'done'（center 自收全块无自 ACK，防广播屏障死锁，H-01） |
| `branchB_doNothingNoOpSkipsTableState()` | 依赖行 DoNothing 且 `ps.rowExists`（INSERT OR IGNORE 实为 no-op）→ **不**为该行更新 table_state（M-01，避免污染 checksum 与 changeset 路径分歧） |
| `branchB_preScanBeforeUpsert()` | PreScan 在 UPSERT **之前**读 rowExists/beforeHash（H-05：UPSERT 后旧行已失，事后读会取到新值致 isInsert/beforeHash 错判） |
| `branchB_preScanExecFailureAborts()` | PreScan 的 `existQ.exec()` 失败 → `rec_.abort()` + `txn.rollback()`、`errorCode=="E_DB_UPSERT"`（M-01：误读会错标 insert/update） |
| `branchB_rowErrorAbortsChunk()` | UpsertExecutor 收集到 rowErrors → `rec_.abort()` + rollback，据 FK/foreign 关键字映射 `E_SYNC_APPLY_FK` 否则 `E_SYNC_APPLY_CONSTRAINT`（C-09 不 ACK 半块） |
| `branchB_sealFailureRollsBack()` | `sealInto` 失败 → rollback、`errorCode=="SEAL"` |
| `branchB_markChunkSqlFailureRollsBack()` | push_chunk_progress upsert 失败 → rollback、`errorCode==E_SYNC_TRANSPORT` |

#### 6.6.3 分支 C：本地写（`branchBC`，isInbound=false）

步骤序：**fresh 捕获** → PreScan → UpsertExecutor → `sealInto(本地 origin=nodeId_, epoch=streamEpoch_)`（无幂等/SchemaGuard/markChunk）。

| slot | 断言要点 |
| --- | --- |
| `branchC_happyPath()` | 本地 UPSERT：行落库、sealInto 以本地 origin==nodeId_/epoch==streamEpoch_ 入 changelog、table_state 更新；commit 成功 |
| `branchC_sealUsesLocalOrigin()` | `sealInto` origin==nodeId_、schemaVer/Fp 用本地 `schemaVer_`/`schemaFp_`（非入站值） |
| `branchC_noIdempotencyNoSchemaGuard()` | branch C **跳过** push 幂等检查与 SchemaGuard（isInbound=false 分支不进这两块） |
| `branchC_requiresAllocatedOriginSeq()` | `originSeq<=0` → `rec_.abort()` + rollback、`errorCode==E_SYNC_INIT`、errorMsg 含 "local origin_seq must be allocated"（本地写须先分配 seq） |
| `branchC_freshCaptureSeal()` | 本地写经 SessionRecorder fresh 捕获后 sealInto（与分支 A 的转发对照） |
| `branchC_rowErrorAbortsAll()` | 本地 UPSERT 行错误 → abort + rollback，映射约束/FK 码 |
| `branchC_tableStateUpdateFailureRollsBack()` | branch C 的 `ts_.applyMutations` 失败 → rollback、`errorCode=="TABLE_STATE_UPDATE"`，全回退 |

#### 6.6.4 三分支共性、`extractMutations` 与 table_state 撕裂处理

| slot | 断言要点 |
| --- | --- |
| `execRoutesByKind()` | `execute()` 按 `WriteKind` 正确路由：InboundChangeset→branchA、InboundSelectionPush/LocalWrite→branchBC；未知 kind→`errorCode=="UNKNOWN_KIND"` |
| `txnBeginFailureNoWork()` | `txn.begin` 失败 → 立即返回 `errorCode=="TXN_BEGIN"`，未做任何写 |
| `txnCommitFailureSurfaces()` | `txn.commit` 失败 → `errorCode=="TXN_COMMIT"`，errorMsg 透传（注意此分支已无显式 rollback，依赖 WriteTxn 析构兜底回滚——列为不变量断言） |
| `anyStepFailureNoHalfCommit()` | 参数化遍历各失败注入点（schema/apply/advance/table_state/append/seal/markChunk）→ 每点失败后库**无半提交**（业务行、changelog、applied_vector、table_state 全回退一致） |
| `allBranchesMaintainTableStateNoFullScan()` | 三分支均经增量 mutation（extractMutations 或 PreScan）更新 table_state，断言执行期间**无 `SELECT … FROM <业务表>` 全表扫描**（仅按 PK 的 LIMIT 1 预扫）（I-07/I-08 性能不变量） |
| `extractMutationsInsert()` | `extractMutations` 对 INSERT：pkHash/afterHash 正确、isInsert=true、isDelete=false |
| `extractMutationsUpdate()` | UPDATE：pkHash 取 old（PK 不变）、before+after 双 hash、isInsert/isDelete=false |
| `extractMutationsDelete()` | DELETE：pkHash/beforeHash（old 值）、isDelete=true、afterHash 空 |
| `extractMutationsSkipsRejectedTables()` | `__sync_*` 与非白名单表跳过（`isAllowedSyncTable`），不污染 table_state |
| `extractMutationsHashFormatMatchesRowHash()` | before/after hash 用列名排序 `QVariantMap` 委托 `TableStateStore::rowHash()`，与 `resetFromBaseline()` 全扫格式一致（M-02/M-03，防基线后增量 checksum 分歧） |
| `extractMutationsStaticEquivInstance()` | `extractMutationsStatic` 产出与实例 `extractMutations` 在同一 changeset/库上逐字段一致（submitImportSync 用静态版避免全扫，M-01） |
| `extractMutationsPragmaFailFallback()` | `PRAGMA table_info` 失败时列名回退 `_col_%1`，仍产出 mutation（不崩、不丢行）——覆盖兜底分支 |
| `tableStateStaleSinceNonFatal()` | （设计预留）当 table_state 更新被建模为非致命时，`WriteResult.tableStateStaleSince` 非空标记陈旧、`ok` 仍 true、业务写已提交——验证该字段被正确回填且不触发回滚（与当前 M-03「失败即回滚」实现互斥的演进路径，本用例按实现现状二选一：现状下断言 staleSince 始终为空且失败走回滚） |

**关联需求/错误码**：设计 §5.4（三分支）、G-03（统一写模板）、G-05（严格连续，branch A）、I-07（branch A table_state）、I-08（branch B/C UPSERT + table_state）、C-09（行错整块回滚）、C12（下行/上行不叠 rank）、H-01/H-03/H-05、M-01/M-02/M-03/M-04；错误码 `E_SYNC_SCHEMA_MISMATCH`、`E_SYNC_APPLY_FK`、`E_SYNC_APPLY_CONSTRAINT`、`E_SYNC_PAYLOAD_CORRUPT`、`E_SYNC_TRANSPORT`、`E_SYNC_INIT`，及内部状态码 `GAP_PENDING` / `AV_ADVANCE` / `TABLE_STATE_UPDATE` / `CLOG_FORWARD` / `SEAL` / `TXN_BEGIN` / `TXN_COMMIT` / `SESSION_BEGIN` / `E_DB_UPSERT` / `UNKNOWN_KIND`。

**目标行覆盖率 ≥ 88%**：三分支主路径 + 每分支全部失败注入点 + 幂等/缺口/schema 边界 + `extractMutations` 三 op 与静态/实例等价 + table_state 撕裂处理全覆盖。剩余差额来自 `extractMutationsStatic` 与实例方法的重复代码块（同一用例难同时覆盖两份拷贝的等价分支），由 `extractMutationsStaticEquivInstance` + `extractMutationsPragmaFailFallback` 双向逼近。

> **测试性要求（对实现）**：分支 A/B/C 的各失败注入点（advance/table_state/append/seal/markChunk 失败）需可经注入坏句柄、预置冲突行或 mock store 触发；`allBranchesMaintainTableStateNoFullScan` 依赖第 3 节可侦测 `wconn` 对全表 `SELECT` 计数的能力（或经 `sqlite3_trace` 钩子统计），列为夹具增强项。
## 7. schema 守卫子系统单元测试

本节覆盖 `src/sync/schema/**` 的四个 store：`SchemaEligibility`（合格性判定，275 行）、`TableStateStore`（表状态/判等三元组，275 行）、`SchemaGuard`（版本/指纹守卫，103 行）、`QuarantineStore`（载荷隔离/重放，70 行）。这些模块是「能不能同步」与「内容是否等价」的第一道闸门，逻辑分支密集且多为纯/半纯判定，适合用第 3 节夹具做高密度单元测试。本节目标行覆盖率 **≈90%**（`src/sync/schema` 在 §1.5 预算表中目标 88%，本节四个 store 因分支可穷举上调至约 90%）。

四套件均使用第 2.6 节隔离骨架（独立临时库 + 独立 `connName`）；`SchemaEligibility`/`SchemaGuard` 的纯解析路径可走 `:memory:`，涉及 `__sync_table_state`/`__sync_quarantine` 落库与重开的 `TableStateStore`/`QuarantineStore` 用临时文件库。所有期望失败的用例统一用 §3.7 的 `ASSERT_ERR_CODE` 宏断言错误码。数据集复用第 3.2 节 `tests/data/sync/` 建表脚本。

### 7.1 套件总览

| 套件 | 被测目标 | 关联需求/不变量 | 关联错误码 | 重点 |
| --- | --- | --- | --- | --- |
| `tst_schema_eligibility` | `SchemaEligibility::{verify,expandSyncTables,introspect,hasUpsertTarget,isShadowTable}` | G-04 合格性规则、FR-2 表选择、C-08 空集合扩展 | `E_SYNC_UNSUPPORTED_SCHEMA` | 逐表合格/拒绝判定（重点） |
| `tst_table_state` | `TableStateStore::{applyMutations,readState,resetFromBaseline,rowHash}` | G-06 判等三元组、§6.2 顺序无关模加、NFR-* 禁全表扫描 | —（无失败码，纯状态正确性） | 判等三元组 + 顺序无关模加（重点） |
| `tst_schema_guard` | `SchemaGuard::{verifyPayload,computeFingerprint}` | G-04 模式协商、FR-2 schema 一致 | `E_SYNC_SCHEMA_MISMATCH` | 版本/指纹双因子守卫 |
| `tst_quarantine_store` | `QuarantineStore::{quarantine,drainReady,markReplayed}` | H-01 隔离重放、G-04 低版本载荷不丢 | —（路由判定，不抛码） | 迁移后 FIFO 重放 |

### 7.2 `tst_schema_eligibility`（重点）

逐表用第 3.2 节数据集断言 G-04 规则：**合格 = 普通 rowid / WITHOUT ROWID 表 + 显式非空 PK + 可用完整唯一冲突目标**；**拒绝 = 虚表 / 视图 / 影子表 / 无显式非空 PK / 仅 partial 或表达式唯一目标**，回填 `rejected` 并使 `verify` 返回 false（错误码 `E_SYNC_UNSUPPORTED_SCHEMA`）；**生成列仅从列集中排除该列，不拒整表**。

| 测试用例（slot 名） | 数据集 / 输入 | 断言要点 |
| --- | --- | --- |
| `verify_rowidExplicitPk_eligible` | `01_rowid_explicit_pk.sql` | `verify(db,{t},&rej,&err)==true`；`rej.isEmpty()`；普通 rowid + 单列非空 PK → 合格 |
| `verify_compositePk_eligible` | `02_composite_pk.sql` | `verify(...)==true`；复合 PK 全列非空 → 合格；`introspect.pkCols=={a,b}` 按 pk 序 |
| `verify_withoutRowid_eligible` | `03_without_rowid.sql` | `verify(...)==true`；`WITHOUT ROWID` 表合格（PK 即行标识） |
| `verify_noExplicitPk_rejected` | `04_no_explicit_pk.sql` | `ASSERT_ERR_CODE(verify(db,{t},&rej,&_err),"E_SYNC_UNSUPPORTED_SCHEMA")`；**`rej` 含该表名**；隐式 rowid 不可跨节点稳定 |
| `verify_view_rejected` | `05_view.sql` | `verify(db,{v},&rej,&err)==false`；`rej` 含视图名；`introspect.isView==true` |
| `verify_fts5Vtab_rejected` | `06_fts5_vtab.sql`（FTS5 虚表名） | `verify(...)==false`；`rej` 含虚表名；`introspect.isVirtual==true` |
| `verify_shadowTable_rejected` | `06_fts5_vtab.sql`（`*_data`/`*_idx` 影子表名） | `verify(...)==false`；`rej` 含影子表名；`isShadowTable(db,shadow,&err)==true` |
| `verify_partialOrExprUnique_rejected` | `10_partial_expr_unique.sql` | `ASSERT_ERR_CODE(verify(...),"E_SYNC_UNSUPPORTED_SCHEMA")`；`hasUpsertTarget(db,t,&err)==false`（无完整非 partial 唯一目标）；`rej` 含表名 |
| `verify_generatedColumn_excludedNotRejected` | `07_generated_column.sql` | `verify(...)==true`（**不拒表**）；表合格，生成列仅被排除出列集，普通列+PK 仍构成有效目标 |
| `verify_mixedSet_partialRejection` | `01` + `04` + `05` 合并库 | `verify(db,{ok,noPk,view},&rej,&err)==false`；`rej` 恰含 `{noPk,view}` 两项、不含 `ok`；逐表判定相互独立 |
| `expandSyncTables_empty_returnsAllUserTables` | 多用户表 + 1 个 `__sync_*` + `sqlite_*` | `expandSyncTables(db,{},&err)` 返回全部用户表，**排除** `sqlite_%` 与 `__sync_%`（C-08） |
| `expandSyncTables_explicit_passthrough` | 同上 | `expandSyncTables(db,{a,b},&err)` 原样返回 `{a,b}`，非空集合不扩展 |
| `introspect_fieldsCorrect_rowid` | `01_rowid_explicit_pk.sql` | `exists==true`，`isView/isVirtual/isShadow==false`，`pkCols` 正确，`pkNotNull==true` |
| `introspect_nullablePk_pkNotNullFalse` | PK 列允许 NULL 的表 | `introspect.pkNotNull==false` → `verify` 据此拒绝（非空 PK 是合格前提） |
| `introspect_missingTable_existsFalse` | 不存在的表名 | `introspect.exists==false`；`verify` 对缺表回填 `rej` 并 false |
| `hasUpsertTarget_fullUnique_true` | 含普通 `UNIQUE(col)` 或 PK 的表 | `hasUpsertTarget(...)==true`（PK 本身即合法 ON CONFLICT 目标） |
| `hasUpsertTarget_partialOnly_false` | `10_partial_expr_unique.sql` | `hasUpsertTarget(...)==false`（partial / 表达式 unique 不可作冲突目标） |
| `isShadowTable_true_false` | FTS5 影子表名 vs 普通表名 | 影子表 `==true`；普通用户表 `==false`（名称含 `_` 但非虚表前缀者不误判） |

**关联**：G-04（合格性规则）、C-08（空集合扩展为全部用户表）、FR-2（同步表选择）；唯一错误码 `E_SYNC_UNSUPPORTED_SCHEMA`，每条拒绝用例均以码值断言并校验 `rejected` 含表名（而非仅断言 false）。
**目标覆盖率**：`SchemaEligibility.cpp` 行覆盖率 ≥ **92%**（合格/拒绝五分支 + 生成列排除 + expand 两分支 + introspect 各字段路径基本可穷举）。

### 7.3 `tst_table_state`（重点：判等三元组）

验证 §6.2 / G-06 的内容校验和算法：**content_checksum 为顺序无关的模加**（INSERT `+= H(after)`，DELETE `-= H(before)`，UPDATE `+= H(after) - H(before)`，quint64 模 2^64 自然回绕，以 hex 串存储）；**判等三元组 = `schema_fingerprint` + `row_count` + `content_checksum`**，**`high_water_seq` 不参与判等**；禁止全表扫描（增量更新只触 `__sync_table_state` 单行）。`applyMutations` 须在活跃 `WriteTxn` 内调用。

| 测试用例（slot 名） | 输入 / 操作 | 断言要点 |
| --- | --- | --- |
| `applyMutations_insert_increments` | 单条 INSERT mutation | `readState` 后 `row_count==1`；`checksum == hashToU64(afterHash)`（模加初值） |
| `applyMutations_delete_decrements` | 先 INSERT 再 DELETE 同行 | `row_count==0`；`checksum==0`（`+H` 后 `-H` 抵消，回到空态） |
| `applyMutations_update_deltaOnly` | UPDATE（before≠after） | `row_count` 不变；`checksum` 变化量 `== H(after) - H(before)`（模运算） |
| `applyMutations_batchMixed_correct` | 一批 INSERT/DELETE/UPDATE 混合 | 终态 `checksum`/`row_count` 等于逐条串行结果 |
| `applyMutations_orderIndependent_sameChecksum` | **同一批 mutation 乱序应用两次（打乱顺序）** | 两次终态 `checksum` 完全相等；`row_count` 相等（顺序无关，§6.2） |
| `applyMutations_modAdd_noSelfCancel_vsXor` | 同一行哈希 H 出现偶数次（如两次 INSERT 同哈希） | 模加结果 `== 2*H mod 2^64 ≠ 0`；对照 XOR 实现会自抵消为 0 → **证明模加不被偶数次同哈希清零** |
| `readState_triple_found` | 应用若干 mutation 后读 | `found==true`；回填 `fp`/`checksum`/`rowCount` 三元组与预期一致 |
| `readState_neverSynced_foundFalse` | 从未同步的表 | 返回 true（查询无错），`found==false`；fp/checksum/rowCount 不被填充（J-12 语义） |
| `equality_sameContentDiffHighWater_green` | 两库内容相同、`high_water_seq` 不同 | 三元组（fp+row_count+checksum）逐项相等 → **判等为绿**（G-06：high_water 不参与判等） |
| `equality_diffChecksumSameHighWater_red` | 两库 `high_water_seq` 相同、内容（checksum）不同 | 三元组不等（checksum 不同）→ **判等为红**（high_water 相同不能掩盖内容差异） |
| `resetFromBaseline_recomputesFromScratch` | 直接改基表后调用 `resetFromBaseline` | 全表重算得到的 `checksum`/`row_count` 等于按当前全量行逐行模加的期望值；覆盖增量漂移 |
| `resetFromBaseline_multiTable` | 多表一次重置 | 每表各自 `__sync_table_state` 行独立重算正确，互不串扰 |
| `rowHash_canonicalEncoding_stable` | 同一逻辑行不同 `QVariantMap` 插入序/同值不同表示 | `rowHash` 结果稳定一致（规范编码：列序归一、类型/值规范），跨进程可重现 |
| `rowHash_distinctRows_differ` | 两条仅一列不同的行 | `rowHash` 不同（无显著碰撞） |
| `applyMutations_noFullScan_boundedSql` | 一批 N 条 mutation | **断言无全表扫描**：通过可计数 query 钩子断言执行的 SQL 行数有界（与 N 成正比、与基表总行数无关），增量只命中 `__sync_table_state` 单行 upsert |

**关联**：G-06（判等三元组、high_water 不参与判等）、§6.2（顺序无关模加增量算法）、NFR 性能（禁全表扫描）。本套件无失败错误码（纯状态正确性断言）。
**目标覆盖率**：`TableStateStore.cpp` 行覆盖率 ≥ **90%**（applyMutations 三类 delta + readState found/未找到 + resetFromBaseline 单/多表 + rowHash + hashToU64 均覆盖）。

> 顺序无关与模加的关键判据：`orderIndependent_sameChecksum` 锁定「乱序同终态」，`modAdd_noSelfCancel_vsXor` 锁定「模加 ≠ XOR」——两者共同排除了用 XOR/异或聚合时偶数次同哈希被错误抵消的缺陷。

### 7.4 `tst_schema_guard`

验证版本 + 指纹双因子守卫。`verifyPayload(payloadVer,payloadFp,err)` 在版本或指纹与 `setLocal` 设定的本地基线不符时返回 false 并回填 `E_SYNC_SCHEMA_MISMATCH`；构造参数 `verifyFingerprint=false` 时跳过指纹比对（仅比版本）。`computeFingerprint` 对列序/列类型敏感、对**表顺序**不敏感（内部排序）。

| 测试用例（slot 名） | 输入 / 操作 | 断言要点 |
| --- | --- | --- |
| `verifyPayload_matchBoth_ok` | `setLocal(v,fp)` 后传相同 `(v,fp)` | `verifyPayload(...)==true`；`err` 未被设置 |
| `verifyPayload_versionMismatch_rejected` | 本地 v=3，载荷 v=4，指纹相同 | `ASSERT_ERR_CODE(g.verifyPayload(4,fp,&_err),"E_SYNC_SCHEMA_MISMATCH")` |
| `verifyPayload_fingerprintMismatch_rejected` | 版本相同，指纹不同 | `ASSERT_ERR_CODE(g.verifyPayload(v,otherFp,&_err),"E_SYNC_SCHEMA_MISMATCH")` |
| `verifyPayload_skipFingerprint_versionOnly` | `SchemaGuard g(false)`；指纹不同但版本相同 | `verifyPayload(v,otherFp,&err)==true`（verifyFingerprint=false 跳过指纹比对） |
| `verifyPayload_skipFingerprint_versionStillChecked` | `SchemaGuard g(false)`；版本不同 | 仍 `==false` 且回填 `E_SYNC_SCHEMA_MISMATCH`（关闭指纹不豁免版本） |
| `computeFingerprint_columnOrderSensitive` | 两表列定义顺序不同 | 指纹**不同**（对列序敏感） |
| `computeFingerprint_typeSensitive` | 仅某列类型不同（如 INTEGER vs TEXT） | 指纹**不同**（对列类型敏感） |
| `computeFingerprint_tableOrderInsensitive` | 同一组表，传入 `tables` 顺序不同 | 指纹**相同**（内部排序 → 对表序不敏感） |
| `computeFingerprint_pkSensitive` | PK 列集不同、其余相同 | 指纹**不同**（PK 列纳入指纹） |
| `fingerprint_returnsLocal` | `setLocal(v,fp)` 后 | `g.fingerprint()==fp`（回读本地指纹） |

**关联**：G-04（模式协商）、FR-2（schema 一致前提）；唯一错误码 `E_SYNC_SCHEMA_MISMATCH`，版本不符与指纹不符两条独立路径均以码值断言。
**目标覆盖率**：`SchemaGuard.cpp` 行覆盖率 ≥ **95%**（103 行，verifyPayload 三分支 + verifyFingerprint 双态 + computeFingerprint 排序/敏感性路径基本全覆盖）。

### 7.5 `tst_quarantine_store`

验证「低 epoch / 模式版本超前的载荷先隔离，迁移后按到达序重放」。`quarantine` 落库一条隔离记录；`drainReady(db,currentSchemaVer)` 仅返回 `payload_schema_ver <= currentSchemaVer` 的可重放项，按 `id ASC`（到达序）返回 `(id, payload)` 对且**不删除**；`markReplayed(db,id)` 删除已重放行使其不再返回（H-01）。

| 测试用例（slot 名） | 输入 / 操作 | 断言要点 |
| --- | --- | --- |
| `quarantine_persists` | `init` 后 `quarantine(db,origin,seq,epoch,schemaVer,payload,&err)` | 返回 true；隔离表新增一行，字段（origin/seq/epoch/schema_ver/payload）可回读 |
| `drainReady_onlyLeqCurrent` | 隔离 ver={3,5,7}，`drainReady(db,5)` | 仅返回 ver∈{3,5} 的项；ver=7（超前）**不返回**，仍留库待后续迁移 |
| `drainReady_returnsIdPayloadPairs` | 隔离若干项后 drain | 返回 `QList<QPair<qint64,QByteArray>>`，每对 `(id, payload)` payload 字节与入库一致 |
| `drainReady_fifoOrderById` | 乱序入库多条（ver 均 ≤ current） | 返回顺序按 `id ASC`（即到达序）严格递增，多条 FIFO |
| `drainReady_doesNotDelete` | drain 后不调用 markReplayed，再次 drain | 两次 drain 返回相同集合（drainReady 只读不删） |
| `markReplayed_removesRow` | drain 得到 id 后 `markReplayed(db,id)` | 该 id 行被删除；后续 `drainReady` 不再包含它 |
| `markReplayed_partialThenRedrain` | drain 多条，仅对部分调用 markReplayed | 再次 drain 仅返回未 markReplayed 的剩余项，已重放项消失 |
| `migrationReplay_endToEnd` | 入库 ver=7；`drainReady(5)` 空 → 升级 current=7 → `drainReady(7)` | 升级前隔离不丢、升级后可重放；模拟迁移后排空流程 |

**关联**：H-01（隔离记录 id 升序重放、drain 不删需显式 markReplayed）、G-04（低版本/版本超前载荷不丢失，迁移后重放）。本套件为路由/排空判定，不抛错误码。
**目标覆盖率**：`QuarantineStore.cpp` 行覆盖率 ≥ **90%**（70 行，quarantine 落库 + drainReady 过滤/排序 + markReplayed 删除路径全覆盖）。

### 7.6 本节覆盖率小结

| 套件 | 被测文件 | 目标行覆盖率 |
| --- | --- | --- |
| `tst_schema_eligibility` | `SchemaEligibility.cpp`（275） | ≥ 92% |
| `tst_table_state` | `TableStateStore.cpp`（275） | ≥ 90% |
| `tst_schema_guard` | `SchemaGuard.cpp`（103） | ≥ 95% |
| `tst_quarantine_store` | `QuarantineStore.cpp`（70） | ≥ 90% |
| **schema 守卫子系统加权** | `src/sync/schema/**` | **≈ 90%**（≥ §1.5 预算 88%） |
## 8. transport 传输与 conflict 冲突仲裁子系统单元测试

本节覆盖 `src/sync/transport/`（基于文件系统的原子发布与幂等消费）与 `src/sync/conflict/`（冲突仲裁、rebase、防回声路由）两个子系统的单元测试。两子系统的核心不变量分别是「**接收端永远只见到完整制品**」（.ready 哨兵语义）与「**冲突解析与路由是确定的全序判定**」（无论应用顺序最终态一致、不回推来源）。

**目标覆盖率**：transport 子系统 ≈ 82%（IO 失败分支多、`Q_OS_UNIX` 门控的 fsync 路径在 CI 上仅能部分触达，故定 82%）；conflict 子系统 ≈ 95%（纯函数判定、分支密集且无 IO，几乎可全覆盖；`ConflictArbiter` 仅 33 行、整个 conflict 目录约 93 行）。

### 8.0 子系统测试性前置约定（对实现的要求）

下列三点是本节多套件可确定运行的前提，列为对被测实现的测试性要求（与 §3.6 可控时钟一致）：

- **可注入时钟**：`AckChannel`（`ackMaxDelayMs`）与 `InboxLedger`（`first_seen_ms`/`consumed_ms`/`stalePending` 的 `gapTimeoutMs`）当前直接调用 `QDateTime::currentMSecsSinceEpoch()`。为消除时序抖动、避免真实 `sleep`，须改为接受可注入的 `std::function<qint64()> nowMs`（默认绑定系统时钟）。本节用例以 `FakeClock`（§3.6）推进虚拟时间断言时机；在注入点落地前，对应「时机」类断言以 `QTRY_VERIFY` + 真实短延时作为降级写法，但不作为长期方案。
- **`.ready` 哨兵契约**：接收端（`InboxWatcher`/第三方搬运）必须以 `<name>.ready` 的存在为「主文件 `<name>` 已完整可读」的唯一判据；主文件单独存在而无 `.ready` 不可消费。`OutboxWriter::writeAtomic` 的发布顺序（tmp→fsync→rename→写 `.ready`）保证此契约。
- **`artifactReady` 信号**：`InboxWatcher` 当前实现走同步 `scan()`，`artifactReady` 信号保留为前向兼容 hook 且**不**发射（见 `InboxWatcher.h` I-10 注释）。本节针对该信号的用例标注为 `[FWD]`（前向兼容占位），默认 `QSKIP`，仅在事件循环驱动的 worker 落地后启用。

### 8.1 tst_outbox_writer

**被测**：`OutboxWriter::{write, writeAck, writeAtomic}`（`src/sync/transport/OutboxWriter.{h,cpp}`）。
**关联需求/错误码**：原子发布协议（plan T-transport、F-13 制品命名）；IO/durability 失败统一回 `E_SYNC_TRANSPORT`（含 tmp 打开失败、partial write、`flush`/`fsync` 失败、`rename` 失败、`.ready` 写入失败、目录 `fsync` 失败）。
**夹具**：`QTemporaryDir` 作 outbox 目录；POSIX-only 分支（`#ifdef Q_OS_UNIX` 的 fsync/dir-fsync）在 UNIX runner 上覆盖。

| slot | 断言要点 |
| --- | --- |
| `init()` | 公共：建临时目录作为 outbox 根。 |
| `writeAtomic_publishesReadyAfterPayload` | `write("a__1.payload", data)` 成功后，主文件 `a__1.payload` 与 `a__1.payload.ready` 均存在且内容/大小正确；`.ready` 为空文件。 |
| `writeAtomic_noHalfVisibleDuringWrite` | 发布**中途**目录内不得出现「无 `.ready` 的主文件」可被消费态：断言任一时刻只存在 `.tmp`（尚未 rename）或「主文件 + `.ready`」（已完成），不存在「主文件存在但 `.ready` 缺失」的稳定中间态。以「rename 后立刻、写 `.ready` 之前」为关键观测点（必要时借 fault-hook 或在 rename 与 .ready 间插桩断言）。验证顺序：tmp→rename→`.ready`。 |
| `writeAtomic_tmpRenamedNotCopied` | 发布后目录内**无** `.tmp` 残留（rename 而非 copy），主文件 inode 即原 tmp（POSIX：`rename` 原子替换）。 |
| `receiver_seesReadyBeforeReadingPayload` | 模拟接收端：仅当 `<name>.ready` 存在时才读 `<name>`；构造「`.ready` 已落、主文件可读」与「`.ready` 未落」两态，断言前者读到完整 data、后者不读。 |
| `writeAtomic_overwritesExistingFinal` | 同名 `finalName` 二次 `write` 不报错（`rename` 原子替换旧主文件，I-11），第二次内容覆盖第一次。 |
| `writeAck_artifactNaming` | `writeAck(ddl::ackArtifactName("B","A",nowMs), data)` 发布的文件名符合 ACK 命名约定（`fromPeer__toPeer__…__ack`），并伴随同名 `.ready`。 |
| `writeAtomic_dirNotWritable_E_SYNC_TRANSPORT` | outbox 目录设为不可写（`chmod 0500`）或 tmp 打开失败 → 返回 false 且 `*err` 反映传输失败（`ASSERT_ERR_CODE(..., "E_SYNC_TRANSPORT")`；当前实现 err 文案为 "cannot open tmp file/cannot create outbox dir"，断言映射到 `E_SYNC_TRANSPORT` 语义）。失败后目录内**无**主文件、无 `.ready`、无 `.tmp` 残留（cleanup 完整，M-08）。 |
| `writeAtomic_readyWriteFails_cleansUpPayload` | 注入 `.ready` 写入失败（如 `.ready` 路径预先占位为只读/不可创建）→ 失败时主文件与 `.ready` 均被清除（M-08：不留「孤儿主文件」），返回 false。 |
| `writeAtomic_partialWrite_cleansTmp` `[UNIX]` | 注入 `write()` 返回字节不足（满盘/配额）→ 返回 false、移除 `.tmp`、无主文件。 |

**覆盖说明**：`Q_OS_UNIX` 下 `fsync`/dir-`fsync` 失败分支（M-04/M-06）难以在普通文件系统稳定注入，列为「尽力覆盖」，缺口计入 transport 82% 预算。

### 8.2 tst_inbox_ledger

**被测**：`InboxLedger::{init, markSeen, markConsumed, markCorrupt, status, pendingSeen, stalePending}`；`enum LedgerStatus{Seen,Consumed,Corrupt,Unknown}`（`InboxLedger.{h,cpp}`）。DDL `__sync_inbox_ledger(artifact_name PK, status, first_seen_ms NOT NULL, consumed_ms)`。
**关联需求/错误码**：制品级幂等消费（F-12：已 consumed 再现跳过）；M-01 gap 检测（`stalePending` → `E_SYNC_GAP`/baseline 回退的输入）。
**夹具**：`SqliteFixture`（临时文件库，先 `SyncDDL` 建 `__sync_inbox_ledger`）；`FakeClock` 注入 `nowMs`（见 §8.0）。

| slot | 断言要点 |
| --- | --- |
| `init_requiresTable` | 表已建时 `init` 返回 true；表缺失时返回 false 且回填 `*err`（`init` 仅探测表存在）。 |
| `markSeen_setsSeenAndFirstSeenMs` | `markSeen("art1")` 后 `status==Seen`，`first_seen_ms == 注入 nowMs`，`consumed_ms` 为 NULL。 |
| `markSeen_idempotentInsertOrIgnore` | 对已存在制品再 `markSeen`（即使 nowMs 推进）不改变 `first_seen_ms`、不改变 status（`INSERT OR IGNORE` 语义）。 |
| `transition_seen_to_consumed` | `markSeen`→`markConsumed("art1")`：`status==Consumed`，`consumed_ms == 推进后的 nowMs`，`first_seen_ms` 不变。 |
| `transition_to_corrupt` | `markSeen`→`markCorrupt("art1")`：`status==Corrupt`（`markCorrupt` 不写 `consumed_ms`）。 |
| `consumed_idempotent_skip` | **核心幂等**：制品已 `Consumed` 后，模拟「重复到达」再次进入消费流程时应被跳过——断言 `status` 仍为 `Consumed`，且（配合 `InboxWatcher`）不再返回为可消费（F-12）。 |
| `status_unknown_whenAbsent` | 未登记的制品 `status` 返回 `Unknown`。 |
| `pendingSeen_onlyReturnsSeen` | 混合状态（seen / consumed / corrupt）后 `pendingSeen` 仅返回 status==seen 的制品名，不含 consumed/corrupt。 |
| `pendingSeen_emptyWhenNone` | 无 seen 制品时返回空列表。 |
| `stalePending_returnsAgedSeen` | `markSeen("old")`（nowMs=T0），推进 `FakeClock` 超过 `gapTimeoutMs`，再 `markSeen("fresh")`（nowMs=T1）→ `stalePending(db, gapTimeoutMs)` 仅含 `"old"`（`first_seen_ms < now - gapTimeoutMs`）。 |
| `stalePending_excludesConsumed` | 已 consumed 的老制品不出现在 `stalePending`（仅 status==seen 参与）。 |

**覆盖说明**：状态机四态迁移 + 三个查询的全分支基本可达；conflict/transport 中本套件属高覆盖部分。

### 8.3 tst_inbox_watcher

**被测**：`InboxWatcher::scan(db) -> QStringList`（返回完整制品路径）；signal `artifactReady`（`[FWD]` 不发射，见 §8.0）（`InboxWatcher.{h,cpp}`，QObject）。
**关联需求/错误码**：三时机扫描（启动全量补扫 / watcher / timer）；以 `.ready` 哨兵 + ledger 为消费判据；幂等（已 consumed/corrupt 跳过）。
**夹具**：`SqliteFixture`（带 `__sync_inbox_ledger`）+ `InboxLedger` 真实实例；`QTemporaryDir` 作 inbox，用 `OutboxWriter` 向其发布制品以构造真实「主文件+.ready」对。

| slot | 断言要点 |
| --- | --- |
| `scan_returnsReadyArtifacts` | inbox 内有 `a.payload`+`a.payload.ready`、`b.payload`+`b.payload.ready` → `scan` 返回两者**完整路径**；返回后 ledger 中两者 status==Seen（`markSeen` 已写入）。 |
| `scan_readyOnlyNoPayload_notConsumable` | 仅有 `c.payload.ready` 而**缺主文件** `c.payload`（哨兵先到、主文件未达）→ `scan` **不**将 `c` 计为可消费（`QFileInfo::exists(artifactPath)` 为假，过滤掉）。 |
| `scan_skipsConsumed` | 预先 `ledger.markConsumed("a.payload")`，inbox 含 `a.payload`+`.ready` → `scan` 不返回 `a`（已消费幂等跳过）。 |
| `scan_skipsCorrupt` | 预先 `markCorrupt("a.payload")` → `scan` 不返回 `a`。 |
| `scan_duplicateEventIdempotent` | 同一 inbox 连续 `scan` 两次（模拟重复 watcher/timer 事件）：第二次仍可返回未消费制品，但 ledger 中 `first_seen_ms` 不因二次 scan 改变（`markSeen` 幂等）；制品被标 consumed 后再 scan 不返回（配合 ledger）。 |
| `scan_startupFullRescan` | 启动场景：inbox 预置多份未消费制品（无任何先验 ledger 记录）→ 首次 `scan` 全量返回并全部置 Seen（模拟「启动补扫」时机）。 |
| `scan_fifoOrderOldestFirst` | 按 mtime 顺序投放多份 `.ready`，断言 `scan` 返回顺序为 oldest-first（`QDir::Time|QDir::Reversed`，M-08）。 |
| `scan_emptyOrMissingDir` | inbox 目录为空或不存在 → 返回空列表，不抛错。 |
| `scan_markSeenFailureSkips` | 注入 `markSeen` 失败（如临时令 ledger 表不可写）→ 该制品本轮被跳过、不计入返回（M-5：避免无 ledger 记录导致无限重处理）。 |
| `artifactReady_notEmitted` `[FWD]` | （默认 `QSKIP`）当前实现 `scan()` 不发 `artifactReady`；占位用例，待事件循环 worker 落地后改为正向断言。 |

### 8.4 tst_ack_channel

**被测**：`AckChannel(OutboxWriter&, nodeId, ackMaxDelayMs=5000)`；`scheduleChangesetAck`、`schedulePushChunkAck`、`flush`、`maybeFlush`（私有，经 schedule 间接触达）、`nextDeadlineMs`（`AckChannel.{h,cpp}`）。
**关联需求/错误码**：F-14（`ackMaxDelayMs` 内必发 ACK，攒批）；J-01（`fromPeer=nodeId`）；H-04（PushChunkAck 用 `toPeer`/`pushId` 路由）；写出经 `OutboxWriter::writeAck` → 失败传 `E_SYNC_TRANSPORT`。
**夹具**：注入时钟 `FakeClock`（§8.0）；`OutboxWriter` 指向 `QTemporaryDir`；真实 `PayloadCodec`；用 `AckArtifacts`（§3.6）构造 `ChangesetAck`/`PushChunkAck`。失败注入用「不可写 outbox」。

| slot | 断言要点 |
| --- | --- |
| `schedule_beforeDeadline_noFlush` | `ackMaxDelayMs=500`，`scheduleChangesetAck(ack)` 后 `FakeClock` 仅推进 200ms（未达 deadline）→ outbox 内**无** ACK 文件（`maybeFlush` 未触发），`nextDeadlineMs == lastFlushMs+500`。 |
| `schedule_atDeadline_autoFlush` | schedule 后推进 ≥ 500ms，再次 `scheduleChangesetAck`（触发 `maybeFlush`）→ 队列被 flush，outbox 出现 ACK 制品 + `.ready`（F-14 必发）。 |
| `flush_immediate` | `flush()` 立即写出所有已排队 ACK，无视 deadline；调用后队列清空、`lastFlushMs` 更新为当前注入时间。 |
| `flush_empty_noop` | 无任何排队 ACK 时 `flush()` 返回 true 且不产生任何文件（空 flush no-op）。 |
| `changesetAck_encodedViaCodec_andWritten` | 排队一个 `ChangesetAck` 并 `flush` → outbox 文件内容等于 `codec.encodeChangesetAck(ack)`；文件名 `ackArtifactName(nodeId, ack.toPeer, nowMs)`（`fromPeer==nodeId`，J-01）。 |
| `pushChunkAck_encodedViaCodec_andWritten` | 排队一个 `PushChunkAck` 并 `flush` → 内容等于 `codec.encodeChunkAck(ack)`；`toPeer` 空时回退用 `pushId` 路由（H-04）。 |
| `mixedAcks_bothTypesFlushed` | 同时排队 Changeset 与 PushChunk 两型，一次 `flush` 两类各自编码、各写一个制品。 |
| `nextDeadlineMs_emptyReturnsMax` | 无 pending 时 `nextDeadlineMs()==INT64_MAX`（`std::numeric_limits<qint64>::max()`，M-03），便于主循环用常规 sleep。 |
| `nextDeadlineMs_pendingReturnsDeadline` | 有 pending 时 `nextDeadlineMs()==lastFlushMs_+ackMaxDelayMs`。 |
| `flush_writeFailure_retainsPending_E_SYNC_TRANSPORT` | outbox 不可写 → `flush` 返回 false、`*err` 反映传输失败；**失败的 ACK 保留在队列**（`pendingChangeset_=failedChangeset`），下轮可重试（断言队列非空）。 |

**覆盖说明**：`maybeFlush` 经 schedule 路径覆盖；时机类断言全部走 `FakeClock`，无真实 `sleep`，套件确定可重放。

### 8.5 tst_conflict_arbiter

**被测**：`ConflictArbiter::{setRankMap, rankOf, beats}`（`ConflictArbiter.{h,cpp}`，约 33 行）。
**关联需求/错误码**：C7 规范序——`(rank, originSeq)` 字典序，`rank` 高者胜、同 rank 比 `originSeq`、再同则以 `originId` 字典序作稳定 tie-breaker（H-01），保证任意应用顺序最终态一致。
**夹具**：无 IO，纯函数；`QHash<QString,int>` 构造 rank 表。

| slot | 断言要点 |
| --- | --- |
| `setRankMap_rankOf` | `setRankMap({{"A",10},{"B",5}})`：`rankOf("A")==10`、`rankOf("B")==5`、未知 origin `rankOf("Z")==0`（默认）。 |
| `beats_higherRankWins` | rank 不等时高 rank 胜，与 seq 无关：`beats("A",1,"B",999)==true`（A rank 高）。 |
| `beats_sameRankHigherSeqWins` | 同源或同 rank 时高 seq 胜：`beats("A",5,"A",3)==true`、`beats("A",3,"A",5)==false`。 |
| `beats_sameRankSameSeq_originTieBreak` | rank 相等、seq 相等 → 以 `aOrigin > bOrigin` 字典序决胜（H-01）：`beats("B",1,"A",1)==true`、`beats("A",1,"B",1)==false`。 |
| `beats_irreflexive` | 自反性：`beats(x,s,x,s)==false`（同一候选不胜过自身；`aOrigin>aOrigin` 为假）。 |
| `beats_antisymmetric` | 反对称：对任意不同候选 `beats(a,b)` 与 `beats(b,a)` 恰一为真（用 data-driven 多组覆盖跨 rank/跨 seq/跨 origin）。 |
| `beats_transitive` | 传递性：构造 a≻b、b≻c 三元组，断言 a≻c（覆盖 rank 链、seq 链、origin 链各一组），证明 `beats` 是全序。 |
| `beats_totalOrder_dataDriven` | `_data()` 列举跨源/同源/全等多组 (aOrigin,aSeq,bOrigin,bSeq) 与期望布尔，确保无并列未决（全序、winner 唯一）。 |

**覆盖说明**：`beats` 三条分支（rank、seq、origin tie-break）+ `rankOf` 默认值全覆盖，本套件支撑 conflict 子系统 ≈95% 目标。

### 8.6 tst_rebase_engine

**被测**：`RebaseEngine::rebase(rebaseBuffer, changeset, QByteArray* rebased, err)`（`RebaseEngine.{h,cpp}`，封装 `sqlite3rebaser_create/configure/rebase/delete`）。
**关联需求/错误码**：rebase 失败统一 `E_SYNC_REBASE_FAILED`（当前实现 err 文案为 `sqlite3rebaser_* failed: <rc>`，断言映射到该错误码语义）。
**夹具**：用「阶段0真实 buffer」——经 `ChangesetApplier`（`SQLITE_CHANGESETAPPLY_NOSAVEPOINT`）应用产生的真实 `rebaseBuffer`（见 §3.3 `ChangesetFixture` + applier outcome），以及真实 `recordChangeset` 的 changeset blob。禁手搓字节。

| slot | 断言要点 |
| --- | --- |
| `rebase_validBufferAndChangeset` | 用阶段0真实 `rebaseBuffer` + 真实 changeset → `rebase` 返回 true，`*rebased` 非空且为合法 changeset（可被 `sqlite3changeset_start` 解析 / 再 apply 成功）。 |
| `rebase_emptyBuffer` | 空 `rebaseBuffer`（无冲突需重写）+ 有效 changeset → 返回 true，`*rebased` 语义等价原 changeset（`configure(0,...)` 合法）。 |
| `rebase_emptyChangeset` | 空 changeset → 返回 true，`*rebased` 为空/可解析的空 changeset。 |
| `rebase_corruptBuffer_E_SYNC_REBASE_FAILED` | 损坏的 `rebaseBuffer`（截断/乱码字节）→ `configure` 或 `rebase` 返回非 `SQLITE_OK`，`rebase` 返回 false 且 `*err` 反映 rebase 失败（`ASSERT_ERR_CODE` 映射 `E_SYNC_REBASE_FAILED`）。 |
| `rebase_corruptChangeset_E_SYNC_REBASE_FAILED` | 有效 buffer + 损坏 changeset → 返回 false，`*err` 反映失败。 |
| `rebase_releasesRebaser_noLeak` | （内存卫生）多次 `rebase` 循环后无泄漏（`sqlite3rebaser_delete`/`sqlite3_free` 均在成功与失败路径调用）；可在 ASAN 构建下运行验证。 |

**覆盖说明**：`sqlite3rebaser_create` 失败分支几乎不可注入（仅 OOM），列为「尽力覆盖」，缺口计入 transport/conflict 预算。

### 8.7 tst_routing_table

**被测**：`RoutingTable::{configure, shouldRoute}`（`RoutingTable.{h,cpp}`）。
**关联需求/错误码**：C2/F-04 防回声——`origin != peer`（不回推来源）**且** `originSeq > peerAckedSeq`（peer 尚未确认）才路由。
**夹具**：无 IO，纯判定；`configure(localNodeId, peers)` 配置多 peer。

| slot | 断言要点 |
| --- | --- |
| `configure_basic` | `configure("self", {"A","B","C"})` 不报错；后续判定基于该配置（localNodeId/peers 记录）。 |
| `shouldRoute_originEqualsPeer_false` | 真值表①：`origin==peer`（制品来自该 peer）→ `shouldRoute("A","A",10,0)==false`（不回推来源，C2）。 |
| `shouldRoute_seqAlreadyAcked_false` | 真值表②：`originSeq <= peerAckedSeq`（peer 已确认）→ `shouldRoute("B","A",5,5)==false`、`shouldRoute("B","A",4,5)==false`。 |
| `shouldRoute_newToPeer_true` | 真值表③：`origin!=peer` 且 `originSeq > peerAckedSeq` → `shouldRoute("B","A",6,5)==true`（应发）。 |
| `shouldRoute_seqEqualBoundary` | 边界：`originSeq == peerAckedSeq` 不发（`>` 而非 `>=`），`originSeq == peerAckedSeq+1` 发。 |
| `shouldRoute_truthTable_dataDriven` | `_data()` 枚举 {origin==peer / origin!=peer} × {seq<acked, seq==acked, seq>acked} 全组合，逐行对照期望布尔（完整真值表）。 |
| `shouldRoute_multiPeerFanout` | 多 peer：同一来源变更 `(origin="A", seq=7)` 对 `{"A":acked=0,"B":acked=3,"C":acked=7}` 分别判定 → A 否（来源）、B 是（7>3）、C 否（7==7 已确认），验证星型扇出按 peer 独立水位判定。 |

**覆盖说明**：`shouldRoute` 两条 return 分支 + 边界全覆盖；与 `ConflictArbiter` 同属 conflict 子系统高覆盖部分，共同支撑 ≈95% 目标。

### 8.8 本节套件与覆盖目标汇总

| 套件 | 被测组件 | 子系统 | 关键需求/错误码 | 用例数 |
| --- | --- | --- | --- | --- |
| `tst_outbox_writer` | `OutboxWriter` | transport | 原子发布(F-13)、`E_SYNC_TRANSPORT` | 10 |
| `tst_inbox_ledger` | `InboxLedger` | transport | 幂等消费(F-12)、gap(M-01) | 11 |
| `tst_inbox_watcher` | `InboxWatcher` | transport | 三时机扫描、`.ready`+ledger 判据 | 10 |
| `tst_ack_channel` | `AckChannel` | transport | ACK SLA(F-14)、J-01/H-04、`E_SYNC_TRANSPORT` | 10 |
| `tst_conflict_arbiter` | `ConflictArbiter` | conflict | 规范全序(C7/H-01) | 8 |
| `tst_rebase_engine` | `RebaseEngine` | conflict | `E_SYNC_REBASE_FAILED` | 6 |
| `tst_routing_table` | `RoutingTable` | conflict | 防回声(C2/F-04) | 7 |

**子系统覆盖目标**：transport ≈ 82%（IO/fsync 失败分支与 `Q_OS_UNIX` 门控路径部分不可在 CI 稳定注入，作「尽力覆盖」缺口）；conflict ≈ 95%（纯判定函数、分支密集、约 93 行，几近全覆盖）。所有时机/时间相关断言均经 `FakeClock` 注入虚拟时间（§8.0），套件确定可重放、无真实 `sleep`。
## 9. selection 上行选择链路与 baseline 基线子系统单元测试

本节覆盖 `src/sync/selection/`（上行「选择 → 闭包 → 冻结 → 分片」链路，628 行）与 `src/sync/baseline/`（全表基线导出/应用，536 行）两个子系统的单元测试。selection 子系统的核心不变量是「**上行只搬运拓扑闭包内、与中心不一致的行，且分片父不晚于子、(push_id,chunk_seq) 幂等续传**」（C13/G-05）；baseline 子系统的核心不变量是「**基线应用是一次全有或全无的重置——成功则 applied_vector/table_state/row_winner 与新锚点同步翻新，失败则旧锚点纹丝不动**」（C16/I-20）。

**目标覆盖率**：selection 子系统 ≈ 85%（`FkClosureBuilder` 241 行分支密集且依赖 `SchemaCatalog` 的 FK 图与 Kahn 拓扑，可控；但 `ChunkStreamer` 的「重复 chunk 同/异 checksum」幂等判定真正落在消费端 `SyncWorker::processSelectionPushArtifact`，单元层只能验证 streamer 自身的分片/预算/拓扑序契约，跨端幂等留待 §（集成）覆盖，故定 85% 而非更高）；baseline 子系统 ≈ 82%（导出/应用主路径与回滚分支可达，但 `applyBaseline` 中 `PRAGMA foreign_keys` 切换、`av.resetTo`/`rw.resetAll`/`ts.resetFromBaseline` 的逐个失败回滚分支需逐条注入，且 row_winner seed 的类型分支多，部分 IO 失败分支难稳定注入，作「尽力覆盖」缺口）。

### 9.0 子系统测试性前置约定（对实现的要求）

下列约定是本节多套件可确定运行的前提：

- **只读快照连接**：`SelectionResolver`/`FkClosureBuilder`/`BaselineManager::exportBaseline` 一律使用 `rconn`（只读快照连接），断言其执行后**业务表与同步影子表行数/内容均无变化**（只读不写库）。本节用 `SqliteFixture` 的 `roConn()`（独立只读连接，`QSQLITE_OPEN_READONLY` 或在独立连接上仅 SELECT）施测；写库副作用断言以「测试前后 `SELECT count(*)`/内容快照比对」实现。
- **FK 图夹具**：`FkClosureBuilder`/`ChunkStreamer` 依赖 `dbridge::detail::SchemaCatalog` 提供 FK 邻接与拓扑信息。本节用 `SchemaFixture`（§3.x）从临时库 `PRAGMA foreign_key_list` 真实构建 catalog，避免手搓 FK 图；环/悬挂父等错误场景通过构造真实建表语句（含 `FOREIGN KEY`）触发。
- **fingerprint 与 pkHash 一致性**：`FrozenEntry.pkHash`/`fingerprint` 与 `RowWinnerStore::pkHash` 共用同一规范（SHA-256 截断），baseline 往返与 closure 套件复用真实指纹函数，禁手填。
- **错误码断言**：沿用 §3.x 的 `ASSERT_ERR_CODE(err, "E_SYNC_*")`——以 `*err` 字符串**前缀/包含**目标错误码判定（实现统一以 `E_SYNC_*: <detail>` 形式回填，如 `BaselineManager` 的 `wrapErr` 前缀化、`ChunkStreamer` 的 `E_SYNC_SELECTION_TOO_LARGE: ...`），而非仅断言 `false`（满足 O3）。
- **ConsistencyCache 注入**：`FkClosureBuilder::build` 的 `pruneConsistent` 经 `ConsistencyCache&` 查询一致性。本节用真实 `ConsistencyCache`（`durable=false` 内存态）作为夹具，预先 `stampFromAuthoritative` 喂入「已与中心一致」的父行指纹来驱动剪枝分支。

### 9.1 tst_selection_resolver

**被测**：`SelectionResolver::{resolvePk, resolveRecord, resolveWhere, rowToPk}`；`struct ResolveResult{table, pk, row}`（`src/sync/selection/SelectionResolver.{h,cpp}`）。
**关联需求/错误码**：MVP 仅「表 + 主键集合」选择（设计 §4.4）；`addWhere` 受限 DSL——raw SQL 已在 `SyncSelection::Builder::build()` 层以 `E_SYNC_SELECTION_EMPTY` 拒绝（H-01），resolver 侧仅解析合法 PK-set；不存在的 pk 解析为「无行」（不报错，交闭包层判悬挂）。
**夹具**：`SqliteFixture` 临时库，建含单列 PK 与复合 PK 的业务表并插入若干行；`roConn()` 只读连接；`SyncSelection::Builder` 构造选择。

| slot | 断言要点 |
| --- | --- |
| `init()` | 公共：建临时库、建 `t_single(id INTEGER PRIMARY KEY, v)` 与 `t_compound(a, b, v, PRIMARY KEY(a,b))`，插入基准行。 |
| `resolvePk_singleRecord` | `Builder().addRecord("t_single","1").build()` → `resolvePk(roConn, sel, &out, &err)==true`，`out` 含一条 `ResolveResult{table=="t_single", pk=="1"}`，`row` 含全列且值正确。 |
| `resolvePk_recordSet` | `addRecords("t_single", {"1","2","3"})` → `out` 含 3 条，顺序与去重符合实现（同 PK 不重复拉行）；每条 `row` 与库内一致。 |
| `resolvePk_multipleTables` | 选择跨 `t_single` 与 `t_compound` 多表多 PK → `out` 按 record 顺序汇总各表行，`table` 字段正确区分。 |
| `resolveRecord_compoundPk` | 经 `resolvePk` 解析 `t_compound` 复合 PK（如 `pk=="a1 b1"` 或实现约定的复合 PK 编码）→ `rowToPk(row,"t_compound",roConn)` 回算的 pk 与输入一致（往返自洽，验证 `rowToPk` 复合 PK 拼接规范）。 |
| `rowToPk_singleVsCompound` | 直接以 `t_single` 行与 `t_compound` 行调 `rowToPk`（经 `resolvePk` 间接触达）→ 单列 PK 返回该列字符串值、复合 PK 返回按 PK 列序连接的规范键（与 `getPkColumn`/PRAGMA 列序一致）。 |
| `resolvePk_nonexistentPk_noRow` | `addRecord("t_single","999")`（不存在）→ `resolvePk` 返回 `true` 但该 record 不产出 `ResolveResult`（无行，留给闭包层判 `E_SYNC_FK_CLOSURE_MISSING`）；`out` 不含 pk=999 项。 |
| `resolveWhere_legalFilter_snapshotOnly` | （受限 DSL 合法分支）若实现接受单表单条件只读过滤：`resolveWhere(roConn,"t_single","v > 0", ...)` 在只读快照上 SELECT 出匹配行，`out` 正确；执行后**库内行数不变**（只读快照不写库）。注：当前 `SyncSelection::Builder::addWhere` 对非空 `whereExpr` 在 `build()` 即拒绝，故本 slot 直接调用 `resolveWhere` 私有路径（经 friend/测试桥）验证其只读语义。 |
| `resolveWhere_rejectsMultiStatement` | （受限 DSL 拒绝分支）`whereExpr` 含分号多语句 / 子查询 / 注释逃逸（如 `"1=1; DROP TABLE t_single"`、`"v IN (SELECT ...)"`）→ `resolveWhere` 拒绝并回 `false`、`*err` 含选择非法语义（映射 `E_SYNC_SELECTION_EMPTY`/拒绝码）；库内 `t_single` 仍存在（未被注入语句破坏，验证不写库 + 注入防护）。 |
| `resolvePk_readOnly_noSideEffect` | 任一成功 `resolvePk` 后，对比执行前后 `t_single`/`t_compound` 的 `count(*)` 与内容指纹完全一致——resolver 不在快照连接上产生任何写副作用。 |
| `resolvePk_emptySelection` | 空 `SyncSelection`（`isEmpty()==true`）→ `resolvePk` 返回 `true` 且 `out` 为空（或按实现回 `E_SYNC_SELECTION_EMPTY`），不崩溃。 |

**覆盖说明**：`resolveWhere` 受限 DSL 在 MVP 被 Builder 层前置拒绝，本套件经测试桥直达私有方法补齐其只读/拒绝分支，构成 selection 85% 的一部分；复合 PK 往返自洽是 `rowToPk` 的关键守护。

### 9.2 tst_fk_closure_builder（重点）

**被测**：`FkClosureBuilder::{build, buildClosure, topoSort, fetchRow, getPkColumn}`；`struct Entry{table, pk, row, isSelected, topoIndex}`（`src/sync/selection/FkClosureBuilder.{h,cpp}`，241 行）。
**关联需求/错误码**：拓扑闭包（C13）；**FK 环 → `E_SYNC_FK_CYCLE_UNSUPPORTED`**（Kahn 检测残留节点）；**悬挂父 → `E_SYNC_FK_CLOSURE_MISSING`**（引用的父行在快照中不存在）；**超 maxSize → `E_SYNC_SELECTION_TOO_LARGE`**；复用 `SchemaCatalog` FK 图 / Kahn 拓扑 / 一致性剪枝（H-02：`includeFkDeps`/`pruneConsistent` 遵循 `SyncSelection` 标志）。
**夹具**：`SqliteFixture` + `SchemaFixture`（真实 `PRAGMA foreign_key_list` 构 catalog）；真实 `ConsistencyCache(durable=false)`；建表含真实 `FOREIGN KEY` 约束（父表 `parent`、子表 `child(parent_id REFERENCES parent)`、孙表 `grandchild` 多级链、复合 PK 父表、自/互引用造环）。

| slot | 断言要点 |
| --- | --- |
| `init()` | 公共：建多级 FK 链 `parent ← child ← grandchild`、复合 PK 父表 `cparent(a,b)`、环表组 `ca↔cb`，插入基准父/子行；构建 `SchemaCatalog`。 |
| `build_selectedPlusParentClosure` | 直选一条 `child` 行（其 `parent_id` 指向存在的 `parent` 行），`includeFkDeps=true` → `out` 含 child（`isSelected==true`）**与** parent（`isSelected==false`，作为 FK 依赖被拉入）；child.row/parent.row 内容正确。 |
| `build_topoOrder_parentBeforeChild` | 上条结果中 `parent.topoIndex < child.topoIndex`（父在子前，G-05/C13 拓扑序），`out` 整体按 `topoIndex` 升序即为合法应用序。 |
| `build_multiLevelChain` | 直选一条 `grandchild` → 闭包递归拉入其 `child` 与 `child` 的 `parent`（三级），`topoIndex` 满足 `parent < child < grandchild`；`buildClosure` 递归正确、无重复 Entry（`seen` 去重）。 |
| `build_compoundPkParent` | 直选引用复合 PK 父 `cparent(a,b)` 的子行 → 闭包以复合 PK 正确 `fetchRow` 出父行（`getPkColumn` 多列、pk 拼接与 resolver 一致），父被标 `isSelected==false`。 |
| `build_fkCycle_E_SYNC_FK_CYCLE_UNSUPPORTED` | 构造 `ca ↔ cb` 互引用环（或单表自引用环）并直选环内一行 → `topoSort` Kahn 入度归零后仍有残留节点 → `build` 返回 `false`，`ASSERT_ERR_CODE(err, "E_SYNC_FK_CYCLE_UNSUPPORTED")`。 |
| `build_danglingParent_E_SYNC_FK_CLOSURE_MISSING` | 子行 `child.parent_id` 指向**快照中不存在**的 parent（先删父、或插入孤儿子行且 FK 未即时校验）→ `fetchRow` 父行得空 → `build` 返回 `false`，`ASSERT_ERR_CODE(err, "E_SYNC_FK_CLOSURE_MISSING")`。 |
| `build_exceedsMaxSize_E_SYNC_SELECTION_TOO_LARGE` | 直选 + 闭包后 Entry 数 > `maxSize`（设 `maxSize=2`，选择展开出 ≥3 行）→ `build` 返回 `false`，`ASSERT_ERR_CODE(err, "E_SYNC_SELECTION_TOO_LARGE")`，`out` 不被部分填充（或填充后由调用方丢弃，按实现断言）。 |
| `build_includeFkDepsFalse_noParents` | 同 `build_selectedPlusParentClosure` 输入但 `includeFkDeps=false` → `out` 仅含直选 child（`isSelected==true`），**不**拉入 parent（H-02：尊重标志）；`out.size()==1`。 |
| `build_pruneConsistent_dropsConsistentParent` | 先 `cache.stampFromAuthoritative(db,"parent",parentPk,fp)` 使父行与中心一致，且本地父行指纹 == 该 fp → `build(..., pruneConsistent=true)` 查 `ConsistencyCache::isConsistent` 命中 → parent 被剪掉（不出现在 `out`），仅保留 child；验证「只搬不一致依赖」。 |
| `build_pruneConsistent_keepsInconsistentParent` | 父行已 stamp 但本地父行被改动致指纹 != 中心 fp（`isConsistent` 未命中）→ parent **不被**剪枝、仍入 `out`（剪枝仅作用于确证一致者）。 |
| `build_pruneConsistentFalse_keepsAll` | `pruneConsistent=false` 时即使 `isConsistent` 命中也不剪枝，所有 FK 父均保留（标志优先于缓存）。 |
| `build_selectedRowAlsoConsistent_notPruned` | 直选行本身在 cache 中标记一致 → **直选行不剪**（剪枝仅针对 FK 依赖 `isSelected==false`，直选恒搬运）；断言直选行始终在 `out`。 |
| `getPkColumn_cachedAndCorrect` | `getPkColumn` 对单列/复合 PK 返回正确列名（复合返回按 PRAGMA 序的列），二次调用命中 `pkColCache_`（行为等价、可经计数桩或仅断言结果一致）。 |

**覆盖说明**：本套件是 selection 子系统覆盖主力——三大错误码（CYCLE/MISSING/TOO_LARGE）各有专用触发场景，`includeFkDeps`/`pruneConsistent` 四象限全覆盖，多级链与复合 PK 验证 `buildClosure` 递归与 `fetchRow`/`getPkColumn`，是支撑 selection ≈85% 的核心。

### 9.3 tst_consistency_cache

**被测**：`ConsistencyCache::{init, isConsistent, stampFromAuthoritative, invalidateTable}`（`src/sync/selection/ConsistencyCache.{h,cpp}`）。DDL `__sync_consistency_cache(table_name, pk, center_fp, ...)`（durable 落库）。
**关联需求/错误码**：C10——一致性缓存**仅由下行/基线喂养**（`stampFromAuthoritative`，来自权威中心确认），上行剪枝消费它；C17——本地迁移即作废（`invalidateTable` 由 inbound apply / baseline apply 触发）。
**夹具**：`SqliteFixture` 临时库（durable 用例先 `SyncDDL` 建表）；内存态用例 `durable=false`。

| slot | 断言要点 |
| --- | --- |
| `isConsistent_missBeforeStamp` | 新建 cache（无任何 stamp）→ `isConsistent("t","1",fpA)==false`（未命中，未喂养前一律视为不一致，保守搬运）。 |
| `stampThenConsistent_hit` | `stampFromAuthoritative(db,"t","1",fpA)` 后 `isConsistent("t","1",fpA)==true`（本地指纹 == 权威指纹）。 |
| `stamp_differentLocalFp_miss` | stamp 为 `fpA`，但查询用本地指纹 `fpB(!=fpA)` → `isConsistent("t","1",fpB)==false`（指纹不等即不一致，触发上行搬运）。 |
| `onlyAuthoritativeFeeds` | （C10 语义）cache 仅有 `stampFromAuthoritative` 一条喂养入口——无任何「本地写入即标一致」的 API；断言类无写喂养路径（结构性约束，以「仅 stamp 后才 hit」反证）。 |
| `invalidateTable_dropsEntries` | stamp `t` 的多个 pk 后 `invalidateTable(db,"t")` → 该表所有 pk 的 `isConsistent` 均回 `false`（C17：本地迁移作废整表）；其它表条目不受影响。 |
| `durableTrue_persistAndReload` | `init(db, durable=true)`，stamp 若干 → 新建另一 `ConsistencyCache` 实例对**同一库** `init(db,true)` → `loadFromDb` 重载，`isConsistent` 仍命中（落 `__sync_consistency_cache` 表并重载）。 |
| `durableFalse_memoryOnly` | `init(db, durable=false)`，stamp 后查询命中；但新实例对同库 `init(db,false)` 后**不命中**（仅内存、未落库，`durable=false` 不读不写表）。 |
| `durableFalse_noTableWrite` | `durable=false` 下 stamp/invalidate 后断言 `__sync_consistency_cache` 表无新增行（纯内存，不触库）。 |
| `invalidate_durablePersists` | `durable=true` 时 `invalidateTable` 同步 `DELETE FROM __sync_consistency_cache WHERE table_name=?`，重载后该表确实空（落库作废）。 |
| `usedForPruning_integrationWithBuilder` | 与 §9.2 联动的轻量断言：stamp 一致父行后供 `FkClosureBuilder` 剪枝（剪枝用途冒烟），确认 cache 作为剪枝输入语义正确（详尽剪枝矩阵在 §9.2）。 |

**覆盖说明**：四个公共方法 + durable/内存两态 + 单表作废全分支可达；C10「仅权威喂养」与 C17「迁移即作废」是两条关键语义守护。

### 9.4 tst_frozen_manifest

**被测**：`FrozenManifest::{init, save, load, remove}`（`src/sync/selection/FrozenManifest.{h,cpp}`）。DDL `__sync_frozen_manifest(push_id, chunk_seq, table_name, pk_hash, primary_key, record_kind, topo_index, fingerprint, PRIMARY KEY(push_id,chunk_seq,table_name,pk_hash))`（由 `SyncDDL` 建，`init` 仅 no-op 返回 true）。
**关联需求/错误码**：C16——冻结清单在释放读快照前持久化（护 WAL，供 `ChunkStreamer` 续传的权威来源）；`save` 用 `INSERT OR REPLACE`（重复 (push_id,chunk_seq,table,pk_hash) 覆盖、幂等）。
**夹具**：`SqliteFixture`（先 `SyncDDL::allCreateStatements()` 建 `__sync_push_progress` 父表 + `__sync_frozen_manifest`，满足外键）；先插一行 `__sync_push_progress(push_id=...)` 以满足 FK 约束。

| slot | 断言要点 |
| --- | --- |
| `init()` | 公共：建库 + `SyncDDL` 全表；插一条 `__sync_push_progress` 占位（push_id="P1"，满足 FK）。`FrozenManifest::init` 返回 `true`（仅探测，无副作用）。 |
| `save_load_roundTrip` | `save(db,"P1",0, {e1,e2}, &err)` 成功后 `load(db,"P1",0)` 返回 2 条 `FrozenEntry`，`table/pkHash/primaryKey/recordKind/topoIndex/fingerprint` 字段逐项与写入相等。 |
| `load_orderedByTopoIndex` | 以乱序 `topoIndex`（如 2,0,1）写入同 chunk → `load` 结果按 `ORDER BY topo_index` 升序返回（父在子前，供续传按拓扑序重放）。 |
| `save_multipleChunks_isolated` | 同 push_id 写 `chunk_seq=0` 与 `chunk_seq=1` 不同 entries → `load(...,0)` 与 `load(...,1)` 各自返回对应片，互不串台（按 (push_id,chunk_seq) 隔离）。 |
| `save_multiRowsPerChunk` | 单 chunk 写入多 entry（不同 table/pk_hash）→ 全部落库，`load` 全数返回；验证 PK (push_id,chunk_seq,table_name,pk_hash) 支持「每片多行」。 |
| `save_duplicatePk_insertOrReplace` | 对同一 (push_id,chunk_seq,table,pk_hash) 二次 `save`（fingerprint/topoIndex 不同）→ `INSERT OR REPLACE` 覆盖，`load` 仅一条且为最新值（幂等 + 覆盖语义，续传重写安全）。 |
| `remove_deletesAllChunks` | 写入 P1 的多 chunk 后 `remove(db,"P1",&err)` → `load(db,"P1",0)`/`load(db,"P1",1)` 均为空（`DELETE ... WHERE push_id=?` 删整 push 全部片，清理完成的 push）。 |
| `remove_otherPushUnaffected` | 同时存在 P1、P2 清单 → `remove(P1)` 后 P2 仍可 `load`（按 push_id 隔离删除）。 |
| `save_execFailure_returnsErrAndCode` | 注入写失败（如临时令表只读 / 违反 FK：用未登记的 push_id 触发 `FOREIGN KEY` 失败）→ `save` 返回 `false` 且回填 `*err`（`q.lastError().text()`），半批不致悬挂（调用方应在事务内回滚，见 §集成）。 |
| `load_missingPush_emptyList` | `load(db,"NOPE",0)` → 返回空列表、不报错（`exec` 成功但无行）。 |

**覆盖说明**：`save/load/remove` 三方法 + 多 chunk/多行/覆盖/隔离/失败全分支可达；C16「释放快照前冻结」的持久化契约由往返与续传重写守护。

### 9.5 tst_chunk_streamer

**被测**：`ChunkStreamer::stream(manifest, origin, targetPeer, chunkBudgetBytes, codec, &chunks, &err)`；`struct Chunk{pushId, chunkSeq, totalChunks, entries, rows}`（`src/sync/selection/ChunkStreamer.{h,cpp}`）。
**关联需求/错误码**：拓扑序分片（父片不晚于子片，C13）；按 `chunkBudgetBytes` 切分；`pushId` 空则生成 UUID（全局唯一，含 origin/epoch 语义由上层注入）；单行超预算 → `E_SYNC_SELECTION_TOO_LARGE`。
**夹具**：真实 `PayloadCodec`（用其 `encode*` 计量字节）；输入 `manifest` 为 §9.2 `FkClosureBuilder::build` 产出的真实 `Entry` 列表（已按 `topoIndex` 排序）；`QUuid` 生成 pushId。

| slot | 断言要点 |
| --- | --- |
| `init()` | 公共：构造一组真实 manifest（含 parent/child/grandchild，`topoIndex` 0/1/2，已排序）。 |
| `stream_singleChunkUnderBudget` | 大 `chunkBudgetBytes` 使全部行落单片 → `chunks.size()==1`，`chunk.totalChunks==1`、`chunkSeq==0`，`entries`/`rows` 一一对应且与 manifest 等长。 |
| `stream_topoOrderParentNotLaterThanChild` | 多片场景下，对任一 (table,pk) 父行所在 `chunkSeq` ≤ 其子行所在 `chunkSeq`（父片不晚于子片，C13）；同片内 `entries` 按 `topoIndex` 升序。 |
| `stream_budgetSplitsMultipleChunks` | 设小 `chunkBudgetBytes`（仅容 1~2 行编码字节）→ manifest 被切成多片，`totalChunks==chunks.size()`，`chunkSeq` 连续 `0..N-1`，各片累计字节 ≤ budget（除单行本身超界外）。 |
| `stream_entriesRowsAligned` | 每个 chunk 内 `entries[i]` 与 `rows[i]` 严格对齐（同 table/pk），数量一致；切片不打乱 entry↔row 配对。 |
| `stream_pushIdGeneratedWhenEmpty` | `origin` 给定、pushId 由 streamer 生成（无大括号 UUID）→ 所有 chunk 的 `pushId` 相同且非空；两次独立 `stream` 调用产出不同 pushId（全局唯一）。 |
| `stream_pushIdStableAcrossChunks` | 单次 `stream` 产出的多片共享同一 `pushId`，幂等键 (pushId,chunkSeq) 在片间唯一（chunkSeq 不重复）——为续传/去重提供稳定键。 |
| `stream_emptySelection_noChunks` | 空 manifest → `chunks` 为空、返回 `true`（无可搬运，空选择不报错；上游 `E_SYNC_SELECTION_EMPTY` 已在 Builder 层处理）。 |
| `stream_singleRowExceedsBudget_E_SYNC_SELECTION_TOO_LARGE` | 某单行编码字节 > `chunkBudgetBytes`（设极小 budget 或超大 BLOB 行）→ `stream` 返回 `false`，`ASSERT_ERR_CODE(err, "E_SYNC_SELECTION_TOO_LARGE")`（单行无法装入任何片，明确报错而非死循环切片）。 |
| `stream_chunkSeqContiguousNoGap` | 切多片后 `chunkSeq` 严格连续无空洞（G-05），`totalChunks` 等于实际片数，供续传按序消费。 |

**跨端幂等说明（重要）**：`ChunkStreamer` 本身是**纯分片器**，**不**承担「重复 chunk 同 checksum no-op / 异 checksum → `E_SYNC_PAYLOAD_CORRUPT`」的判定——该幂等续传逻辑落在**消费端** `SyncWorker::processSelectionPushArtifact`，依据 `__sync_push_chunk_progress(push_id,chunk_seq,checksum,status)` 表（PK `(push_id,chunk_seq)`）实现：同键再到达若 `checksum` 相同则 no-op 跳过、不同则 `E_SYNC_PAYLOAD_CORRUPT`。本单元套件仅验证 streamer 产出的 (pushId,chunkSeq) 键稳定唯一、为该幂等提供正确输入；以下两条标注为 `[CONSUMER]`，归入 §（SyncWorker/集成）覆盖，本节列出以保链路完整：

| slot `[CONSUMER]` | 断言要点（落 SyncWorker，本节仅占位） |
| --- | --- |
| `chunk_duplicateSameChecksum_noop` | 同 (push_id,chunk_seq) 重投、`checksum` 相同 → 消费端查 `__sync_push_chunk_progress` 命中且 checksum 一致 → no-op 跳过、不重复应用（幂等续传，G-08）。 |
| `chunk_duplicateDiffChecksum_E_SYNC_PAYLOAD_CORRUPT` | 同 (push_id,chunk_seq) 但 `checksum` 不同 → 消费端判篡改/损坏 → `ASSERT_ERR_CODE(err, "E_SYNC_PAYLOAD_CORRUPT")`，拒绝应用并隔离。 |

**覆盖说明**：streamer 自身的拓扑序/预算/连续 chunkSeq/UUID 生成/超规模五类分支单元可达；跨端 checksum 幂等因依赖消费端持久表，作 `[CONSUMER]` 移交集成，是 selection 单元层定 85%（而非更高）的主因。

### 9.6 tst_baseline_manager

**被测**：`BaselineManager::{exportBaseline, applyBaseline, shouldFallbackToBaseline, serializeTables, deserializeAndApply}`；`struct BaselineArtifact{data, sourceMaxSeq, originCuts(QVector<BaselineOriginCut>含 streamEpoch)}`（`src/sync/baseline/BaselineManager.{h,cpp}`，536 行）。
**关联需求/错误码**：C-03（per-origin `originCuts` 含 `streamEpoch`，应用时 `av.resetTo` 用各源自身 epoch）；M-01/M-02（self origin cut 合并、row_winner 以 `baselineRank`/`sourceMaxSeq` 播种防低 rank 覆盖）；H-05（`schemaFp` 写入 table_state 避免假 Diff）；I-20——导出或应用失败统一前缀 **`E_SYNC_BASELINE_FAILED`**，应用失败 `txn.rollback()` **不触动旧锚点**（`*newAnchorSeq` 仅在成功末尾赋 `art.sourceMaxSeq`）；C16/C1（`PRAGMA foreign_keys=OFF` 须在 `BEGIN` 之前、commit 后恢复）。
**夹具**：两套 `SqliteFixture`（导出库 `src`、应用库 `dst`），`src` 建业务表 + `SyncDDL` 全表并写入若干行 + `__sync_applied_vector` 多 origin 游标；真实 `AppliedVectorStore`/`TableStateStore`/`RowWinnerStore`/`ConsistencyCache`；`sqlite3* h` 取自 `dst` 连接 handle。

| slot | 断言要点 |
| --- | --- |
| `init()` | 公共：建 `src`/`dst` 两库，`src` 插 `parent`/`child` 行与多 origin 的 `__sync_applied_vector`（如 originA epoch=1 seq=10、originB epoch=2 seq=7）。 |
| `exportBaseline_serializesData` | `exportBaseline(srcRo, {"parent","child"}, &art, &err, localOrigin="self", localEpoch=3, localOriginSeq=5)` → `true`，`art.data` 非空（`qCompress` 后），`art.sourceMaxSeq == src 最大 local_seq`。 |
| `exportBaseline_originCutsWithEpoch` | `art.originCuts` 含 originA/originB 各自的 `{origin, streamEpoch, appliedSeq}`（epoch 来自 `__sync_applied_vector`，非本地 epoch，C-03）；**且** self origin cut（"self", epoch=3, seq=5）被合并进 `originCuts`（M-01：导出节点自身 cut 不在 applied_vector 中，须由参数补入）。 |
| `exportBaseline_readOnly_noWrite` | `exportBaseline` 在只读 `src` 连接执行后，`src` 业务表与同步表行数/内容快照不变（只读不写库）。 |
| `applyBaseline_resetsStoresAndAnchor` | 对 `dst` 应用上面的 `art`：返回 `true`，`*newAnchorSeq == art.sourceMaxSeq`；应用后 `dst.__sync_applied_vector` 每个 (origin) 按其 cut 的 epoch/appliedSeq 被 `resetTo`（originA epoch=1 seq=10、originB epoch=2 seq=7、self epoch=3 seq=5），`__sync_table_state` 经 `resetFromBaseline` 写入传入 `schemaFp`（H-05），`__sync_row_winner` 先 `resetAll` 清空。 |
| `applyBaseline_seedsRowWinners` | 应用后 `__sync_row_winner` 为每条导入行播种一条 winner：`winning_origin==origin`、`winning_rank==baselineRank`、`winning_origin_seq==art.sourceMaxSeq`，`pk_hash` 用 `RowWinnerStore::pkHash`、`winning_content` 为 JSON 行（M-02：防后续低 rank 覆盖基线真值）。 |
| `applyBaseline_feedsConsistencyCache` | 应用成功后对每个 table 调 `cache.invalidateTable`（C17：基线即「本地迁移」，作废旧一致性条目）；断言应用前 stamp 的条目应用后 `isConsistent` 回 `false`（**注**：当前实现以 invalidate 而非 stamp「喂养」cache——基线翻新后旧一致性记录一律作废，下行后续 stamp 重新喂养，本 slot 按实现断言 invalidate 语义）。 |
| `applyBaseline_roundTrip_twoSidesEqual` | export(`src`) → apply(`dst`) 往返后，`dst` 的 `parent`/`child` 业务表行数与内容与 `src` 完全一致（`SELECT *` 排序后逐行比对），验证序列化/反序列化无损 + UPSERT 正确（端到端基线一致性）。 |
| `applyBaseline_emptyBaseline_primaryReset` | `art.originCuts` 为空（空基线）→ apply 时主 origin 以 `(origin, epoch, seq=0)` 兜底 `resetTo`（`primaryReset` 分支），不崩溃，`*newAnchorSeq==art.sourceMaxSeq`。 |
| `shouldFallbackToBaseline_truthTable` | `shouldFallbackToBaseline(appliedSeq, sourceMinSeq)`：`appliedSeq < sourceMinSeq`（缺口/源已压实掉所需 changeset）→ `true`；`appliedSeq >= sourceMinSeq` → `false`；边界 `appliedSeq==sourceMinSeq` → `false`。data-driven 覆盖「缺锚点(appliedSeq=0)/有缺口/无缺口」三态真值。 |
| `applyBaseline_deserializeFails_E_SYNC_BASELINE_FAILED_anchorUntouched` | 注入损坏 `art.data`（截断/乱码）→ `deserializeAndApply` 失败 → `txn.rollback()`，`applyBaseline` 返回 `false`，`ASSERT_ERR_CODE(err, "E_SYNC_BASELINE_FAILED")`；`*newAnchorSeq` **未被赋值**（保持调用方旧锚点），`dst` 业务表/同步表回滚到应用前状态（全有或全无，I-20）。 |
| `applyBaseline_resetStoreFails_rollback` | 注入某 store 重置失败（如令 `__sync_row_winner` 或 `__sync_table_state` 临时不可写）→ 任一 `av.resetTo`/`ts.resetFromBaseline`/`rw.resetAll`/`rw.put` 失败即 `rollback()`+`restoreFk()`+`wrapErr` → 返回 `false`、`E_SYNC_BASELINE_FAILED`，旧锚点与旧数据不变（逐条失败分支守护）。 |
| `applyBaseline_fkRestoredAfterFailure` | 失败路径（上两条）后断言 `dst` 连接 `PRAGMA foreign_keys` 已恢复为 `ON`（`restoreFk` 在每条错误路径调用，C-4：FK=OFF 不泄漏到连接状态）。 |
| `exportBaseline_serializeFails_E_SYNC_BASELINE_FAILED` | 导出时 `serializeTables` 失败（如表不存在 / 读失败）→ `exportBaseline` 返回 `false`，`*err` 前缀 `E_SYNC_BASELINE_FAILED`（I-20），`out` 不被部分填充。 |
| `applyBaseline_fkOffBeforeBegin` | 验证 `PRAGMA foreign_keys=OFF` 在 `txn.begin` 之前执行（C-1：事务内切 pragma 被静默忽略）——以「含违反 FK 顺序的基线数据（子先于父导入）仍能成功 apply」反证 FK 已禁用；commit 后 `foreign_keys==ON`。 |

**覆盖说明**：导出（序列化 + originCuts 含 epoch + self cut 合并 + 只读）、应用（重置三大 store + 播种 row_winner + cache 作废 + 锚点）、回退判定真值表、以及**失败回滚不动旧锚点 + FK 恢复**三类错误分支均有专用用例；逐条 store 失败注入与 row_winner seed 类型分支是 baseline ≈82% 的主要构成，PRAGMA/IO 失败的少数分支作「尽力覆盖」缺口。

### 9.7 本节套件与覆盖目标汇总

| 套件 | 被测组件 | 子系统 | 关键需求/错误码 | 用例数 |
| --- | --- | --- | --- | --- |
| `tst_selection_resolver` | `SelectionResolver` | selection | MVP PK-set、受限 DSL 拒绝、只读快照 | 11 |
| `tst_fk_closure_builder` | `FkClosureBuilder` | selection | 拓扑闭包(C13)、`E_SYNC_FK_CYCLE_UNSUPPORTED`/`E_SYNC_FK_CLOSURE_MISSING`/`E_SYNC_SELECTION_TOO_LARGE`、剪枝(H-02) | 14 |
| `tst_consistency_cache` | `ConsistencyCache` | selection | 仅权威喂养(C10)、迁移即作废(C17)、durable/内存 | 10 |
| `tst_frozen_manifest` | `FrozenManifest` | selection | 释放快照前冻结(C16)、(push_id,chunk_seq,table,pk_hash) 每片多行 | 10 |
| `tst_chunk_streamer` | `ChunkStreamer` | selection | 拓扑序分片(C13)、预算切分、(pushId,chunkSeq) 幂等键、`E_SYNC_SELECTION_TOO_LARGE`；checksum 幂等`[CONSUMER]` | 10 (+2 `[CONSUMER]`) |
| `tst_baseline_manager` | `BaselineManager` | baseline | originCuts 含 epoch(C-03)、重置三 store + 播种(M-02)、回退真值、`E_SYNC_BASELINE_FAILED` 回滚不动锚点(I-20) | 15 |

**子系统覆盖目标**：selection ≈ 85%（`FkClosureBuilder` 三错误码与剪枝四象限单元可达，但 `ChunkStreamer` 的跨端 checksum 幂等（no-op vs `E_SYNC_PAYLOAD_CORRUPT`）依赖消费端 `__sync_push_chunk_progress`，作 `[CONSUMER]` 移交集成，构成单元层缺口）；baseline ≈ 82%（导出/应用/回退主路径 + 失败回滚 + FK 恢复全覆盖，逐条 store 失败注入与 PRAGMA/IO 少数分支作「尽力覆盖」缺口）。所有受测组件的写库副作用以「执行前后行数/内容快照比对」断言只读性，所有 `E_SYNC_*` 失败以 `ASSERT_ERR_CODE` 码级断言（满足 O3）。
## 10. diff/peer/core 与 ETL 回归补强单元测试

本节覆盖比对子系统（`src/sync/diff/**`：`DiffEngine`、`StagingBuffer`、`InboundTableGate`、`ComparisonSession`，对应 plan T4.1/T4.3）、peer 健康治理（`src/sync/peer/DeadPeerEvictor`）、同步核心基础设施（`src/sync/`：`SyncContext`/`SyncContextRegistry`、`ForegroundGate`、`WriteTxn`）、不可变配置/选择值对象（`include/dbridge/sync/`：`SyncConfig::Builder`、`SyncSelection::Builder`），以及 ETL 三件套的回归补强（`ImportService` 提取守护、`ExportService`、`BatchTransfer`）。

这些组件的核心不变量分别是：**比对零全量**（DiffEngine 表级判定只消费 M1 维护的 `TableStateStore`，不扫基表）、**判等三元组**（`schema_fingerprint + row_count + content_checksum` 三项全等才 Identical，`high_water_seq` 不参与判等，R-08/G-06）、**会话脚下变动即作废**（钉 `data_version`，落库前再校验，变动则 `E_SYNC_STAGE_STALE`）、**单库单上下文**（G-07：OS 文件标识 dev+inode 去重，refCount 末位释放才析构）、**前台串行**（`ForegroundGate` 重入即 `E_BUSY`）、**写经 `BEGIN IMMEDIATE`**（`WriteTxn`），以及 **`UpsertExecutor` 提取前后 import 行为不变**（先红后绿守护，Q-03/T2.0a）。

全部套件复用第 2.6 节隔离骨架（独立临时库 + 独立 `connName`）与第 3 节夹具：`SqliteFixture`（§3.1）、`tests/data/sync/` 数据集（§3.2）、`FakeClock`（§3.6，用于 `DeadPeerEvictor` 注入 `nowMs`）、`ASSERT_ERR_CODE`（§3.7）。`DiffEngine`/`ComparisonSession`/`StagingBuffer` 因涉及真实 SQLite 读写与 session 捕获，用临时文件库（WAL）；`InboundTableGate`/`ForegroundGate`/`SyncConfig`/`SyncSelection` 为纯内存判定，无须库。

### 10.1 套件总览与覆盖目标

| 套件 | 被测目标 | 关联需求/不变量 | 关联错误码 | 目标行覆盖率 |
| --- | --- | --- | --- | --- |
| `tst_diff_engine` | `DiffEngine::{tableDiffs,rowDiffs,compareRows}` | 零全量、判等三元组（R-08/G-06）、keyset 分页（T4.1） | —（纯判定，不抛码） | ≈ 82% |
| `tst_staging_buffer` | `StagingBuffer::{stage,unstage,isEmpty,toMutations,save,discard,getRow}` | save 经 `UpsertExecutor`、discard 零落盘（T4.3） | `E_DB_UPSERT`（透传） | ≥ 90% |
| `tst_inbound_table_gate` | `InboundTableGate::{open,isOpen,shouldDefer,releaseAll}` | 会话期被比对表整发暂停（E-12/T4.1） | — | ≥ 95% |
| `tst_comparison_session`（重点） | `ComparisonSession`/`OwningComparisonSession` 全 API | 会话整链、`E_SYNC_STAGE_STALE`、save 普通本地写、discard 零落盘 | `E_SYNC_STAGE_STALE`、`E_SYNC_INIT` | ≥ 85% |
| `tst_dead_peer_evictor` | `DeadPeerEvictor::{configure,evaluate,evict}` | 三维阈值软告警→硬逐出、outbox 坍缩 | —（路由判定，不抛码） | ≥ 92% |
| `tst_sync_context`（G-07 重点） | `SyncContextRegistry::{canonicalKey,getOrCreate,getExisting,release,ensureContextUuid}` | OS 文件标识去重（G-07）、refCount、UUID 二次确认 | — | ≥ 90% |
| `tst_foreground_gate` | `ForegroundGate::{tryAcquire,release,isHeld}` | 前台串行重入互斥 | `E_BUSY` | ≥ 95% |
| `tst_write_txn` | `WriteTxn::{begin,commit,rollback,isActive}` | `BEGIN IMMEDIATE` 写事务 RAII | — | ≥ 95% |
| `tst_sync_config_builder` | `SyncConfig::Builder::build` + getter | 完整性校验产出不可变值对象、`originRank` | —（build 回填 err 文案） | ≥ 95% |
| `tst_sync_selection_builder` | `SyncSelection::Builder` + `isSimpleIdent` | 受限 DSL（§4.4）、拒原始 SQL/非法标识 | `E_SYNC_SELECTION_EMPTY` | ≥ 95% |
| `tst_import_service_regression` | `ImportService::run` 提取守护 | `UpsertExecutor` 提取前后行为不变（T2.0a，先红后绿） | `E_DB_UPSERT` | 守护现有覆盖 |
| `tst_export_service` / `tst_batch_transfer` | `ExportService::run`、`BatchTransfer`（`IBatchTransfer`） | 非阻塞受理/轮询/stop/`E_BUSY` 互斥、导入维护 table_state | `E_BUSY`、`E_OPEN_DB` | ≥ 80% |

> 覆盖率说明：`DiffEngine` 定 ≈82%（`tableDiffs` 的 `rowDiff` 估算分支与 `fetchLocalRows` 的 `q.exec()` 失败分支难稳定注入，列为缺口）；`BatchTransfer`（408 行）原无测试，本节自 80% 起步补强。

### 10.2 `tst_diff_engine`（判等三元组 + 零全量重点）

**被测**：`DiffEngine::{tableDiffs,rowDiffs}`（公共），间接覆盖私有 `fetchLocalRows/getPkColumn/compareRows`（`src/sync/diff/DiffEngine.{h,cpp}`，224 行）。
**关联需求/不变量**：T4.1 表级零全量（只读 `__sync_table_state` 单行，不扫基表）；R-08/G-06 判等三元组（`schemaFp + rowCount + checksum`，`high_water` 不判等）；行级只物化受影响行 + keyset 分页。
**夹具**：`SqliteFixture`（临时文件库）建 1~2 张含显式 PK 的用户表 + `__sync_table_state`；先经 `TableStateStore::applyMutations`（§7.3）写入本地状态行（带正确 `streamEpoch`），再用手构的 `QHash<QString,RemoteMeta>` 充当远端元数据；`rowDiffs` 的 `remoteRows` 用 `QList<QVariantMap>` 直接构造。本套件无错误码（纯状态/判定正确性断言）。

| slot | 断言要点 |
| --- | --- |
| `tableDiffs_identical_allThreeMatch` | 本地 state 与远端 `RemoteMeta` 的 `checksum`/`schemaFp`/`rowCount` 三项全等 → `status==Identical`；`addedRows/deletedRows/modifiedRows==0`。 |
| `tableDiffs_different_checksumDiffers` | **判等三元组①**：`schemaFp`、`rowCount` 相同但 `checksum` 不同 → `status==Different`；行数相等故 `modifiedRows==localRowCount`、`added/deleted==0`（全为修改）。 |
| `tableDiffs_identical_highWaterDiffersContentSame` | **判等三元组②（R-08 核心）**：构造两库内容相同（三元组全等）但 `high_water_seq` 不同 → `status==Identical`（`high_water` 不参与判等，绿）。 |
| `tableDiffs_different_rowCountDiffers` | 远端 `rowCount > localRowCount` 且 checksum 异 → `Different`，`addedRows==rm.rowCount-localRowCount`、`modifiedRows==localRowCount/4`（估算分支）；反向（远端更少）→ `deletedRows` 置位。 |
| `tableDiffs_onlyLocal` | 本地 `readState` 命中（`localFound`）、远端 `remote` 不含该表 → `status==OnlyLocal`。 |
| `tableDiffs_onlyRemote_seedsAddedRows` | 远端含该表、本地从未同步（`localFound==false`）→ `status==OnlyRemote`，`addedRows==remote.rowCount`。 |
| `tableDiffs_bothAbsent_identical` | 远端、本地皆无记录（`!remoteFound && !localFound`）→ `status==Identical`（双空视为一致）。 |
| `tableDiffs_noFullScan_boundedReads` | **零全量**：对一张有 N 行的基表执行 `tableDiffs`，用可计数 query 钩子断言读取行数有界（只命中 `__sync_table_state` 单行 readState），与基表行数 N 无关、不出现 `SELECT * FROM 基表`。 |
| `rowDiffs_added` | `remoteRows` 含本地缺失的 PK → 该行 `kind==Added`，`cells` 来自 `compareRows({}, remote)`（localValue 全空）。 |
| `rowDiffs_deleted` | 本地含远端缺失的 PK → `kind==Deleted`，`cells` 来自 `compareRows(local, {})`（remoteValue 全空）。 |
| `rowDiffs_modified` | 同 PK 两侧某列不同 → `kind==Modified`，对应 `CellDiff.changed==true`、其余列 `changed==false`。 |
| `rowDiffs_same` | 同 PK 两侧逐列相等 → `kind==Same`（`anyChanged==false`），仍返回该行但标 Same（供 `stageTable` 跳过）。 |
| `compareRows_cellLevel_unionColumns` | 局部缺列时取列名并集；`CellDiff.localValue/remoteValue` 用 `value(col)`（缺列得空 QVariant），`changed=(local!=remote)` 逐格判定。 |
| `rowDiffs_keysetPaging_stableWindow` | **分页**：`offset/limit` 切窗，`fetchLocalRows` 走 `ORDER BY pk`（H-01）→ 同一 limit 下连续两页 PK 不重叠/不跳漏；远端切片用同窗 `mid(offset, limit)`。 |
| `rowDiffs_emptyPk_noCrash` | 表无显式 PK（`getPkColumn` 返回空）→ 不崩溃，返回空 diff（key 为空被过滤），覆盖 `pkCol.isEmpty()` 分支。 |

**目标覆盖率**：`DiffEngine.cpp` ≈ **82%**（五类 `TableDiffStatus` + 三元组判等 + 四类 `RowDiffKind` + 分页/空 PK 分支可达；`fetchLocalRows` 的 `q.exec()` 失败与 `rowDiff` 估算的全部子支属缺口）。

### 10.3 `tst_staging_buffer`

**被测**：`StagingBuffer::{stage,unstage,getRow,isEmpty,toMutations,save,discard}`（`src/sync/diff/StagingBuffer.{h,cpp}`，103 行）。
**关联需求/不变量**：T4.3 内存暂存；`save` 经 `WriteTxn(BEGIN IMMEDIATE)` + `UpsertExecutor::apply`（普通 origin 本地写，`UpsertMode::DoUpdate`）落库；`discard` 零落盘。
**夹具**：`SqliteFixture`（临时文件库）建一张含 PK 的目标表；真实 `UpsertExecutor`；`save` 直接传 `fx.db()` 作 `wconn`。失败注入用「目标表缺列/类型冲突」触发 `UpsertExecutor` 透传错误。

| slot | 断言要点 |
| --- | --- |
| `stage_thenGetRow` | `stage("t","1",row)` 后 `getRow("t","1")==row`；`isEmpty()==false`。 |
| `stage_sameKeyOverwrites` | 对同 `(table,pk)` 二次 `stage` 不新增条目、覆盖 `row`（断言 `getRow` 取最新，且 `toMutations` 只产 1 条）。 |
| `unstage_removesEntry` | `stage` 后 `unstage("t","1")` → `getRow` 返回空 map；若为唯一条目则 `isEmpty()==true`。 |
| `isEmpty_initialTrue` | 新建 buffer `isEmpty()==true`；任意 stage 后转 false。 |
| `toMutations_perTablePkCols` | 多表暂存 → `toMutations(pkColsPerTable, fallback)` 每条 `RowMutation` 的 `pkColumns` 取该表映射值；表不在映射中时用 `pkColsFallback`；`mode==DoUpdate`，`columns/values` 与 `row.keys()/values()` 对应。 |
| `save_emptyBuffer_trueNoop` | 空 buffer `save(...)` 返回 true，不开事务、库不变（`staged_.isEmpty()` 短路）。 |
| `save_viaUpsertExecutor_persists` | stage 2 行后 `save(wconn,upsert,pkCols,&err)==true` → 目标表可查到这 2 行（值正确）；验证经 `BEGIN IMMEDIATE`→`apply`→`COMMIT` 链路真正落库。 |
| `save_upsertFails_rollback_E_DB_UPSERT` | 注入 `UpsertExecutor::apply` 失败（如 stage 的列与表 schema 不符）→ `save` 返回 false、`*err` 反映底层 `E_DB_UPSERT`；**事务回滚，目标表无残留写入**（断言行数不变）。 |
| `discard_zeroPersist` | stage 若干行后直接 `discard()` → `isEmpty()==true`，且**目标表行数与 stage 前完全一致**（discard 零落盘，未触库）。 |
| `save_doesNotHoldTxnAcrossCalls` | `save` 内部自持 `WriteTxn` 并在返回前 commit/rollback（`isActive` 不外泄）——连续两次 `save` 各自独立事务，第二次成功不受第一次影响。 |

**目标覆盖率**：`StagingBuffer.cpp` ≥ **90%**（stage 覆盖/新增、unstage 命中/未命中、save 成功/空/失败回滚、toMutations 映射/回退、discard 全分支可达）。

### 10.4 `tst_inbound_table_gate`

**被测**：`InboundTableGate::{open,shouldDefer,releaseAll,isOpen}`（`src/sync/diff/InboundTableGate.{h,cpp}`）。
**关联需求/不变量**：会话期被比对表的入站载荷整发暂停、暂不 ACK（E-12）；任一表命中即 defer（整发，不拆分）。
**夹具**：无 IO，纯内存判定；`QSet<QString>` 构造 payload 表集合。

| slot | 断言要点 |
| --- | --- |
| `open_setsWatched_isOpenTrue` | `open({"a","b"})` 后 `isOpen()==true`（watched 非空）。 |
| `isOpen_initialFalse` | 未 open 的 gate `isOpen()==false`（watched 为空）。 |
| `shouldDefer_anyHit_true` | watched={"a","b"}，payload 含 `{"x","a"}`（命中 a）→ `shouldDefer==true`（**整发暂停**，只要任一表命中）。 |
| `shouldDefer_noHit_false` | watched={"a","b"}，payload={"x","y"}（无交集）→ `shouldDefer==false`（放行）。 |
| `shouldDefer_singlePayloadHit_true` | payload 仅含 watched 中一张表（`{"a"}`）→ true，验证「任一」语义不要求全集命中。 |
| `shouldDefer_emptyPayload_false` | payload 为空集 → false（无表可命中）。 |
| `releaseAll_thenDeferFalse` | open 后 `releaseAll()` → `isOpen()==false`，同一 payload 再 `shouldDefer==false`（放行恢复）。 |
| `reopen_replacesWatched` | `open({"a"})` 后再 `open({"b"})` → watched 被替换为 `{"b"}`（非累加）：payload `{"a"}` 不再 defer、`{"b"}` defer。 |

**目标覆盖率**：`InboundTableGate.cpp` ≥ **95%**（四方法 + 命中/未命中/空集分支全覆盖，仅余 mutex 不可测路径）。

### 10.5 `tst_comparison_session`（重点）

**被测**：`ComparisonSession`（实现 `IComparisonSession`，`src/sync/diff/ComparisonSession.{h,cpp}`，560 行）全 API + `OwningComparisonSession` 包装 + `createComparisonSession` 工厂。
**关联需求/不变量**：T4.1/T4.3 会话整链——`initialize`（钉 `data_version` + `BEGIN DEFERRED` 读快照 + 开 gate + 算表级 diff）；`save` 经 `workerCaptureWriteFn`（首选，走 `CapturedWriteTemplate`+`UpsertExecutor`，session 捕获+changelog+广播）或回退 `workerWriteFn`（`StagingBuffer::save` 经 `UpsertExecutor`）；脚下 `data_version` 变动 → `E_SYNC_STAGE_STALE`；`discard` 零落盘；会话期被比对表经 gate 暂停 inbox。
**夹具**：`SqliteFixture`（临时文件库，WAL）建含 PK 的用户表 + `__sync_table_state`，先经 `TableStateStore::applyMutations` 写本地状态；真实 `DiffEngine`/`InboundTableGate`/`UpsertExecutor`/`TableStateStore`；构造 `SyncContext`（refCount 由测试管理）并装配 `workerCaptureWriteFn`/`workerWriteFn`/`rescanFn` 为可断言的桩（记录被调用与入参）。`data_version` 变动用「在第二条独立写连接上对基表执行一次 COMMIT 写」制造（`PRAGMA data_version` 在跨连接提交后递增）。

| slot | 断言要点 |
| --- | --- |
| `initialize_pinsDataVersion_opensGate` | `initialize(snapshots,&err)==true`；内部钉 `pinnedDataVersion_>0`、`readTxnActive_` 开（`BEGIN DEFERRED`）、`gate.isOpen()==true`、`tableDiffs()` 已算出。 |
| `initialize_snapshotConversion` | 经 `IComparisonSession::initialize(QList<RemoteTableSnapshot>)` 重载 → `meta.schemaFingerprint/contentChecksum/rowCount` 正确映射到内部 `RemoteMeta`，`rows` 入 `remoteData_`。 |
| `initialize_readTxnFails_false` | 令 `rconn_.transaction()` 失败（如连接已占用/只读冲突）→ `initialize` 返回 false 且回填 err；gate 不开。 |
| `tableDiffs_afterInit` | `initialize` 后 `tableDiffs()` 返回与 `DiffEngine::tableDiffs` 一致的表级结果（委托）。 |
| `rowDiffs_delegatesToEngine` | `rowDiffs("t",0,10)` 对已知 `remoteData_` 返回与直接调 `DiffEngine::rowDiffs` 一致；表不在 `remoteData_` → 返回空。 |
| `stageRow_remoteRowStaged` | `stageRow("t",pk)` 取 `findRemoteRow` 命中 → staged 非空；远端无该 PK → 返回 false。 |
| `stageTable_stagesNonSameRows` | `stageTable("t")` 对所有非 `Same` 的 diff 行 stage；`Same` 行跳过（断言 staged 数 == Different/Added/Deleted 行数）。 |
| `unstage_removesStaged` | `stageRow` 后 `unstage("t",pk)` → 该行不再在 staging。 |
| `acceptLocal_unstages` | `acceptLocal("t",pk)` 等价 `unstage`（丢弃该行远端暂存）。 |
| `acceptRemote_stagesRemote` | `acceptRemote("t",pk)` 等价 `stageRow`（暂存远端行）。 |
| `stageCell_accumulates` | 同行多次 `stageCell` → 经 `staging_.getRow` 累积（C-4），后一次不覆盖前一次已改单元格；首次从 local 行 seed（local 空则 remote）。 |
| `stageCell_noRow_false` | 目标 PK 在 local 与 remote 均不存在 → `stageCell` 返回 false。 |
| `fetchRemoteRows_keysetPaging` | `fetchRemoteRows("t",token,pageSize,snap)`：`token` 为空从 0 起、非空作 index 提示；返回 `pageSize` 行、每行 `kind==Added`、`primaryKey` 取 PK 列；末页不越界。 |
| `save_viaCaptureWriteFn_localWrite` | 装配 `workerCaptureWriteFn` → `save(&err)==true` 调用之、入参 `mutations` 来自 `toMutations`、`syncTables==canonicalSyncTables`；调用后 staging 清空、`gate.releaseAll`、`rescanFn` 触发（**普通本地写经 UpsertExecutor**）。 |
| `save_fallbackWorkerWriteFn` | 未配 capture、仅配 `workerWriteFn` → 走回退分支，lambda 内 `StagingBuffer::save` 经 `UpsertExecutor` 落库；返回 true。 |
| `save_emptyStaging_releasesGate` | staging 为空 `save` → 直接 `rollback` 读事务 + `releaseAll` + `rescanFn`，返回 true（无写）。 |
| `save_noWorkerQueue_E_SYNC_INIT` | `context_` 无 `workerCaptureWriteFn` 也无 `workerWriteFn` → `save` 返回 false，`*err` 含 `E_SYNC_INIT`（`ASSERT_ERR_CODE`）。 |
| `save_dataVersionChanged_E_SYNC_STAGE_STALE` | **核心**：`initialize` 后在另一连接对基表提交一次写使 `PRAGMA data_version` 递增 → `save` 经 `checkStale` 检出 `current!=pinned` → 返回 false、`*err` 含 `E_SYNC_STAGE_STALE`；staging 被 `discard`、gate 释放、**未落任何暂存写**。 |
| `discard_zeroPersist` | stage 若干行后 `discard()` → staging 清空、读事务 `rollback`、`gate.releaseAll`、`rescanFn` 触发；**目标表与 `__sync_table_state` 均不变**（零落盘，H-09 仍触发 rescan 放行被 defer 的 seen 制品）。 |
| `gate_defersWatchedDuringSession` | `initialize` 开 gate 后，对被比对表的 payload `gate.shouldDefer==true`（inbox 暂停）；`save`/`discard` 后 `shouldDefer==false`（放行）——与 §10.4 联动验证「会话期暂停被比对表」。 |
| `owningSession_cleansUpConnection` | 经 `createComparisonSession(config,&err)` 取得 `OwningComparisonSession`，析构后底层只读 `connName` 已 `removeDatabase`（无「connection still in use」告警，M-6）。 |

**目标覆盖率**：`ComparisonSession.cpp` ≥ **85%**（initialize 双重载 + 七类 stage/accept + fetchRemoteRows 分页 + save 三分支（capture/fallback/空/无队列）+ checkStale + discard + Owning 包装可达；`readDataVersion` 的 query 失败分支与部分 err 文案属缺口）。

### 10.6 `tst_dead_peer_evictor`

**被测**：`DeadPeerEvictor::{configure,evaluate,evict}`；`struct PeerState`、`enum AlertLevel`（`src/sync/peer/DeadPeerEvictor.{h,cpp}`，62 行）。
**关联需求/不变量**：三维阈值（seq/bytes/ms）软告警→硬逐出；硬阈任一命中即 Dead、软阈任一命中即 Lagging、皆未命中 Healthy；`evict` 标记 `pending_baseline=true` 并把 `acked_seq` 置 -1（outbox 截断恢复时，min(acked_seq) 重算自然排除已逐出 peer）。
**夹具**：`evaluate` 注入 `nowMs`（用 `FakeClock`/常量，无真实等待）；`evict` 用 `SqliteFixture` 建 `__sync_outbound_ack` 并经真实 `OutboundAckStore`。本套件无错误码（健康分级与路由判定）。

| slot | 断言要点 |
| --- | --- |
| `evaluate_healthy_allUnderSoft` | 三维 lag 均低于软阈、`msLag<softMs` → `Healthy`。 |
| `evaluate_lagging_softSeq` | `lagSeq>=softSeq_` 且 `<hardSeq_`，其余健康 → `Lagging`（软告警，不逐出）。 |
| `evaluate_lagging_softBytes` | `lagBytes>=softBytes_` 且 `<hardBytes_` → `Lagging`。 |
| `evaluate_lagging_softMs` | 注入 `nowMs` 使 `nowMs-lastAckMs ∈ [softMs,hardMs)` → `Lagging`。 |
| `evaluate_dead_hardSeq` | `lagSeq>=hardSeq_` → `Dead`（硬阈优先于软阈判定）。 |
| `evaluate_dead_hardBytes` | `lagBytes>=hardBytes_` → `Dead`。 |
| `evaluate_dead_hardMs` | `nowMs-lastAckMs>=hardMs_`（注入 nowMs）→ `Dead`。 |
| `evaluate_alreadyEvicted_dead` | `peer.evicted==true` → 直接 `Dead`（短路，不看阈值）。 |
| `evaluate_noTimeData_msIgnored` | `lastAckMs==0`（无时间数据）→ ms 维度不参与（`hasTimeData==false`），仅 seq/bytes 判定。 |
| `evaluate_boundary_geqNotGt` | 边界：lag 恰等于阈值即触发（`>=` 而非 `>`）——`lagSeq==softSeq_` 即 Lagging、`==hardSeq_` 即 Dead。 |
| `evict_marksPendingBaselineAndZeroAck` | `evict(db,"A",ack,&err)==true` → `__sync_outbound_ack` 中 A 的 `pending_baseline==1`、`acked_seq==-1`。 |
| `evict_outboxMinRecompute_excludesDead` | 多 peer（A 存活 acked=100、B 逐出后 acked=-1）→ 逐出后按存活 peer 重算保留水位 `min` 排除 B（-1 不拉低存活集合），验证 outbox 坍缩/截断恢复正确。 |
| `evict_ackStoreFails_false` | `OutboundAckStore::setPendingBaseline` 失败（表缺失）→ `evict` 返回 false、回填 err，UPDATE 不执行。 |

**目标覆盖率**：`DeadPeerEvictor.cpp` ≥ **92%**（evaluate 的 evicted 短路 + 三硬阈 + 三软阈 + 无时间数据 + Healthy 兜底 + evict 成功/失败分支全覆盖）。

### 10.7 `tst_sync_context`（G-07 重点）

**被测**：`SyncContextRegistry::{instance,getOrCreate,getExisting,release,ensureContextUuid}` + 私有 `canonicalKey`（经 `getOrCreate` 回填 `canonicalKeyOut` 间接断言）（`src/sync/SyncContext.{h,cpp}`，163 行）。
**关联需求/不变量**：G-07 单库单上下文——`canonicalKey` 用 OS 文件标识（POSIX `stat` 的 `(st_dev,st_ino)`；Windows 卷序列+文件索引）而非路径字符串，故 symlink/hardlink/相对路径/不同大小写/`file://` URI 等**指向同一物理库的别名得到同一 key**（同一 `SyncContext`）；不同物理库不同 key；refCount 末位 `release` 才析构；`ensureContextUuid` 二次确认（库无 UUID 则写入、有相同则 no-op、有不同则读回采纳）。
**夹具**：`SqliteFixture` 或临时目录建真实库文件（`canonicalKey` 要求文件存在才能 `stat`）；用 `QFile::link`（symlink）、`::link`（hardlink）构造别名；`getOrCreate` 后须配对 `release` 防泄漏。注意：`canonicalKey` 接受**已存在文件路径**，对不存在路径回填 err（M-04，无路径回退）。

| slot | 断言要点 |
| --- | --- |
| `canonicalKey_symlinkSamePhysical_sameKey` | 对真实库 `db.sqlite` 与其 symlink `link.sqlite` 分别 `getOrCreate` → 两次 `canonicalKeyOut` 相同，返回**同一** `shared_ptr<SyncContext>`（refCount 累加为 2，G-07）。 |
| `canonicalKey_hardlinkSamePhysical_sameKey` | hardlink（同 inode）别名 → 同 key、同 context（dev+inode 相同）。 |
| `canonicalKey_relativeVsAbsolute_sameKey` | 相对路径与绝对路径指向同一文件 → 同 key（`stat` 解析到同 inode，与字符串形式无关）。 |
| `canonicalKey_caseVariant_sameKey` `[平台相关]` | 大小写不同的路径指向同一文件（依赖文件系统大小写不敏感）→ 同 key；大小写敏感 FS 上标 `QSKIP`。 |
| `canonicalKey_fileUriOrTrailingForm_sameKey` | 经规范化后指向同一物理文件的等价路径形式 → 同 key（OS 文件标识去重，不依赖字符串归一）。 |
| `canonicalKey_differentFiles_differentKey` | 两个不同物理库文件 → 不同 key、不同 context（refCount 各为 1）。 |
| `canonicalKey_missingFile_errNoKey` | 不存在的路径 → `canonicalKey` 回填 err、返回空 key；`getOrCreate` 返回 nullptr（M-04：无路径回退，保单写）。 |
| `refCount_lastReleaseDestroys` | 同库 `getOrCreate` 两次（refCount=2）→ `release` 一次后 `getExisting` 仍返回非空（未析构）；再 `release` 一次（refCount=0）后 `getExisting` 返回 nullptr（已析构并从 registry 移除）。 |
| `getExisting_noRefCountBump` | `getOrCreate` 一次（refCount=1）后多次 `getExisting` → 不增 refCount；之后单次 `release` 即析构（验证 getExisting 不持内部计数，J-10）。 |
| `getExisting_absent_nullptr` | 未注册的库路径 `getExisting` 返回 nullptr，不创建条目。 |
| `ensureContextUuid_writesWhenAbsent` | 新库无 UUID → `ensureContextUuid(db,&uuid,&err)` 写入 `__sync_context_meta(context_uuid)`，`uuid` 不变、返回 true。 |
| `ensureContextUuid_noopWhenSame` | 库已存相同 UUID → no-op、返回 true，`uuid` 不变（**二次确认**幂等）。 |
| `ensureContextUuid_adoptsStoredOnRestart` | 库已存不同 UUID（模拟重启，内存新生成）→ 读回库内值赋给 `*uuid`、返回 true（采纳持久化值，H-01）。 |
| `ensureContextUuid_queryFails_false` | DDL/查询失败（如库只读）→ 返回 false、回填 err。 |

**目标覆盖率**：`SyncContext.cpp` ≥ **90%**（canonicalKey 成功/缺文件 + getOrCreate 新建/复用 + getExisting 命中/缺失 + release 减计/析构 + ensureContextUuid 三态可达；Windows `#ifdef` 分支在 POSIX runner 上属平台缺口）。

### 10.8 `tst_foreground_gate`

**被测**：`ForegroundGate::{tryAcquire,release,isHeld}`（`src/sync/ForegroundGate.h`，header-only inline）。
**关联需求/不变量**：每库前台操作至多一个；重入第二次 `tryAcquire` 返回 false 且 `*err=="E_BUSY"`；后台 inbox apply/broadcast 不受此 gate 约束（不在本套件范围）。
**夹具**：无 IO，纯内存（`QMutex` + bool）。

| slot | 断言要点 |
| --- | --- |
| `tryAcquire_firstSucceeds` | 新 gate `tryAcquire(&err)==true`；`isHeld()==true`。 |
| `tryAcquire_reentrant_E_BUSY` | 已持有时再 `tryAcquire(&err)==false`，`err=="E_BUSY"`（`ASSERT_ERR_CODE` 或直接断言文案）；`isHeld` 仍 true。 |
| `release_clearsHeld` | `release()` 后 `isHeld()==false`。 |
| `reacquireAfterRelease` | acquire→release→再 `tryAcquire==true`（释放后可再获取，无残留）。 |
| `isHeld_initialFalse` | 未 acquire 的 gate `isHeld()==false`。 |
| `release_idempotent` | 未持有时 `release()` 不报错、`isHeld` 保持 false（幂等释放）。 |

**目标覆盖率**：`ForegroundGate` ≥ **95%**（三方法 + 持有/未持有分支全覆盖）。

### 10.9 `tst_write_txn`

**被测**：`WriteTxn::{begin,commit,rollback,isActive}`（`src/sync/WriteTxn.{h,cpp}`）。
**关联需求/不变量**：写事务以 `BEGIN IMMEDIATE` 开启（立即取写锁，避免升级死锁）；析构时若仍 active 则 `rollback`（RAII）；commit 失败自动 rollback。
**夹具**：`SqliteFixture`（临时文件库）建一张表作写入对象；用第二连接观测「事务期内未提交写对外不可见」。

| slot | 断言要点 |
| --- | --- |
| `begin_immediate_takesWriteLock` | `begin(&err)==true`、`isActive()==true`；并发第二连接此时 `BEGIN IMMEDIATE` 应 busy（验证 IMMEDIATE 立即取写锁，而非 DEFERRED 延迟）。 |
| `commit_persists` | `begin`→执行 INSERT→`commit(&err)==true`、`isActive()==false`；第二连接可见已提交行。 |
| `rollback_discards` | `begin`→INSERT→`rollback()` → `isActive()==false`，表中无该行（写被丢弃）。 |
| `destructor_rollsBackActive` | 在作用域内 `begin`+INSERT 后不 commit，作用域结束析构 → 自动 rollback，库无残留（RAII，断言行数不变）。 |
| `commit_withoutBegin_or_doubleBegin` | 未 `begin` 直接 `commit` → 失败回填 err（无活跃事务，SQLite 报错）；已 active 再 `begin` → 第二次 `BEGIN IMMEDIATE` 失败（嵌套事务非法），err 回填。 |
| `rollback_whenInactive_noop` | `isActive()==false` 时 `rollback()` 直接返回（短路，不执行 SQL）。 |
| `isActive_tracksState` | 跨 begin/commit/rollback 全程 `isActive()` 与实际事务状态一致。 |

**目标覆盖率**：`WriteTxn.cpp` ≥ **95%**（begin/commit/rollback 成功路径 + commit 失败回滚 + rollback 非活跃短路 + 析构回滚全覆盖）。

### 10.10 `tst_sync_config_builder`

**被测**：`SyncConfig::Builder`（全字段 setter + `build`）、`SyncConfig` getter（含 `originRank`、`gapTimeoutMs` 等）（`include/dbridge/sync/SyncConfig.h`，header-only）。
**关联需求/不变量**：Builder 链式设全字段 → `build(err)` 做完整性校验后产出**不可变值对象**（`valid_==true`）；任一校验失败回填 err 并返回无效对象（`isValid()==false`）；`originRank(origin)` 未配置回退 0；默认值（`ackMaxDelayMs=5000`、`broadcastIntervalMs=5000`、`gapTimeoutMs=30000` 等）。
**夹具**：无 IO，纯值对象构造。

| slot | 断言要点 |
| --- | --- |
| `build_allFields_validReadback` | 设全部字段（含 role/nodeId/center/peers/paths/ranks/各阈值）→ `build(&err)` 成功、`isValid()==true`，所有 getter 回读与设入一致。 |
| `build_missingNodeId_fails` | 不设 nodeId → `build` 返回无效对象、`err=="nodeId is required"`、`isValid()==false`。 |
| `build_missingDatabase_fails` | 缺 `database()` → `err=="database path is required"`。 |
| `build_missingOutboxOrInbox_fails` | 缺 outboxDir 或 inboxDir → `err` 含 "outboxDir and inboxDir are required"。 |
| `build_edgeWithoutCenter_fails` | `role(Edge)` 但无 centerNodeId → `err` 含 "centerNodeId is required for Edge role"。 |
| `build_nodeIdInPeers_fails` | peerNodes 含本机 nodeId → `err` 含 "nodeId must not appear in peerNodes"。 |
| `build_duplicatePeer_fails` | peerNodes 含重复项 → `err` 含 "duplicate entry"。 |
| `build_softGtHard_fails` | `peerLagSoftSeq>peerLagHardSeq`（或 bytes/ms 同理）→ `err` 含 "must be <= peerLagHard..."。 |
| `build_nonPositiveThreshold_fails` | `gapTimeoutMs<=0` / `ackMaxDelayMs<=0` / `broadcastIntervalMs<=0` / `maxSelectionSize<=0` 等 → 各自回填「must be positive」类 err。 |
| `build_duplicateRank_fails` | 两 origin 配相同 rank → `err` 含 "duplicate rank value"（H-01：rank 必须全局唯一，保冲突仲裁确定）。 |
| `originRank_mappingAndDefault` | `originPriority("A",10)` → `originRank("A")==10`；未配置的 `originRank("Z")==0`（默认）；`allRanks()` 含已配置项。 |
| `defaults_whenUnset` | 仅设必填项 build 成功后：`ackMaxDelayMs()==5000`、`broadcastIntervalMs()==5000`、`gapTimeoutMs()==30000`、`verifySchemaFingerprint()==true`、`conflictPolicy()==SourceWins`、`role()==Edge`。 |
| `immutability_noSettersOnResult` | `build` 产出的 `SyncConfig` 只有 getter（编译期不可变）；二次 `build` 同 Builder 得等价值对象（Builder 状态可复用产出一致结果）。 |

**目标覆盖率**：`SyncConfig.h`（Builder::build 校验链）≥ **95%**（必填缺失 + 各阈值正性 + soft<=hard + 重复 peer/rank + Edge center 分支基本可穷举）。

### 10.11 `tst_sync_selection_builder`

**被测**：`SyncSelection::Builder::{addRecord,addRecords,addWhere,includeFkDependencies,pruneConsistentDependencies,build}` + 私有 `isSimpleIdent`（经 addRecord/build 间接断言）（`include/dbridge/sync/SyncSelection.h`，header-only）。
**关联需求/不变量**：受限 DSL（设计 §4.4：MVP 仅 PK 集选择）——`isSimpleIdent` 拒非法表名（非 `[A-Za-z_][A-Za-z0-9_]*`，防表名注入 H-11）、`addWhere` 携带原始 SQL 则 build 失败（H-01，不静默吞）；`includeFkDeps`/`pruneConsistent` 默认 true；`isEmpty`/`build` 语义。
**夹具**：无 IO，纯值对象构造。错误码 `E_SYNC_SELECTION_EMPTY`（兼作非法标识/原始 SQL 的拒绝码）。

| slot | 断言要点 |
| --- | --- |
| `addRecord_validIdent` | `addRecord("t","1")` → `records()` 含 `{t,1}`；`build` 成功、`isEmpty()==false`。 |
| `addRecords_expandsPks` | `addRecords("t",{"1","2","3"})` → 产生 3 条 Record（同表不同 PK）。 |
| `addRecord_invalidIdent_buildFails` | `addRecord("t; DROP TABLE x","1")`（含非法字符）→ build 返回空 selection、`err` 含 `E_SYNC_SELECTION_EMPTY` 与 "not a valid SQLite identifier"（`isSimpleIdent` 拒）。 |
| `addRecords_invalidIdent_buildFails` | `addRecords("1bad",{...})`（首字符为数字）→ 记入 invalidTables、build 失败。 |
| `isSimpleIdent_rejectsEmptyAndLeadingDigit` | 空串、以数字开头、含空格/标点的表名均被拒；下划线开头、含数字的合法标识通过（边界数据驱动）。 |
| `addWhere_rawSql_buildFails` | `addWhere("t","age > 18")`（非空原始 SQL）→ build 失败，`err` 含 "addWhere() with raw SQL is not supported in MVP (design §4.4)"（受限 DSL 对齐 §4.4）。 |
| `addWhere_emptyExpr_notRecorded` | `addWhere("t","")`（空表达式）→ 不计入 rawWhereAttempts，但仍 append 空 whereClause（不触发 build 失败）。 |
| `includeFkDeps_defaultTrue` | 未调用 setter → `includeFkDeps()==true`；`includeFkDependencies(false)` 后转 false。 |
| `pruneConsistent_defaultTrue` | 默认 `pruneConsistent()==true`；`pruneConsistentDependencies(false)` 后转 false。 |
| `isEmpty_noRecords` | 未 addRecord/addWhere → `isEmpty()==true`；build 仍回填 `E_SYNC_SELECTION_EMPTY`（空选择）。 |
| `build_validSelection_passthrough` | 含合法 record 的选择 → `build(&err)` 不设 err、`records()` 原样透传。 |

**目标覆盖率**：`SyncSelection.h` ≥ **95%**（isSimpleIdent 各拒绝/通过分支 + addWhere 原始 SQL 拒绝 + 空选择 + 默认值 + FK/prune 双态全覆盖）。

### 10.12 `tst_import_service_regression`（提取守护，对齐 plan T2.0a）

**被测**：`ImportService::run`（`src/service/ImportService.{h,cpp}`，UPSERT 写循环 683-731 行被提取为 `UpsertExecutor` 的对象）。
**关联需求/不变量**：T2.0a 提取守护（Q-03）——在把 `ImportService.cpp` UPSERT 循环抽为 `UpsertExecutor` **之前先补齐回归（红）**，提取后行为完全不变（绿）。覆盖 multi-table、lookup、fkInject、行级 skip、`writtenRows` 计数、rollback、`DO NOTHING`、bind 序八项。
**夹具**：`SqliteFixture`（临时文件库，`PRAGMA foreign_keys=ON`）+ `tests/data/sync/` 建表脚本 + `ProfileSpec`/`SchemaCatalog` 构造多表/lookup/FK 拓扑；真实 `.xlsx` 输入或等价行集。错误码 `E_DB_UPSERT`。

| slot | 断言要点 |
| --- | --- |
| `run_multiTable_routesAllWritten` | 一张 Excel 行经 Mixed/多 RouteSpec 拓扑写入多张表 → 每表对应行落库、`writtenRows` 计 Excel 行数（非 payload 数）。 |
| `run_lookup_resolvesForeignValue` | lookup 列经查表解析为外键值后写入 → 目标列存解析后的值（lookup 失败行计 error/skip，不写）。 |
| `run_fkInject_parentChildOrder` | FK 注入按拓扑序写父后子；父注入失败 → 子被 `buildDescendantFailSet` 整闭包跳过（H-04），`writtenRows` 不计这些行。 |
| `run_rowLevelSkip_failedRowsExcluded` | 含映射/校验错误的 Excel 行（`failedExcelRows` 且 `hasNonRouteError`）整行跳过；仅 route-local 错误的行其有效兄弟 route 仍写（H-02/M-04）。 |
| `run_writtenRows_onlyOnActualUpsert` | `writtenRows` 仅当某 Excel 行至少一个 payload 真正 upsert 才 +1（全 payload 被 `skipPayloadIndices` 抑制的行不计，H-01）。 |
| `run_rollback_onAbortOnError` | `abortOnError=true` 且发生 table/row error → 整事务回滚（`manageTransaction` 路径），目标表无任何残留写入。 |
| `run_doNothing_conflictNoOverwrite` | `DO NOTHING` 冲突模式：PK 冲突行不覆盖已有值（`SqlBuilder::buildUpsert` 强制 DO NOTHING）；既有行保持原值。 |
| `run_bindOrder_matchesColumns` | 绑定值顺序与 `payload.binds`/`dbColumns` 一致（提取后 `RowMutation.columns/values` 序与原 prepared `addBindValue` 序一致）——同一输入提取前后写入的列值映射逐列相等。 |
| `run_prepareFails_E_DB_UPSERT` | 构造无效 SQL（如列不存在）使 `q.prepare` 失败 → 回填 `E_DB_UPSERT`、`writeOk=false`、break（断言错误码经 `ASSERT_ERR_CODE`）。 |
| `run_extractionParity_goldenSnapshot` | **提取等价黄金断言**：同一组输入在提取前后产出的 `ImportResult`（`ok`/`writtenRows`/`readRows`/`errors` 码与序）与目标表终态完全一致（先红后绿的不变性锚点）。 |

**目标覆盖率**：守护既有 `ImportService` 写阶段覆盖（重点是 UPSERT 循环 → `UpsertExecutor` 边界行为不变），不追加新增覆盖率指标，以「提取前后双跑同绿」为验收。

### 10.13 `tst_export_service` / `tst_batch_transfer`

**被测**：`ExportService::run`（`src/service/ExportService.{h,cpp}`，既有路径补强）；`BatchTransfer`（`IBatchTransfer` 8+3 接口实现 + `createBatchTransfer`，`src/batch/BatchTransfer.{h,cpp}`，408 行原无测试）。
**关联需求/不变量**：T3.1——`BatchTransfer` `startImport/startExport` 非阻塞受理（`QtConcurrent::run` 后台）、`importProgress/Result`/`exportProgress/Result` 锁内快照轮询、`stop` 经原子标志优雅停、`importState/exportState` 状态机；`ForegroundGate` 互斥（`tryAcquire` 失败回 `E_BUSY`）；导入若 sync 活跃则经 `ctx->importFn`（SyncWorker wconn + session 捕获，维护 changelog+table_state），否则回退本连接直跑。
**夹具**：`SqliteFixture` + `DataBridge` 实例（`snapshotProfileCatalog`/`dbPath`）；可控 `.xlsx`；用 `QTRY_VERIFY`/`QTRY_COMPARE` 轮询异步状态（非真实 sleep）；构造已注册 `SyncContext`（带/不带 `importFn`）以覆盖两条导入路径。错误码 `E_BUSY`、`E_OPEN_DB`。

| slot | 断言要点 |
| --- | --- |
| `export_run_writesXlsx` | `ExportService::run(profile,catalog,xlsx,opts,db)` 成功 → 输出 `.xlsx` 行数==查询结果行数，`result.ok==true`、`writtenRows` 正确。 |
| `export_invalidProfile_fails` | 导出 profile 校验失败（列序/缺表）→ `result.ok==false`、`errors` 非空（与 `ProfileValidator::validateForExport` 一致）。 |
| `batch_startImport_nonBlockingAccept` | `startImport(opts,&err)==true` 立即返回（不阻塞）；`importState()` 经 `QTRY` 由 `Running` 转 `Completed`。 |
| `batch_importProgress_polling` | 轮询 `importProgress()` → percent 由 0→50→100 推进；完成后 `rowsDone==writtenRows`、`rowsTotal==readRows`（>0 时）。 |
| `batch_importResult_afterCompletion` | 完成后 `importResult().ok==true`、`importErrors()` 与 result.errors 一致（锁内快照）。 |
| `batch_startExport_nonBlocking` | `startExport` 受理后 `exportState()` 经 `QTRY` 转 `Completed`；`exportProgress().percent==100`。 |
| `batch_stop_setsStoppingThenStopped` | 运行中 `stop()` → `importState()`/`exportState()` 转 `Stopping` 再终结为 `Stopped`（原子标志被 worker 读到提前退出）；`stop` 返回 true。 |
| `batch_startImport_whileRunning_rejected` | 已 `Running`/`Stopping` 时再 `startImport` → 返回 false、`err=="Import already running"`（自身重入拒绝）。 |
| `batch_foregroundGate_E_BUSY` | 已注册 `SyncContext` 且 gate 被占用（前台操作进行中）→ `startImport`/`startExport` 的 `ctx->gate.tryAcquire` 失败 → 返回 false、`err=="E_BUSY"`（`ASSERT_ERR_CODE`）。 |
| `batch_emptyXlsxPath_rejected` | `xlsxPath` 为空 → `startImport`/`startExport` 返回 false、`err` 含 "xlsxPath is required"。 |
| `batch_importViaSyncWorker_maintainsTableState` | sync 活跃（`ctx->importFn` 已装配）→ `runImport` 走 `importFn`（经 wconn + `CapturedWriteTemplate`），导入后 `__sync_table_state` 三元组被更新、changelog 追加（Q-01：导入经 wconn 维护 table_state）。 |
| `batch_importFallbackDirect_whenNoSync` | 无 `SyncContext`/无 `importFn` → 回退本线程独立连接直跑 `ImportService`（不触 session 捕获），结果仍正确落库。 |
| `batch_openDbFails_E_OPEN_DB` | 库路径不可打开 → result.errors 含 `E_OPEN_DB`、`importState()`/`exportState()` 终结为 `Failed`。 |
| `batch_destructor_gracefulShutdown` | 运行中析构 `BatchTransfer` → 置 stop 标志并 `waitForFinished`，无悬挂线程/崩溃（best-effort 优雅停）。 |

**目标覆盖率**：`ExportService.cpp` 补强既有路径覆盖；`BatchTransfer.cpp` ≥ **80%**（startImport/startExport 受理 + 两条导入路径 + 轮询 getter + stop + 状态机 + gate 互斥 + 析构全覆盖；ms 级竞态窗口与部分 `Stopping` 中间态属轮询缺口）。

### 10.14 本节套件与覆盖目标汇总

| 套件 | 被测文件 | 子系统 | 关键不变量/错误码 | 目标行覆盖率 | 用例数 |
| --- | --- | --- | --- | --- | --- |
| `tst_diff_engine` | `DiffEngine.cpp`（224） | diff | 零全量、判等三元组（R-08/G-06） | ≈ 82% | 15 |
| `tst_staging_buffer` | `StagingBuffer.cpp`（103） | diff | save 经 UpsertExecutor、discard 零落盘；`E_DB_UPSERT` | ≥ 90% | 10 |
| `tst_inbound_table_gate` | `InboundTableGate.cpp` | diff | 任一表命中整发暂停（E-12） | ≥ 95% | 8 |
| `tst_comparison_session` | `ComparisonSession.cpp`（560） | diff | `E_SYNC_STAGE_STALE`、save 本地写、discard 零落盘 | ≥ 85% | 22 |
| `tst_dead_peer_evictor` | `DeadPeerEvictor.cpp`（62） | peer | 三维阈值软告警→硬逐出、outbox 坍缩 | ≥ 92% | 13 |
| `tst_sync_context` | `SyncContext.cpp`（163） | core | OS 文件标识去重（G-07）、refCount、UUID 二次确认 | ≥ 90% | 14 |
| `tst_foreground_gate` | `ForegroundGate.h` | core | 重入 `E_BUSY` | ≥ 95% | 6 |
| `tst_write_txn` | `WriteTxn.cpp` | core | `BEGIN IMMEDIATE` RAII | ≥ 95% | 7 |
| `tst_sync_config_builder` | `SyncConfig.h` | core | 完整性校验产出不可变、`originRank` | ≥ 95% | 13 |
| `tst_sync_selection_builder` | `SyncSelection.h` | core | 受限 DSL（§4.4）、`E_SYNC_SELECTION_EMPTY` | ≥ 95% | 11 |
| `tst_import_service_regression` | `ImportService.cpp` | ETL | 提取前后行为不变（T2.0a，先红后绿）；`E_DB_UPSERT` | 守护现有 | 10 |
| `tst_export_service` / `tst_batch_transfer` | `ExportService.cpp` / `BatchTransfer.cpp`（408） | ETL | 非阻塞/轮询/stop/`E_BUSY` 互斥、导入维护 table_state | ≥ 80% | 14 |

**本节小结**：diff 子系统以 `tst_comparison_session`（22 例）为重点锁定「钉 data_version → 脚下变动 `E_SYNC_STAGE_STALE`」「save 普通本地写经 UpsertExecutor」「discard 零落盘」「会话期被比对表暂停 inbox」四条核心，配合 `tst_diff_engine` 的判等三元组（R-08：内容同/high_water 异仍 Identical，checksum 异即 Different）与零全量（读行数有界）；core 套件覆盖 G-07 单库单上下文（symlink/hardlink/相对路径/大小写/URI 别名同 key、refCount 末位析构、UUID 二次确认）与前台串行/写事务/不可变配置/受限选择 DSL；ETL 三套件以「提取守护先红后绿」保 `UpsertExecutor` 抽取零回归，并为原无测试的 408 行 `BatchTransfer` 自 80% 起步补强非阻塞受理、轮询、互斥与 table_state 维护。所有时间相关断言（`DeadPeerEvictor` 阈值、`BatchTransfer` 异步状态）经 `FakeClock` 注入或 `QTRY_*` 轮询，无真实 `sleep`。
## 11. 集成测试计划（端到端场景）

本节定义按里程碑 M1–M5 组织的端到端集成测试套件（命名 `tst_sync_*` / `tst_batch_*` / `tst_comparison_*`）。集成测试与第 4–8 节单元测试的分工是：单元测试隔离验证单个组件的分支与不变量，集成测试则**驱动真实编排路径**——把 `SyncWorker` 主循环、`SyncEngine` 双状态机、`transport`（outbox/inbox + `.ready` 哨兵 + `InboxLedger`）、`apply` 三分支模板、`conflict` 仲裁串起来，在 `TwoNodeHarness`/`StarHarness`/`CrashHarness`（§3.4/§3.5）上以**手动 drain+deliver** 驱动、确定可重放、无真实 `sleep`（时间相关走 `FakeClock`，§3.6）。

每个场景给出「**目的 / 前置 / 步骤 / 断言 / 关联需求**」五段式。所有写入均经唯一写线程 `wconn` + `WriteTxn(BEGIN IMMEDIATE)`；入站应用经 `CapturedWriteTemplate` 分支 A（M1 唯一被驱动写路径，S-04），分支 B（selectionpush）由 M2 接入、分支 C（本地写）由 M3/M4 接入。错误码断言统一用 `ASSERT_ERR_CODE`（§3.7），状态/进度断言经公开面 `ISyncEngine` 8+1 getter（`state()`/`progress()`/`errors()`/`result()`，由 `createSyncEngine(bridge)` 构造）。

### 11.1 M1 — 两节点最小同步

M1 套件运行在 `TwoNodeHarness`（A=center / B=edge，交叉绑定 outbox/inbox）与 `CrashHarness` 上，验证 M1 DoD：A↔B 双向收敛、幂等 + 严格连续、origin 不重铸、单边逐行胜者、崩溃零窗口、等 ACK 状态机、eligibility 闸门、旧写拒绝、单库单 context。

#### 11.1.1 tst_sync_two_node_converge

- **目的**：验证 A↔B 双向增量经多轮 drain+deliver 后两库 `table_state` 三元组完全一致（`converged()`），即 M1 收敛终态判据成立。
- **前置**：`TwoNodeHarness h`；两节点各 `seed(01_rowid_explicit_pk.sql)`（schema 同构、PK 单列）；两侧均经 `initialize()` 进入同步模式（eligibility 通过）；rank 表配置 `{A:10, B:5}`。
- **步骤**：
  1. 在 A 上经 `wconn` 写入若干行（`applyWrite(node("A"), ...)`），在 B 上写入另一批不相交主键行。
  2. `h.runUntilQuiescent(maxRounds=8)`：反复 `drain(A)`/`drain(B)` 扫各自 inbox→分支 A apply→打包发 outbox，`deliver()` 把已 `.ready` 制品搬到对端 inbox。
  3. 在 A 再写一批与 B 行**主键相交**但低 rank 的更新，再次 `runUntilQuiescent`。
- **断言**：
  - `h.converged(&diff)` 为真（`QVERIFY2(... , qPrintable(diff))`）：两库 `__sync_row_winner` 三元组（key / content_checksum / winner(origin,seq)）逐表逐行一致。
  - 两库业务行内容逐行相等（独立 SELECT 比对，不依赖 table_state）。
  - 收敛后再空跑一轮 `drain+deliver` 产生 0 新制品（静默，防回声前哨，C2）。
- **关联需求**：FR-6 收敛、`converged()` 三元组（G-06）、M1 DoD 双向收敛。

#### 11.1.2 tst_sync_strict_sequential

- **目的**：验证入站严格连续（G-05）四条子路径：① 重投幂等；② 乱序 `seq=2` 先到不推高水位致 `seq=1` 丢更；③ 缺口 pending（ledger `seen`）补齐后应用并转 `consumed`；④ 缺口超 `gapTimeout` → `E_SYNC_GAP` → 回退基线。
- **前置**：`TwoNodeHarness`；B 已 `initialize`；A 录制三个连续 changeset 制品 `(origin=A, epoch=e, seq=1/2/3)`（经 `ChangesetFixture`）；B 侧 `InboxLedger` 注入 `FakeClock`。
- **步骤 / 断言**（分四个 data-row 子用例 `_data()`）：
  - **① 重投幂等**：把 `seq=1` 制品 deliver 到 B 两次，`drain(B)` 两轮。断言：第二次进入消费流程被 `InboxLedger`（status==Consumed）跳过为 no-op；B 的 `applied_vector` 高水位仍为 1，业务行无重复，`changesApplied` 计数只 +1。
  - **② 乱序先到**：先只 deliver `seq=2`，`drain(B)`。断言：`seq=2` 不被应用（高水位仍 0，制品在 ledger 记 `seen` 挂起为 pending，**不**推高水位）；随后 deliver `seq=1`，`drain(B)`，断言 `seq=1`、`seq=2` 依序应用，高水位推到 2，`seq=1` 的更未丢失（终值=seq=1 应用后再被 seq=2 覆盖的正确链）。
  - **③ 缺口 pending 补齐**：deliver `seq=1`、`seq=3`（缺 2），`drain(B)`。断言：`seq=1` 应用、`seq=3` 在 ledger 记 `seen` pending（高水位停在 1）；补 deliver `seq=2`，`drain(B)`（三时机重扫），断言 `seq=2`、`seq=3` 续判应用、高水位到 3，三制品 ledger 均转 `Consumed`。
  - **④ 缺口超时**：deliver `seq=1`、`seq=3`，`drain(B)`，`FakeClock.advance(gapTimeoutMs+1)`，再 `drain(B)`（stalePending 命中）。断言：`errors()` 含 `E_SYNC_GAP`（`ASSERT_ERR_CODE` 语义），后台状态机转向 baseline 回退路径（见 §11.5.1 衔接），高水位不前进。
- **关联需求**：C6/G-05 严格连续 + 缺口；`E_SYNC_GAP`；S-01 缺口 pending 复用 inbox+ledger。

#### 11.1.3 tst_sync_row_winner_convergence

- **目的**：验证 FR-6/G-01 逐行胜者**到达序无关**：`rank(B) > rank(D)`，无论高/低 rank 谁先到，终态恒为高 rank 值。
- **前置**：`TwoNodeHarness`（此处 A=center 仅作 apply 节点，源记为 B/D；rank 表 `{B:8, D:3}`）；两源对**同一主键**各产生一次更新（`ChangesetFixture` 录制 `B@seq=k`、`D@seq=m`）。
- **步骤 / 断言**（两个子用例）：
  - **① 高 rank 先到、低 rank 跨批后到 OMIT**：先 deliver+apply B 的更新（提交、`row_winner=B`），下一批 deliver+apply D 的更新。断言：冲突回调判 `D` 不胜（rank 低），变更被 OMIT，业务行终值=B 值，`row_winner` 仍记 B；`conflicts` 计数 +1，必要时 `W_SYNC_CONFLICT_REPLACED` 不触发（未替换）。
  - **② 反序到达（低 rank 先、高 rank 后）**：先 apply D（暂态 `row_winner=D`、业务行=D 值），再 apply B。断言：B rank 高，覆盖成功，业务行终值=B 值、`row_winner=B`；与子用例①终态**逐字节相同**（到达序无关）。
- **关联需求**：FR-6/G-01 到达序无关；`RowWinnerStore`（T1.7b）；M1b A↔B 单边冲突裁决（R-01）。

#### 11.1.4 tst_sync_origin_not_recast

- **目的**：验证 FR-9/G-03：入站 changeset 经分支 A apply 后，`changelog` 以 `appendForward` 记**原 origin**（非中心 origin、不 fresh 捕获），转发可被对端按原 origin 识别。
- **前置**：三节点链 A→Center→B（用 `StarHarness` 退化或 `TwoNodeHarness` 串联）；A 产生 `(origin=A, seq=7)` changeset，先到 Center。
- **步骤**：
  1. Center `drain`：分支 A apply A 的 changeset；查 Center `__sync_changelog` 该条目 origin 字段。
  2. Center 经 `RoutingTable` 转发到 B（`shouldRoute` 判 `origin!=peer && seq>acked`）。
  3. B `drain` 应用。
- **断言**：
  - Center changelog 该条 `origin == "A"`（**未**被重铸为 "Center"），blob 为原始 blob（`appendForward` 不重新 session 捕获）。
  - B 应用后其 `applied_vector` 记的是 `origin=A` 的高水位（按原 origin 计），不是 Center；防止 origin 漂移导致重复/回声。
  - 把同制品再回投 A（C2 防回声前提）：A 因 `origin==self` 不重复应用。
- **关联需求**：FR-9/G-03 origin 不重铸；`ChangelogStore.appendForward`；R-02。

#### 11.1.5 tst_sync_crash_zero_window（CrashHarness）

- **目的**：验证 FR-1 崩溃零窗口——在四个注入点 `SIGKILL` 后重启，五结构（业务行 / `__sync_changelog` / `__sync_applied_vector` / `__sync_row_winner` / `__sync_inbox_ledger`）**原子一致**（全有或全无），不丢、不重复消费。
- **前置**：`CrashHarness ch(fx.filePath())`，单库复用；实现已埋 `DBRIDGE_FAULT_POINT`（`DBRIDGE_FAULT_INJECTION` 编译门控）；工作负载 = 应用一条入站 changeset（分支 A）。
- **步骤 / 断言**（四个 data-row，每行一个注入点）：
  - `before_commit`：`runChildUntilCrash(BeforeCommit, "apply_one")` → `reopenAndInspect()`。断言 `isAtomic(v)` 且五标志**全 false**（事务未提交，全未发生）。
  - `after_seal`：seal 入 changelog 后、COMMIT 前 kill。断言五标志**全 false**（seal 与业务写同事务，事务未提交故全未发生）。
  - `after_commit`：COMMIT 后、后续标记前 kill。断言五标志**全 true**（业务行 + changelog + applied_vector + row_winner 原子已落）。重启后重扫 inbox：该制品 ledger 即便尚未标 consumed，重启重判须识别为已应用（按 applied_vector 高水位幂等），**不重复消费**。
  - `after_markconsumed`：标记 ledger consumed 后 kill。断言五标志全 true 且 `inboxLedgerConsumed==true`；重启后该制品不再被 `scan` 返回。
- **关联需求**：FR-1 零窗口；五结构原子集（§3.5）；S-01 缺口 pending 跨崩溃续判不丢不重。

#### 11.1.6 tst_sync_ack_wait

- **目的**：验证前台状态机：受理后 `Exporting(percent=-1)`；收全片 ACK 才 `Completed`；不回 ACK 超 `ackMaxDelayMs` → `Failed` 且 `errors()` 含 `E_SYNC_ACK_TIMEOUT`（区别于泛 `Failed`）。
- **前置**：`TwoNodeHarness` + `SyncEngine`（`createSyncEngine`）；`AckChannel` 注入 `FakeClock`；A 调 `sync()` 发起一轮导出。
- **步骤 / 断言**（两个子用例）：
  - **① 正常收 ACK**：A `sync()` 后立即断言 `state()==Exporting`、`progress().percent==-1`（不确定进度）。`deliver()` 制品到 B，`drain(B)` 触发 B 攒批 ACK，`deliver()` ACK 回 A，A `drain` 消费 ACK。当全片 ACK 收齐：断言 `state()==Completed`、`result()` 成功、`lastAckedSeq` 推进到本轮最高 seq。
  - **② ACK 超时**：A `sync()` 后**不** deliver B 的 ACK；`FakeClock.advance(ackMaxDelayMs+1)`，推进 worker 一轮。断言 `state()==Failed`，且 `errors()` 含 `E_SYNC_ACK_TIMEOUT`（`ASSERT_ERR_CODE` 映射），而非无码的泛 Failed；后台 outbox 仍保留未确认制品供后续重试（不静默丢弃）。
- **关联需求**：前台双状态机（Exporting→Completed/Failed）；`E_SYNC_ACK_TIMEOUT`；F-14 ACK SLA。

#### 11.1.7 tst_sync_eligibility_gate

- **目的**：验证 eligibility 闸门在 `initialize()` 内（open `wconn` 后、session attach 前）显式校验：无 PK 表/视图配为同步表 → 返回 false + `E_SYNC_UNSUPPORTED_SCHEMA`，不进同步模式；缺 SESSION 宏 → `E_SYNC_SESSION_UNAVAILABLE`。
- **前置**：`SyncEngine`；分别 `seed(04_no_explicit_pk.sql)`（隐式 rowid）、`seed(05_view.sql)`（视图）、`seed(10_partial_expr_unique.sql)`（无可用冲突目标）。
- **步骤 / 断言**：
  - 用 `04`/`05`/`10` 三种 schema 各配为同步表调 `initialize()`：断言返回 false，`errors()` 含 `E_SYNC_UNSUPPORTED_SCHEMA`，`state()` **未**进入同步态（仍 Idle/未激活），后续 `sync()` 不被受理。
  - 模拟编译时 SESSION 扩展不可用（编译宏关闭或运行时探测失败）：`initialize()` 返回 false 且含 `E_SYNC_SESSION_UNAVAILABLE`；此为整方案硬闸门（FR-1 无降级），不进同步。
  - 反例：`seed(01_rowid_explicit_pk.sql)` 合格表 `initialize()` 返回 true、进入同步态。
- **关联需求**：S-02/R-04 eligibility 前置；`E_SYNC_UNSUPPORTED_SCHEMA`、`E_SYNC_SESSION_UNAVAILABLE`；阶段 0 闸门。

#### 11.1.8 tst_sync_write_blocked

- **目的**：验证 sync-aware 写边界：同步激活后，旧 `DataBridge::importExcel` 对**同步表**统一返回 `E_SYNC_WRITE_BLOCKED`（M1 选拒绝、不改道）；非同步表不受限。
- **前置**：库含同步表 `t_sync`（已 initialize）与普通表 `t_plain`（未纳入同步）。
- **步骤 / 断言**：
  - 对 `t_sync` 调 `importExcel`：断言返回失败、`errors()`/返回错误含 `E_SYNC_WRITE_BLOCKED`；该表 `.db` 实际行数不变（旧路径未写入），同步表写只能经 `wconn`。
  - 对 `t_plain` 调 `importExcel`：断言成功写入（非同步表不受限）。
  - `db_` 句柄对 `t_sync` 只读探测：直接经 `db_` 写 `t_sync` 被拒/不可达（只读边界）。
- **关联需求**：T1.13 sync-aware 写边界；`E_SYNC_WRITE_BLOCKED`；Q-08（改道留 M3）。

#### 11.1.9 tst_sync_context_single_writer

- **目的**：验证 G-07：路径别名（symlink / 相对路径 / URI）指向同一物理库时，注册表只建**单个 `SyncContext` + 单 `wconn`**（key 加固按 OS 文件标识 (dev,inode)/Windows FileIndex）。
- **前置**：一个临时 `.db` 文件；构造其 symlink、相对路径、`file:` URI 三种别名。
- **步骤 / 断言**：
  - 用四种路径（原路径 + 三别名）分别调 `createSyncEngine`/`initialize`：断言四者解析到**同一** `SyncContext`（同一 `context_uuid`/registry 句柄），底层 `wconn` 唯一（指针/标识相同），refcount 计为 4。
  - 经任一别名写入，经另一别名读取可见（同库单写线程，写串行无双写竞争）。
  - 逐一 `stop`/释放：refcount 递减到 0 才真正销毁 context（不提前关闭仍被引用的 wconn）。
  - 反例：另一个**不同**物理库的路径解析到独立 context（不误并）。
- **关联需求**：G-07/R-06 SyncContext key 加固；唯一写线程不变量。

### 11.2 M2 — 星型 + 上行选择性推送

M2 套件运行在 `StarHarness`（center C + edge C/D，center 记为 `Center`，边节点 B/D；`UpsertExecutor` 已提取，分支 B selectionpush 接入），验证多源收敛、防回声、权威下行豁免、上行选择性推送闭环、分片续传幂等、rebase 失败回滚。

#### 11.2.1 tst_sync_star_broadcast

- **目的**：验证星型广播：center + 边 B/D，多源两序同终态；防回声（静默后 0 载荷，C2）；权威下行豁免（Edge 配 `TargetWins/Manual` 仍收敛中心终态，F-04）；B/D 跨批 / 低 rank 后到 / 反序仍按逐行胜者收敛。
- **前置**：`StarHarness`（Center + B + D，rank `{Center:10, B:6, D:3}`，所有边经 Center 中转，边间不直连）；B、D 对部分相交主键各产生更新。
- **步骤 / 断言**：
  - **多源两序同终态**：以两种不同 deliver 顺序（B 先 / D 先）分别跑完 `runUntilQuiescent`，断言两次最终 Center、B、D 三库 `row_winner` 三元组**逐字节相同**（到达序无关）。
  - **防回声（C2）**：收敛后空跑一轮，断言 Center 不向已 ack 到对应水位的 peer 重发（`RoutingTable.shouldRoute` 判 `origin==peer` 或 `seq<=acked` 为假），各 outbox 新增制品数 = 0。
  - **权威下行豁免（F-04）**：B 配 `TargetWins`、D 配 `Manual` 冲突策略；Center 广播权威终值后，断言 B、D **仍**收敛到 Center 终态（权威下行豁免本地策略，不被 TargetWins/Manual 拦截），业务行=Center 值。
  - **逐行胜者**：构造 D（低 rank）的更新跨批、反序到达 Center 与 B，断言终态仍=高 rank 值（B 或 Center），与到达序无关。
- **关联需求**：F-04 权威下行豁免、C2 防回声、FR-6/G-01；M2 DoD 星型无回声 + 多源两序同终态。

#### 11.2.2 tst_sync_selective_push

- **目的**：验证 `syncSelected` 上行选择性推送全链路：选择 → FK 闭包 → 剪枝 → 冻结清单 → 拓扑序分片 → 中心逐行 UPSERT（直选 `DoUpdate` / 依赖 `DoNothing`）→ PushChunkAck → 全片 done 才前台 `Completed`；中心半截不外泄（`push_progress != done` 不广播本 push 的直选变更）。
- **前置**：`StarHarness`；Edge `seed(08_fk_chain.sql)`（`orders`→`order_items` FK 链）；选择 `orders` 若干行（直选），其 `order_items` 为 FK 依赖。
- **步骤 / 断言**：
  1. Edge 调 `syncSelected({orders: 选中行})`：断言计算出 FK 闭包含被引用的 `order_items`（或父行），剪枝掉未选关联，冻结清单（快照选中行集）。
  2. 拓扑序分片（父先于子）发往 Center；Center 分支 B selectionpush apply：断言直选行经 `DoUpdate`（覆盖）、依赖行经 `DoNothing`（仅补全引用，不覆盖中心既有）。
  3. 每片回 `PushChunkAck`；断言只有**全片** done 后前台 `state()` 才转 `Completed`、`progress().percent` 达 100；中途某片未 ack 时 percent < 100、state=Exporting。
  4. **半截不外泄**：在 push 仅完成部分片（`push_progress != done`）时触发 Center 广播：断言 Center **不**把本 push 的直选变更广播给其他 edge（避免半截选择外泄），仅在 done 后纳入广播。
- **关联需求**：上行选择性推送（FK 闭包/剪枝/拓扑分片/直选 DoUpdate-依赖 DoNothing）；`push_progress` done 闸门；M2 DoD 上行闭环。

#### 11.2.3 tst_sync_chunk_resume

- **目的**：验证分片中断后续传幂等与受理前/后失败分流：重复 chunk 同 checksum no-op、异 checksum `E_SYNC_PAYLOAD_CORRUPT`；空选择 `E_SYNC_SELECTION_EMPTY`；FK 环 `E_SYNC_FK_CYCLE_UNSUPPORTED`；超规模 `E_SYNC_SELECTION_TOO_LARGE`；受理前校验经返回值、受理后失败落 `errors()`/`state()==Failed`。
- **前置**：`StarHarness`；`push_chunk_progress` 幂等键含 `(origin,epoch,pushId,chunkSeq)`（R-07）；准备超规模选择集与 `seed(09_fk_cycle.sql)`。
- **步骤 / 断言**：
  - **续传幂等**：发 chunk#1#2#3，在 #2 后模拟中断（不 deliver #3），重发 #2#3。断言 #2 因 `push_chunk_progress` 已记同 checksum → no-op（不重复 apply，业务行无重复）；#3 正常续传；最终全片 done。
  - **异 checksum**：重发 #2 但篡改 payload（checksum 不符）。断言 apply 拒收、`errors()` 含 `E_SYNC_PAYLOAD_CORRUPT`、`state()==Failed`。
  - **空选择**（受理前）：`syncSelected({})` 调用即经返回值返回 false + `E_SYNC_SELECTION_EMPTY`，不进入后台、`state()` 不变。
  - **FK 环**（受理前）：选择含 `09_fk_cycle.sql` 表，闭包拓扑排序无解 → 返回值 `E_SYNC_FK_CYCLE_UNSUPPORTED`。
  - **超规模**（受理前）：选择行数/字节超阈值 → 返回值 `E_SYNC_SELECTION_TOO_LARGE`。
  - **分流断言**：受理前失败（空/非法）经**同步返回值**暴露、`errors()` 不增、`state()` 不变；受理后失败（如异 checksum、apply 约束）落 `errors()` 且 `state()==Failed`。
- **关联需求**：分片续传幂等（R-07）；`E_SYNC_PAYLOAD_CORRUPT`/`E_SYNC_SELECTION_EMPTY`/`E_SYNC_FK_CYCLE_UNSUPPORTED`/`E_SYNC_SELECTION_TOO_LARGE`；受理前后失败语义分流。

#### 11.2.4 tst_sync_rebase

- **目的**：验证 rebase 失败处置：注入 `RebaseEngine` 失败 → `E_SYNC_REBASE_FAILED`，本轮不外发、回滚（不污染本地态）。
- **前置**：`StarHarness`；Center 在转发前需对并发本地变更做 rebase；注入 `RebaseEngine::rebase` 返回 false（损坏 rebaseBuffer 或 stub 失败）。
- **步骤 / 断言**：
  1. Center 应用入站后准备转发，rebase 阶段注入失败。
  2. 断言：本轮转发**未发生**（对端 outbox 无新制品）；`errors()` 含 `E_SYNC_REBASE_FAILED`；Center 本地已提交的 apply 不被回退（rebase 失败仅影响外发，本地态不变），但本轮外发缓冲回滚清空。
  3. 解除注入后重跑一轮：断言可正常 rebase 并外发，最终收敛（失败可恢复，非永久卡死）。
- **关联需求**：`E_SYNC_REBASE_FAILED`；M2 DoD rebaser 失败本轮不外发。

### 11.3 M3 — 批量门面

#### 11.3.1 tst_batch_transfer_e2e

- **目的**：验证 M3 批量门面：非阻塞 `startImport`/`startExport` + 轮询；同库 `E_BUSY` 互斥；既有 `importExcel`/`exportExcel` 回归全绿；导入经 `wconn` 维护 `table_state`。
- **前置**：`SyncEngine` + `BatchTransfer` 门面（T3.1）；普通库 + Excel 测试数据集。
- **步骤 / 断言**：
  - **非阻塞 + 轮询**：调 `startImport(spec)` 立即返回（不阻塞）；轮询 `importState()` 经历 Running→Completed；导入完成后业务行落库。
  - **同库 E_BUSY 互斥**：一个 `startImport` 进行中再发起同库 `startExport`/`startImport`：断言第二次返回 `E_BUSY`（互斥），第一次正常完成后第二次可成功。
  - **既有 API 回归**：直接调旧 `importExcel`/`exportExcel`（非同步表）：断言行为与基线一致（multi-table、lookup、fkInject、行级 skip、`writtenRows`、rollback、`DO NOTHING`、bind 序——回归全绿，Q-03）。
  - **table_state 维护**：经批量导入路径写入后，断言 `__sync_table_state` 三元组被增量维护（导入经 `wconn`，为 M4 DiffEngine 零全量前提）。
- **关联需求**：M3 DoD 非阻塞 + `E_BUSY` + 回归 + table_state 维护。

### 11.4 M4 — 场景2 比对/合并

#### 11.4.1 tst_comparison_e2e

- **目的**：验证场景2 比对/合并端到端：表级零全量（判等三元组 G-06）；行级 + keyset 分页 `fetchRemoteRows`；会话期被比对表 inbox 暂停并按到达序放行；`acceptLocal`/`acceptRemote`/`stageCell` → `save` 为普通本地写；save 前 `.db` 写=0；`data_version` 变动 → `E_SYNC_STAGE_STALE`。
- **前置**：`TwoNodeHarness` + `DiffEngine`（T4.1）+ `StagingBuffer`（T4.3）；两库同 schema、部分行不同。
- **步骤 / 断言**：
  - **表级零全量**：调表级 diff，断言 Identical 判定 = `schema_fingerprint + row_count + content_checksum` 三元组；构造「内容相同但 `high_water_seq` 不同」→ 仍判 green（`high_water` 不参与判等）；「checksum 不同但 high_water 相同」→ 判 red；SELECT 行数有上界（不全量物化）。
  - **行级 + 分页**：对 red 表行级 diff，`fetchRemoteRows` 用 keyset 分页只物化受影响行；断言分页连续无重无漏。
  - **会话期 inbox 暂停**：比对会话期间对被比对表的入站制品被暂停（不立即 apply），断言暂停队列累积；会话结束后按**到达序**放行应用（顺序保持）。
  - **staging 为普通本地写**：`acceptLocal`/`acceptRemote`/`stageCell` 仅写 staging，断言 `save` 前目标 `.db` 文件实际写次数 = 0（暂存不落库）；`save` 后经 `UpsertExecutor`（第三路，共用，Q-07）作普通本地写落库。
  - **脚下变动**：staging 期间底层 `data_version` 变动（带外写）→ `save` 检出 → 返回 `E_SYNC_STAGE_STALE`，不覆盖（陈旧拒绝）。
- **关联需求**：判等三元组 G-06/R-08；keyset 分页；会话暂停按序放行；`E_SYNC_STAGE_STALE`；M4 DoD。

### 11.5 M5 — 加固

#### 11.5.1 tst_sync_baseline_e2e

- **目的**：验证基线（baseline）四触发与应用语义：冷启动 / 缺口 / 迁移后 / 强制 → 基线；应用后重置 `applied_vector`/`table_state`/`row_winner` 并喂养 cache；导出/应用失败 → `E_SYNC_BASELINE_FAILED` 不动旧锚点。
- **前置**：`TwoNodeHarness`；可触发四类 baseline 的钩子（冷启动无锚点、`E_SYNC_GAP` 衔接、迁移完成、强制 API）。
- **步骤 / 断言**：
  - 四触发各跑一次 baseline：断言生成全量锚点制品、对端应用后 `applied_vector`/`table_state`/`row_winner` 被**重置**为基线态并喂养 cache（后续增量从基线高水位继续）；与 §11.1.2④ 缺口超时衔接（`E_SYNC_GAP`→baseline 回退）。
  - **失败不动锚点**：注入基线导出失败 / 应用失败 → `errors()` 含 `E_SYNC_BASELINE_FAILED`；断言旧基线锚点**未被覆盖**（仍可回退到旧锚点，不进入半截基线态）；超大基线伴 `W_SYNC_BASELINE_LARGE` 软告警。
- **关联需求**：baseline 四触发；`E_SYNC_BASELINE_FAILED`；`W_SYNC_BASELINE_LARGE`。

#### 11.5.2 tst_sync_schema_quarantine

- **目的**：验证 schema 隔离：版本/指纹不符 → 隔离（quarantine）+ `E_SYNC_SCHEMA_MISMATCH`；迁移后重放隔离制品。
- **前置**：`TwoNodeHarness` + `SchemaGuard`（T5.2）+ `QuarantineStore`；B 制品携带与 A 不符的 `schema_fingerprint`。
- **步骤 / 断言**：
  - A `drain` 收到指纹不符制品：断言 `SchemaGuard` 拒绝 apply、制品入 `QuarantineStore`、后台状态机进入 `Quarantined`、`errors()` 含 `E_SYNC_SCHEMA_MISMATCH`；该制品**不**丢弃（隔离保留）。
  - A 执行迁移使 schema 指纹匹配后触发重放：断言隔离制品被重新 apply 成功、状态机退出 Quarantined、最终收敛。
- **关联需求**：`E_SYNC_SCHEMA_MISMATCH`；后台 Quarantined 态；迁移后重放。

#### 11.5.3 tst_sync_dead_peer

- **目的**：验证死 peer 三维阈值处置：软告警 `W_SYNC_PEER_LAGGING` → 硬逐出 `E_SYNC_PEER_DEAD` + outbox 坍缩 + 截断恢复（min 重算）。
- **前置**：`StarHarness`；`DeadPeerEvictor` 注入 `FakeClock`；一个 edge 停止回 ACK（模拟离线）。
- **步骤 / 断言**：
  - 推进时间越过**软**阈值（滞后维度之一）：断言 `errors()`/告警含 `W_SYNC_PEER_LAGGING`，peer 仍在册、outbox 继续保留其制品。
  - 继续推进越过**硬**阈值（三维：时间/积压字节/积压条数任一或组合达标）：断言 `E_SYNC_PEER_DEAD`、该 peer 被逐出；其专属 outbox 积压被**坍缩**（清理只为该死 peer 保留的制品）。
  - **截断恢复**：逐出后剩余存活 peer 的最小已确认水位（`min` 重算）作为新的 outbox 截断点，断言可安全截断已被所有存活 peer 确认的 changelog 段而不影响存活 peer 续传。
- **关联需求**：`W_SYNC_PEER_LAGGING`/`E_SYNC_PEER_DEAD`；三维阈值；outbox 坍缩 + min 重算截断（R5）。

#### 11.5.4 tst_sync_migration

- **目的**：验证迁移三路径：① 静默窗排空在途推送后才 DDL；② 在途 push 被 `stop` 取消 → 已落片由 re-baseline 整表收口（无 staging 残留）；③ 押旧 schema 的片迁移后到达 → 按 `push_id` 整发拒收 `E_SYNC_PUSH_SCHEMA_MOVED`。
- **前置**：`StarHarness`；迁移协调器（静默窗 + stop 取消，R-10）；构造在途 push。
- **步骤 / 断言**：
  - **① 静默窗排空**：发起迁移，断言协调器先进入静默窗、等在途推送全部排空（outbox/inbox 在途为 0）后才执行 DDL；DDL 期间无新 apply 穿插。
  - **② stop 取消收口**：在途 push 进行中调 `stop`，断言已落的部分片由 re-baseline **整表收口**（基线覆盖该表）、不留 staging 半截；收口后该表收敛、无悬挂 push_progress。
  - **③ 旧 schema 片迟到**：迁移完成后，押**旧** `schema_fingerprint` 的同一 `push_id` 分片到达：断言按 `push_id` **整发拒收**（不部分应用）、`errors()` 含 `E_SYNC_PUSH_SCHEMA_MOVED`，该 push 不污染新 schema 态。
- **关联需求**：迁移三路径（R-10/G-02）；`E_SYNC_PUSH_SCHEMA_MOVED`；静默窗 + stop 取消 + re-baseline 收口。

#### 11.5.5 tst_sync_untracked_change

- **目的**：验证带外写检出：绕过 `wconn` 的带外写 → `data_version` 检出 → `W_SYNC_UNTRACKED_CHANGE` + re-baseline。
- **前置**：`TwoNodeHarness`；一条直接经 `db_`/外部连接写同步表的带外写（不经 changelog 捕获）。
- **步骤 / 断言**：
  - 执行带外写后推进 worker 一轮：断言 worker 通过 `PRAGMA data_version` 比对检出未跟踪变更，发 `W_SYNC_UNTRACKED_CHANGE` 软告警。
  - 断言随即触发该表 re-baseline（带外变更无 changelog 无法增量同步，须整表收口）；re-baseline 后两库重新收敛，带外写的内容被纳入基线传播。
- **关联需求**：`W_SYNC_UNTRACKED_CHANGE`；`data_version` 检出；带外写 → re-baseline。

#### 11.5.6 tst_sync_byte_budget

- **目的**：验证 NFR-5 载荷字节预算：单/批载荷 `bytesPacked`/`bytesApplied` ≤ 2Mbps 预算（**只承诺字节量预算，不承诺黑盒在途时延**）。
- **前置**：`TwoNodeHarness`；可观测计数器 `bytesPacked`/`bytesApplied`（经 `progress()`/counters）；构造单笔与批量负载。
- **步骤 / 断言**：
  - 跑单笔与批量同步各一轮，读 `bytesPacked`（出站打包字节）与 `bytesApplied`（入站应用字节）：断言在给定窗口内累计字节量不超过 2Mbps 预算换算的字节上限（按 §3.6 时钟窗口折算）。
  - 断言超阈值时发 `W_SYNC_PAYLOAD_LARGE` 软告警（大载荷预警），但**不**对在途时延做断言（黑盒传输时延不在承诺范围）。
- **关联需求**：NFR-5 字节预算；`bytesPacked`/`bytesApplied` counters；`W_SYNC_PAYLOAD_LARGE`。

### 11.6 集成测试与覆盖率

上述 21 个集成场景套件主要覆盖 **`SyncWorker`（约 2106 行）** 与 **`SyncEngine`（约 373 行）** 等编排代码——这正是单元测试难以触达、覆盖率风险最大的部分：`SyncWorker` 的主循环（三时机扫描调度、drain/deliver 推进、缺口判定与 pending 续判、ACK 攒批与超时、rebase 与转发路由、迁移静默窗与 stop 取消、死 peer 逐出与 outbox 截断）和 `SyncEngine` 的双状态机（前台 `Exporting→Completed/Failed`、后台 `Watching/Applying/Acking/Broadcasting/Quarantined`）依赖**多组件协同 + 真实库 + 真实制品流转**，无法用单组件单测有效驱动，只能由 `TwoNodeHarness`/`StarHarness`/`CrashHarness` 端到端用例覆盖其状态迁移与分支组合。

集成测试覆盖目标：使 `SyncWorker`/`SyncEngine` 等编排代码的**行覆盖 ≈ 72%**。未达 100% 的缺口主要落在：跨进程崩溃注入仅四个 `DBRIDGE_FAULT_POINT`（其余路径间隙不可稳定杀入）、`Q_OS_UNIX` 门控的 durability 分支、OOM/极端 IO 失败等不可在 CI 稳定注入的路径——这些与第 8 节 transport ≈82% 缺口同源，作「尽力覆盖」计入预算。所有时间相关推进经 `FakeClock`（§3.6），所有制品流转经 `TwoNodeHarness.drain/deliver` 手动驱动，套件确定可重放、无真实 `sleep`。
## 12. 错误码触发矩阵、需求追溯与退出准则

本节是测试计划的「收口章」：把第 4–11 节定义的全部单元套件（`tst_<area>`）与集成套件（`tst_sync_*`）回接到**错误码**、**需求条目**（FR/NFR/共识 C/设计评审不变量 G）与**可量化退出准则**上，并给出 CI 闸门、与里程碑对齐的实施路线、以及风险与缓解。原则：**每个错误码至少一个码级断言**（`ASSERT_ERR_CODE`，§3.7），**每条 FR/关键不变量至少一个验证套件**，**退出准则全部可机器判定**（ctest 绿 + gcovr 阈值，不靠人工审查）。

> 套件命名约定：单元套件取自第 4–11 节的 `tst_<area>`（如 `tst_changeset_applier`）；集成套件取本节 `tst_sync_<scenario>` 命名（如 `tst_sync_strict_sequential`）。§1.4 另以 `tst_apply_crash_window`/`tst_writetxn_single_writer`/`tst_diff_equality_triple`/`tst_selection_strict_contiguity` 标识四个「不变量专用夹具」，本节统一并入对应集成套件（崩溃→`tst_sync_crash_zero_window`、单写者→`tst_sync_context_single_writer`、判等→`tst_sync_comparison_e2e`、严格连续→`tst_sync_strict_sequential`）。

### 12.1 错误码触发矩阵

下表覆盖 `include/dbridge/Errors.h` v0.5 全量码。每行=[错误码, 触发条件/阶段, 事务动作, 状态落点, 验证测试套件]。错误码断言一律走 `ASSERT_ERR_CODE(call, "<code>")`（§3.7）：失败返回 `false` 且 `*err` 以该码前缀冠首（实现需把现有英文文案统一加码前缀，满足 O3）。「事务动作」列说明该码触发时 `WriteTxn`/apply 单元的回滚语义（R2 单 apply 单元原子，C13）。

#### 12.1.1 Error / Fatal 级错误码（23 个）

| 错误码 | 触发条件 / 阶段 | 事务动作 | 状态落点 | 验证测试套件（码级断言） |
| --- | --- | --- | --- | --- |
| `E_SYNC_INIT` | 阶段0/`initialize`：`wconn` 打开失败、DDL 建表失败、`SyncContext` 注册失败 | 不进入同步模式 | engine state=未初始化；error 环记码 | `tst_phase0`、`tst_sync_context`、`tst_sync_migration` |
| `E_SYNC_SESSION_UNAVAILABLE` | 阶段0闸门：SQLite 缺 `SQLITE_ENABLE_SESSION`/`sessionAvailable(h)==false`，在 attach 前探测 | 整方案停（无降级，FR-1） | phase0 闸门失败→全停 | `tst_phase0`、`tst_session_recorder` |
| `E_SYNC_SCHEMA_MISMATCH` | apply 前置：载荷声明指纹 ≠ 接收端表指纹（FR-7），不进入冲突回调 | apply 单元回滚 | quarantine 入隔离 | `tst_schema_guard`、`tst_changeset_applier`、`tst_sync_schema_quarantine` |
| `E_SYNC_PAYLOAD_CORRUPT` | `decode`：坏 magic / 不支持版本 / 头体读失败 / 解压失败；重复 chunk 异 checksum（R-07） | apply 单元回滚/拒收 | inbox ledger 标 `corrupt`；quarantine | `tst_payload_codec`、`tst_chunk_streamer`、`tst_sync_chunk_resume` |
| `E_SYNC_TRANSPORT` | outbox 发布失败：tmp 打开/partial write/`flush`/`fsync`/`rename`/`.ready`/dir-fsync 失败 | 不发布，cleanup 残留（M-08） | outbox 无半可见制品；ACK 保留待重试 | `tst_outbox_writer`、`tst_ack_channel`、`tst_sync_two_node_converge` |
| `E_SYNC_APPLY_FK` | `apply_v2` 冲突回调：外键约束违反（孤儿/悬挂父） | apply 单元回滚 | applied_vector 不推进 | `tst_changeset_applier`、`tst_fk_closure_builder`、`tst_sync_selective_push` |
| `E_SYNC_APPLY_CONSTRAINT` | `apply_v2` 冲突回调：NOT NULL/UNIQUE/CHECK 等非 FK 约束违反 | apply 单元回滚 | applied_vector 不推进 | `tst_changeset_applier`、`tst_upsert_executor` |
| `E_SYNC_NODE_UNKNOWN` | 路由/ACK：来源/目标 peer 不在 `RoutingTable` 配置内 | 拒绝路由 | — | `tst_routing_table`、`tst_sync_star_broadcast` |
| `E_SYNC_GAP` | 严格连续：`seq>applied_seq+1` 缺口且超 `gapTimeout`（M-01） | 不应用、不 ACK；回退基线 | inbox ledger 保 `seen`(pending)；触发 baseline | `tst_applied_vector`、`tst_inbox_ledger`、`tst_sync_strict_sequential` |
| `E_SYNC_STAGE_STALE` | 场景2：暂存合并期间「脚下」表被改（base 版本漂移） | 合并放弃 | staging buffer 失效 | `tst_staging_buffer`、`tst_comparison_session`、`tst_sync_comparison_e2e` |
| `E_SYNC_STAGE_CONFLICT` | 场景2：暂存合并写回与并发裁决冲突 | 合并放弃 | staging buffer 保留待重试 | `tst_staging_buffer`、`tst_sync_comparison_e2e` |
| `E_SYNC_PEER_DEAD` | 死对端：peer 超无响应阈值被驱逐 | outbox 坍缩为单基线 | routing 摘除该 peer | `tst_dead_peer_evictor`、`tst_sync_dead_peer` |
| `E_SYNC_SELECTION_EMPTY` | 上行：人工选择集合为空（FR-17） | 不出箱 | — | `tst_sync_selection_builder`、`tst_selection_resolver`、`tst_sync_selective_push` |
| `E_SYNC_FK_CLOSURE_MISSING` | 上行闭包：依赖父行不可解析/缺失 | 不出箱 | — | `tst_fk_closure_builder`、`tst_sync_selective_push` |
| `E_SYNC_FK_CYCLE_UNSUPPORTED` | 上行闭包：检测到 FK 环，拓扑序不可建 | 不出箱 | — | `tst_fk_closure_builder` |
| `E_SYNC_SELECTION_TOO_LARGE` | 上行：选择集合/闭包超规模上界 | 不出箱 | — | `tst_selection_resolver`、`tst_sync_selection_builder` |
| `E_SYNC_PUSH_SCHEMA_MOVED` | 上行分片应用：推送期间中心 schema 迁移使冻结清单失效 | 分片放弃 | frozen_manifest 失效 | `tst_frozen_manifest`、`tst_selection_push_applier`、`tst_sync_chunk_resume` |
| `E_BUSY` | 前台门控：同库已有前台活动（`syncSelected`/import/export 互斥，FR-11） | 拒绝并发前台 | foreground_gate 持有者不变 | `tst_foreground_gate`、`tst_sync_batch_transfer_e2e` |
| `E_SYNC_WRITE_BLOCKED` | sync-aware 边界：旧 `importExcel` 对同步表写（M1 拒绝，Q-08） | 拒绝写 | 同步表经 `wconn` 写不受影响 | `tst_import_service_regression`、`tst_inbound_table_gate`、`tst_sync_write_blocked` |
| `E_SYNC_UNSUPPORTED_SCHEMA` | eligibility：无 PK 表/视图配为同步表，`initialize` attach 前显式校验（G-04/S-02） | 不进入同步模式 | engine 拒绝初始化该表 | `tst_schema_eligibility`、`tst_sync_eligibility_gate` |
| `E_SYNC_ACK_TIMEOUT` | 前台状态机：`Exporting` 等 ACK 超时（区别于泛 Failed，G-09） | state→`Failed` | error 环记 ACK 超时 | `tst_ack_channel`、`tst_sync_ack_wait` |
| `E_SYNC_REBASE_FAILED` | rebase：`sqlite3rebaser_*` 失败（G-09） | 本轮不外发、回滚 | 锚点不动 | `tst_rebase_engine`、`tst_sync_rebase` |
| `E_SYNC_BASELINE_FAILED` | 基线：导出/应用基线失败（G-09） | 回滚不动旧锚点 | applied_vector/table_state/row_winner 不重置 | `tst_baseline_manager`、`tst_sync_baseline_e2e` |

#### 12.1.2 Warning 级警告码（7 个）

警告码不中断 apply（不回滚），仅写 error/警告环并继续；断言用 `QVERIFY(warningRing.contains("<code>"))` 或专用 warning getter（非 `ASSERT_ERR_CODE`，因调用仍返回 `true`）。

| 警告码 | 触发条件 / 阶段 | 事务动作 | 状态落点 | 验证测试套件 |
| --- | --- | --- | --- | --- |
| `W_SYNC_CONFLICT_REPLACED` | apply：冲突按规范序裁决，胜者 REPLACE 败者 | 继续提交 | 警告环记一条；row_winner 更新 | `tst_conflict_arbiter`、`tst_row_winner`、`tst_sync_row_winner_convergence` |
| `W_SYNC_BASELINE_LARGE` | 基线导出体积超软阈值 | 继续 | 警告环 | `tst_baseline_manager`、`tst_sync_baseline_e2e` |
| `W_SYNC_PAYLOAD_LARGE` | 编码：单载荷体积超软阈值 | 继续（建议分片） | 警告环 | `tst_payload_codec`、`tst_chunk_streamer`、`tst_sync_byte_budget` |
| `W_SYNC_UNTRACKED_CHANGE` | 外部写检测：`data_version`/校验和发现绕过 `wconn` 的写（FR-2） | 继续（触发 re-baseline 评估） | 警告环；标记需基线 | `tst_table_state`、`tst_sync_untracked_change` |
| `W_SYNC_PEER_LAGGING` | ACK 水位：peer 长期落后但未达死亡阈值 | 继续 | 警告环 | `tst_dead_peer_evictor`、`tst_sync_dead_peer` |
| `W_SYNC_PUSH_ROW_DRIFTED` | 上行分片应用：行快照与当前中心值漂移（依赖父 DO NOTHING 命中） | 继续（不覆盖） | 警告环 | `tst_selection_push_applier`、`tst_sync_selective_push` |
| `W_SYNC_CONCURRENT_MANUAL_PUSH` | 上行：检测到并发人工推送同一选择域 | 继续（幂等去重） | 警告环 | `tst_sync_selection_builder`、`tst_sync_selective_push` |

> 错误码覆盖自检：上表 23 个 Error/Fatal + 7 个 Warning = **30 个码，每个码 ≥1 个码级/警告断言**，满足退出准则③。

### 12.2 需求追溯矩阵

#### 12.2.1 功能需求 FR-1..FR-17

| FR | 设计落点 | 验证测试套件 | 断言要点 |
| --- | --- | --- | --- |
| FR-1 | 短命 session 同事务捕获 + changelog 落地（§5.1/5.2） | `tst_session_recorder`、`tst_changelog_store`、`tst_sync_crash_zero_window` | 「业务已提交」与「changelog 已落地」同一 `COMMIT`；崩溃在提交前/seal 后/commit 后被杀，重启后业务+changelog+applied_vector+row_winner+ledger 原子一致、零窗口 |
| FR-2 | 同步表集合 + sync-aware 边界 + 外部写检测（§5.3） | `tst_inbound_table_gate`、`tst_table_state`、`tst_sync_write_blocked`、`tst_sync_untracked_change` | 空集合=全表；同步表写必经 `wconn`；绕过写经 `data_version`/校验和发现→`W_SYNC_UNTRACKED_CHANGE` |
| FR-3 | 双载荷模型 ChangesetPayload / SelectionPushPayload（§5.4） | `tst_payload_codec`、`tst_selection_push_applier` | 公共头(溯源元数据)+类型 body；changeset 走 `apply_v2`、selection-push 走 UPSERT；版本/magic 严格 |
| FR-4 | outbox/inbox 文件通道 + `.ready` 哨兵 + ACK（§5.5） | `tst_outbox_writer`、`tst_inbox_watcher`、`tst_inbox_ledger`、`tst_ack_channel` | 接收端只见完整制品（`.ready` 唯一判据）；ACK 复用同通道、`ackMaxDelayMs` 内必发；dbridge 不做字节级重传 |
| FR-5 | `apply_v2` + 冲突回调 + R2 单 apply 单元原子（§5.6） | `tst_changeset_applier`、`tst_applied_vector`、`tst_sync_two_node_converge` | apply 前指纹校验→`E_SYNC_SCHEMA_MISMATCH`；apply+applied-vector+转发 changelog 同事务；幂等去重 |
| FR-6 | 逐行胜者 `(rank,originSeq,originId)` 规范序（§5.7） | `tst_row_winner`、`tst_conflict_arbiter`、`tst_sync_row_winner_convergence` | 低 rank 跨批后到不翻盘；任意到达序最终态一致（G-01）；权威下行豁免 ConflictPolicy |
| FR-7 | schema 指纹校验 + 隔离（§5.8） | `tst_schema_guard`、`tst_quarantine_store`、`tst_sync_schema_quarantine` | 指纹不一致拒绝并隔离；迁移后重放；DDL 不随通道传播 |
| FR-8 | 增量基线 + changelog 截断/坍缩（§5.9） | `tst_baseline_manager`、`tst_sync_baseline_e2e` | 锚点由 ACK 推进；冷启动/缺口/迁移后/强制→基线；应用后重置 applied-vector/table_state/row_winner |
| FR-9 | 差异比对 / 合并 API（§5.10） | `tst_diff_engine`、`tst_comparison_session`、`tst_sync_comparison_e2e` | 表级零全量（行数有上界）；行级只物化受影响行；下游强制收敛 |
| FR-10 | 暂存合并 + 死对端有界缓冲（§5.10/5.11） | `tst_staging_buffer`、`tst_dead_peer_evictor`、`tst_sync_dead_peer` | 脚下变动→`E_SYNC_STAGE_STALE`；死对端 outbox 坍缩单基线→`E_SYNC_PEER_DEAD` |
| FR-11 | 前台/后台双状态机 + `E_BUSY` 单活动门控（§5.12） | `tst_foreground_gate`、`tst_sync_context`、`tst_sync_ack_wait` | 前台单活动互斥→`E_BUSY`；`Exporting=等ACK/percent=-1`，足额 ACK 才 `Completed` |
| FR-12 | 表级判等三元组（§5.10） | `tst_diff_engine`、`tst_consistency_cache`、`tst_sync_comparison_e2e` | Identical=`schema_fingerprint+row_count+content_checksum`；`high_water_seq` 不参与判等（G-06） |
| FR-13 | 精简导入导出门面（向后兼容，§5.13） | `tst_export_service`、`tst_batch_transfer`、`tst_sync_batch_transfer_e2e` | 非阻塞导入/导出+轮询；同库 `E_BUSY` 互斥；既有 Profile 能力兼容 |
| FR-14 | ACK SLA `ackMaxDelayMs`（§5.5） | `tst_ack_channel`、`tst_sync_ack_wait` | `ackMaxDelayMs` 内必发 ACK（攒批）；超时→`E_SYNC_ACK_TIMEOUT` |
| FR-15 | 状态快照 + 计数器可观测（§5.12） | `tst_sync_context`、`tst_sync_two_node_converge` | 8 getter + `bytesPacked/bytesApplied/changesApplied/conflicts/lastAckedSeq` 可轮询 |
| FR-16 | 同步触发模型（上行人工/下行自动，§5.14） | `tst_sync_selective_push`、`tst_sync_star_broadcast` | 上行人工发起；下行中心固化裁决后自动广播 |
| FR-17 | 上行选择性推送 + FK 闭包 + 分片（§5.15） | `tst_selection_resolver`、`tst_fk_closure_builder`、`tst_selection_push_applier`、`tst_chunk_streamer`、`tst_sync_selective_push`、`tst_sync_chunk_resume` | 人工选择+闭包剪枝+拓扑序 UPSERT；分片续传幂等 `(push_id,chunk_seq)`；空选择/超规模/FK 环/悬挂父报码 |

#### 12.2.2 非功能需求 NFR-1..NFR-9

| NFR | 内容 | 验证测试套件 / 方式 |
| --- | --- | --- |
| NFR-1 | 纯抽象接口 + 工厂可编译替换 | 编译期断言：`ISyncEngine` 纯虚、`createSyncEngine(bridge)` 工厂；测试以 fake 实现替换并编译链接通过（`tst_sync_context` + 编译守护用例） |
| NFR-2 | 跨平台（Linux/Windows/macOS） | CI 矩阵双轨构建（CMake+qmake）多平台跑 ctest；POSIX-only 分支以 `[UNIX]` 标注（§8.1） |
| NFR-3 | 与既有 dbridge ETL 不回归 | `tst_import_service_regression`、`tst_export_service`、`tst_batch_transfer`（既有回归全绿，退出准则⑤） |
| NFR-4 | 函数 ≤150 行、参数 ≤7 | 由 `clang-format` + pre-commit hook 静态校验（非运行时用例）；CI lint 步骤闸门 |
| NFR-5 | 字节预算（2Mbps 为字节非时延） | `tst_sync_byte_budget`：断言单位时间出箱**字节数**上界（≤2Mbps 换算字节预算），用注入时钟控窗口；`W_SYNC_PAYLOAD_LARGE` 软阈值 |
| NFR-6 | 单写者 + getter 线程安全 | `tst_write_txn`、`tst_writetxn_single_writer`（并发写串行化/拒绝，G-07）；getter 在 worker 写时并发读无数据竞争（`tst_sync_context_single_writer`，可 TSAN 构建） |
| NFR-7 | 可观测/可诊断（错误环+审计） | `tst_sync_context`（error 环容量/淘汰）；§11 加固审计日志用例 |
| NFR-8 | 崩溃可恢复（零数据窗口） | `tst_sync_crash_zero_window`（子进程提交点 kill + 重启一致性，FR-1/Q-02） |
| NFR-9 | 幂等/可续做（`sync()` 安全重入） | `tst_applied_vector`、`tst_inbox_ledger`、`tst_chunk_streamer`、`tst_sync_chunk_resume`（重放/续传不丢不重） |

#### 12.2.3 共识 C1..C17（简表）

| 共识 | 要点 | 验证套件 |
| --- | --- | --- |
| C1 | 短命 session 绑单事务 | `tst_session_recorder` |
| C2 | 防回声：不回推来源 | `tst_routing_table`、`tst_sync_origin_not_recast` |
| C3 | 文件通道 + 哨兵 | `tst_outbox_writer`、`tst_inbox_watcher` |
| C4 | ACK 推进锚点 | `tst_outbound_ack`、`tst_ack_channel` |
| C5 | 锚点/applied-vector 分层 | `tst_outbound_ack`、`tst_applied_vector` |
| C6 | 双载荷模型 | `tst_payload_codec` |
| C7 | 规范全序 `(rank,seq,origin)` | `tst_conflict_arbiter` |
| C8 | 权威下行豁免 ConflictPolicy | `tst_changeset_applier`、`tst_sync_star_broadcast` |
| C9 | schema 指纹校验/隔离 | `tst_schema_guard`、`tst_quarantine_store` |
| C10 | 上行人工选择性推送 | `tst_selection_resolver`、`tst_sync_selective_push` |
| C11 | FK 闭包 + 剪枝 + 拓扑序 | `tst_fk_closure_builder` |
| C12 | 上行 UPSERT 不叠 rank（直选 UPDATE/依赖 DO NOTHING） | `tst_upsert_executor`、`tst_selection_push_applier` |
| C13 | R2 原子作用域=单 apply 单元 | `tst_changeset_applier`、`tst_selection_push_applier` |
| C14 | 分片幂等 `(push_id,chunk_seq)` | `tst_chunk_streamer`、`tst_sync_chunk_resume` |
| C15 | 前台 `E_BUSY` 单活动 | `tst_foreground_gate` |
| C16 | 后台常驻不受 `E_BUSY` | `tst_sync_context`、`tst_sync_ack_wait` |
| C17 | 死对端有界缓冲/坍缩 | `tst_dead_peer_evictor`、`tst_sync_dead_peer` |

#### 12.2.4 设计评审不变量 G-01..G-10

| G | 不变量 | 验证套件 / 断言要点 |
| --- | --- | --- |
| G-01 | 逐行胜者到达序无关 | `tst_row_winner`、`tst_sync_row_winner_convergence`：低 rank 跨批反序到达，C/D 仍收敛中心终态 |
| G-02 | 迁移半截可取消/可恢复 | `tst_sync_migration`：迁移中途 `stop`/崩溃，重启无半截状态、可重做（R-10 收口） |
| G-03 | `CapturedWriteTemplate` 三分支 | `tst_captured_write_template`：A 入站 changeset（不 fresh 捕获）/B 选择性推送分片/C 本地写，各分支路径正确 |
| G-04 | 同步表 eligibility | `tst_schema_eligibility`、`tst_sync_eligibility_gate`：无 PK/视图→`E_SYNC_UNSUPPORTED_SCHEMA`，attach 前显式校验 |
| G-05 | 严格连续应用 | `tst_applied_vector`、`tst_sync_strict_sequential`：`seq==applied+1` 才应用、`<=` 幂等、`>+1` 缺口 pending；序号无空洞无跳变 |
| G-06 | 判等三元组 | `tst_diff_engine`、`tst_sync_comparison_e2e`：内容同 high_water 异→green，checksum 异 high_water 同→red |
| G-07 | `SyncContext` key 唯一 | `tst_sync_context`、`tst_sync_context_single_writer`：symlink/hardlink/相对路径/URI 指向同库只建一个 context；单写者 |
| G-08 | transport ledger | `tst_inbox_ledger`、`tst_inbox_watcher`：制品级幂等（seen/consumed/corrupt 四态）；三时机重扫；半截/补扫 |
| G-09 | 三新错误码区分 | `tst_sync_ack_wait`(`E_SYNC_ACK_TIMEOUT`)、`tst_sync_rebase`(`E_SYNC_REBASE_FAILED`)、`tst_sync_baseline_e2e`(`E_SYNC_BASELINE_FAILED`)：各自有专属触发点，不并入泛 Failed |
| G-10 | apply 经现有写通道（不绕底层文件） | `tst_changeset_applier`、`tst_upsert_executor`：apply 走句柄穿透 `sqlite3*`，触发器/约束/changelog 捕获一致 |

### 12.3 测试用例与覆盖率汇总

| 维度 | 统计 | 说明 |
| --- | --- | --- |
| 单元套件数 | **≈ 42** | §4–§9 `tst_<area>`（phase0/sqlite_handle/session_recorder/changelog_store/payload_codec/outbound_ack/applied_vector/row_winner/changeset_applier/upsert_executor/selection_push_applier/captured_write_template/schema_eligibility/table_state/schema_guard/quarantine_store/outbox_writer/inbox_ledger/inbox_watcher/ack_channel/conflict_arbiter/rebase_engine/routing_table/selection_resolver/fk_closure_builder/consistency_cache/frozen_manifest/chunk_streamer/baseline_manager/diff_engine/staging_buffer/inbound_table_gate/comparison_session/dead_peer_evictor/sync_context/foreground_gate/write_txn/sync_config_builder/sync_selection_builder/import_service_regression/export_service/batch_transfer） |
| 集成套件数 | **≈ 21** | §10–§11 `tst_sync_*`（two_node_converge/strict_sequential/row_winner_convergence/origin_not_recast/crash_zero_window/ack_wait/eligibility_gate/write_blocked/context_single_writer/star_broadcast/selective_push/chunk_resume/rebase/batch_transfer_e2e/comparison_e2e/baseline_e2e/schema_quarantine/dead_peer/migration/untracked_change/byte_budget） |
| 套件总数 | **≈ 63** | 单元 42 + 集成 21 |
| 测试函数（slot）估算 | **≈ 400+** | 每套件 6–12 个 slot（§4–§11 实表为准），按均值 ≈8 计：单元 ≈ 42×8 ≈ 336、集成 ≈ 21×7 ≈ 147，合计 **≈ 480 个用例**（保守口径 ≥ 400） |
| 加权行覆盖率目标 | **≈ 83%（≥ 80%）** | 按 §1.5 各目录 SLOC 加权：纯逻辑目录（diff/selection/conflict）≈90–95%、IO/平台门控目录（transport）≈82%、SyncWorker 大文件靠集成拉至 ≈75%，整体加权 ≈ 83% |
| 分支覆盖率目标 | **≥ 70%** | gcovr branch 闸门 |

### 12.4 退出准则（Exit Criteria）

全部为机器可判定项，**不靠人工代码审查**：

| # | 准则 | 判定方式（命令/闸门） |
| --- | --- | --- |
| ① | 所有套件 ctest 全绿 | `ctest --output-on-failure` 退出码 0；qmake 路径 `make check` 全过 |
| ② | line ≥ 80% 且 branch ≥ 70% | `gcovr -r . --object-directory build-cov --fail-under-line 80 --fail-under-branch 70`（非零退出即 CI 失败，§2 闸门） |
| ③ | 全部错误码有码级断言 | §12.1 矩阵 30 码逐一映射到 `ASSERT_ERR_CODE`/warning 断言；CI 以 grep 校验每个码常量在 `tst_*` 中至少出现一次 |
| ④ | 关键不变量集成可断言 | FR-1(`tst_sync_crash_zero_window`)、FR-6(`tst_sync_row_winner_convergence`)、G-05(`tst_sync_strict_sequential`)、G-06(`tst_sync_comparison_e2e`)、G-07(`tst_sync_context_single_writer`) 均绿 |
| ⑤ | 既有 ETL 回归全绿 | `tst_import_service_regression`、`tst_export_service`、`tst_batch_transfer` 全过（NFR-3 守护提取，先红后绿） |
| ⑥ | 阶段0闸门通过 | `tst_phase0` 全绿：`E_SYNC_SESSION_UNAVAILABLE`/句柄穿透/rebaser 探测均通过；**否则全停**（FR-1 无降级），不进入 M1 |

> 闸门顺序：⑥（阶段0）→ ① → ② → ③/④/⑤ 并行核验。任一未达即视为未达退出准则，禁止合并/发布。

### 12.5 CI 集成

#### 12.5.1 流水线步骤

| 步 | 阶段 | 命令 / 动作 | 失败动作 |
| --- | --- | --- | --- |
| 1 | 构建 | CMake：`cmake -DCMAKE_BUILD_TYPE=Coverage -B build-cov && cmake --build build-cov`；qmake 路径影子构建 `libdbridge.a`/`libQXlsx.a` 先行 | 编译/链接失败即红 |
| 2 | 测试 | `ctest --test-dir build-cov --output-on-failure`（产出 `.gcda`） | 任一套件失败即红（退出准则①） |
| 3 | 覆盖率采集 | `lcov --capture --directory build-cov --output-file cov.info` + `lcov --remove`（过滤 `tests/`、`QXlsx/`、`/usr/`、Qt moc） | — |
| 4 | 阈值闸门 | `gcovr -r . --object-directory build-cov --fail-under-line 80 --fail-under-branch 70 --xml coverage.xml --html-details coverage.html` | 非零退出即红（退出准则②） |
| 5 | 错误码自检 | 脚本 grep `include/dbridge/Errors.h` 全部 `E_SYNC_*`/`W_SYNC_*` 常量，校验每个在 `tests/**/tst_*.cpp` 至少出现一次 | 缺失即红（退出准则③） |
| 6 | 报告归档 | 上传 `coverage.xml`（CI 插件解析趋势）+ `coverage.html`（人工审阅产物） | — |

#### 12.5.2 新套件三处登记的 CI 校验

新增 `tst_<area>`/`tst_sync_*` 必须在三处同步登记（§2.3），CI 增设一致性校验步骤，任一缺失即红：

| 登记处 | 内容 | CI 校验 |
| --- | --- | --- |
| `tests/CMakeLists.txt` | `add_test` / 子目录 + 链接 `dbridge` | 校验：每个 `tst_*` 目录有对应 `add_test` 条目 |
| `tests/tests.pro` | `SUBDIRS` 追加 + `.file` 映射各一行 | 校验：`SUBDIRS` 项数 == 套件目录数 |
| `tests/test-common.pri` | 公共依赖（`PRE_TARGETDEPS` 静态库、`c++17`、coverage flags） | 校验：新套件 `.pro` `include(../test-common.pri)` 存在 |

> 三处登记不一致是「套件被静默漏跑」的最常见根因——CI 以脚本比对三处套件清单交集，差集非空即失败。

### 12.6 测试实施路线（与里程碑对齐）

按 `specs/SQLite-同步工具-plan.md` M0→M5，每里程碑「必须随附的测试套件」清单。统一遵循**先红后绿**（DoD 必须有可断言夹具，不靠审查）：

| 里程碑 | 范围 | 必须随附测试套件 | DoD 可断言点 |
| --- | --- | --- | --- |
| **M0** 阶段0闸门 | session/句柄/rebaser 探测 + row_winner 原型 | `tst_phase0`、`tst_sqlite_handle`、`tst_session_recorder`（基线）、`tst_rebase_engine`（探测） | `E_SYNC_SESSION_UNAVAILABLE` 触发；句柄穿透 `sqlite3*` 可用；不可行→全停（退出准则⑥） |
| **M1a** 基础/DDL/写线程/key/eligibility | `Errors.h` 全量码、`SqliteHandle`/`WriteTxn`/`ForegroundGate`、`SyncContext` key 加固 | `tst_write_txn`、`tst_writetxn_single_writer`、`tst_foreground_gate`、`tst_sync_context`、`tst_schema_eligibility`、`tst_sync_eligibility_gate`、`tst_sync_config_builder`、`tst_sync_selection_builder` | 写串行入队；路径别名同库单 context（G-07）；无 PK/视图→`E_SYNC_UNSUPPORTED_SCHEMA`（G-04）；崩溃夹具可注入点杀进程 |
| **M1b-1** 捕获/changelog/载荷/台账/表态/schema | session 录制、changelog、codec、inbox ledger、table_state、schema 基件 | `tst_session_recorder`、`tst_changelog_store`、`tst_payload_codec`、`tst_outbox_writer`、`tst_inbox_ledger`、`tst_inbox_watcher`、`tst_table_state`、`tst_schema_guard`、`tst_quarantine_store`、`tst_outbound_ack` | 同事务捕获（FR-1）；制品幂等四态（G-08）；指纹拒绝→隔离 |
| **M1b-2** 三分支 apply + 逐行胜者 + 严格连续 | `CapturedWriteTemplate` 分支 A、applied-vector、row_winner、applier、upsert | `tst_captured_write_template`、`tst_applied_vector`、`tst_row_winner`、`tst_changeset_applier`、`tst_upsert_executor`、`tst_conflict_arbiter`、`tst_routing_table` | 严格连续（G-05）；origin 不重铸；逐行胜者裁决（G-01）；`E_SYNC_APPLY_FK/CONSTRAINT/SCHEMA_MISMATCH/GAP` |
| **M1b-3** 崩溃/缺口/乱序夹具 | 集成不变量夹具 | `tst_sync_two_node_converge`、`tst_sync_strict_sequential`、`tst_sync_row_winner_convergence`、`tst_sync_origin_not_recast`、`tst_sync_crash_zero_window` | 双向收敛；乱序/缺口可测（`E_SYNC_GAP`）；崩溃零窗口跨重启一致（FR-1/Q-02） |
| **M1c** 门面/状态机/观测/旧写收口 | `ISyncEngine`+工厂、双状态机、sync-aware 边界 | `tst_sync_ack_wait`、`tst_sync_write_blocked`、`tst_import_service_regression`、`tst_inbound_table_gate` | `Exporting=等ACK`+超时`E_SYNC_ACK_TIMEOUT`（G-09）；旧 importExcel→`E_SYNC_WRITE_BLOCKED`；8 getter 可轮询 |
| **M2** 星型+上行 | UpsertExecutor 提取、广播、选择性推送、分片、rebase | `tst_selection_resolver`、`tst_fk_closure_builder`、`tst_selection_push_applier`、`tst_chunk_streamer`、`tst_frozen_manifest`、`tst_sync_star_broadcast`、`tst_sync_selective_push`、`tst_sync_chunk_resume`、`tst_sync_rebase`、`tst_export_service` | 星型无回声多源同终态；闭包剪枝拓扑序 UPSERT；分片续传幂等；`rebaser` 失败`E_SYNC_REBASE_FAILED`；空选择/FK 环/超规模报码 |
| **M3** 批量门面 | 非阻塞导入导出 + 旧 API 改道 | `tst_batch_transfer`、`tst_sync_batch_transfer_e2e` | 非阻塞+轮询；同库 `E_BUSY` 互斥；importExcel/exportExcel 回归全绿；table_state 维护 |
| **M4** 场景2 对比/合并 | DiffEngine、暂存、对比会话 | `tst_diff_engine`、`tst_staging_buffer`、`tst_comparison_session`、`tst_consistency_cache`、`tst_sync_comparison_e2e` | 表级零全量；判等三元组 green/red（G-06）；脚下变动`E_SYNC_STAGE_STALE`；第三路 save 共用 UpsertExecutor |
| **M5** 加固 | BaselineManager、SchemaGuard 完整化、死对端、迁移、错误码全覆盖 | `tst_baseline_manager`、`tst_dead_peer_evictor`、`tst_sync_baseline_e2e`、`tst_sync_dead_peer`、`tst_sync_migration`、`tst_sync_untracked_change`、`tst_sync_byte_budget`、`tst_sync_schema_quarantine` | 基线重置 applied-vector/table_state/row_winner；失败`E_SYNC_BASELINE_FAILED`不动旧锚点；迁移半截可恢复（G-02）；字节预算守界（NFR-5） |

### 12.7 风险与缓解

| 风险 | 影响 | 缓解措施 |
| --- | --- | --- |
| `SyncWorker` 大文件覆盖率 | 主循环/状态机分支多、单元难穷尽，单元覆盖率拉不到目标 | 主循环路径**靠集成套件**触达（`tst_sync_two_node_converge`/`ack_wait`/`star_broadcast` 等），单元仅覆盖可隔离子逻辑；该文件计入加权 ≈75% 缺口，整体仍 ≥80% |
| 崩溃注入需源码 hook | 无注入点则崩溃零窗口不可断言（FR-1/NFR-8） | M1a 即提供「子进程 + 命名注入点」夹具（提交前/seal 后/commit 后），实现侧预留 fault-hook；`tst_sync_crash_zero_window` 依赖此 hook，缺失即该用例 `QFAIL` 而非静默跳过 |
| `RebaseEngine`/`SessionRecorder` 平台依赖 | `sqlite3rebaser_*`/`SQLITE_ENABLE_SESSION` 在系统 Qt 自带 SQLite 上可能缺失 | 阶段0闸门（`tst_phase0`）显式探测，缺失→`E_SYNC_SESSION_UNAVAILABLE` 全停；CI 用**自带 SESSION/rebaser 的 SQLite**构建；POSIX-only 分支以 `[UNIX]` 标注并计入「尽力覆盖」缺口 |
| 时间相关测试 flaky | ACK SLA/gap 超时/死对端阈值用真实 `sleep` 会抖动 | 全部用**注入时钟** `FakeClock`（§3.6/§8.0）推进虚拟时间，`std::function<qint64()> nowMs` 注入点；禁真实 `sleep`；注入点未落地前的降级写法（`QTRY_VERIFY`+短延时）不作长期方案 |
| 2Mbps 预算语义误解 | 误当时延约束写成「N ms 内完成」导致 flaky | 明确 **2Mbps 为字节预算非时延**：`tst_sync_byte_budget` 断言单位窗口出箱**字节数**上界（用注入时钟界定窗口），不断言墙钟耗时；超软阈值`W_SYNC_PAYLOAD_LARGE` |
| 三处登记漏配 | 套件静默漏跑，覆盖率虚高 | CI §12.5.2 三处清单一致性校验，差集非空即红 |
| 错误码文案未加码前缀 | `ASSERT_ERR_CODE` 无法码级断言（仅文案匹配，违反 O3） | 退出准则③ + CI §12.5 步5 grep 校验：每个 `E_SYNC_*`/`W_SYNC_*` 常量须在测试中出现；实现侧统一为失败文案冠码前缀 |
