# 代码审查报告（第三十四轮）

## 总览（评分、问题数统计）

评分：92/100。

问题统计：

- Critical：0
- High：1
- Medium：0
- Low：0

本轮复核范围：`src/` 全部源码，并对照以下规范：批量导入导出两份设计文档、SQLite 同步工具设计/plan、`export-column-order`、`export-reverse-lookup`、`fk-injection`、`row-lookup`、`time-format` OpenSpec。

上轮重点修复闭合性：

- C-01：`SyncEngine::stop()` 当前只调用 `cancelAckWait()`、置前台状态为 `Stopped` 并释放 gate；未停止 `SyncWorker`，符合“只停止前台 operation，后台继续运行”的语义。
- H-01：`DataBridgePrivate::loadProfileDoc()` 在 DB 已打开时调用 `ProfileValidator::validateForExport()`，主路径已闭合。
- H-02/M-03：`DataBridge::dbPath()` 返回 `PRAGMA database_list` 解析路径；`SyncWorker` 发布 context 时同样用 `PRAGMA database_list` 解析路径查询 registry，主路径已闭合。
- H-03：`SyncEngine::syncSelected()` 入口已在占用 gate 前同步拒绝 `selection.isEmpty()`。
- H-04：`CapturedWriteTemplate` 的 `pkHash` 已改为 `RowWinnerStore::pkHash()`，统一到 `TableStateStore::rowHash()` 的类型标记编码。
- M-02：`ForeignKeyPreflight` 构建 in-batch parent cache 时已跳过 `ctx.failedRouteIndices`，父 route 失败不会再被子 route 当作有效 batch parent。

验证：

- 顶层 `ctest --test-dir build --output-on-failure` 当前未发现 CTest 注册项。
- 直接运行 `build/tests/tst_*` 17 个测试二进制，并设置 `LD_LIBRARY_PATH=/opt/Qt5.12.12/5.12.12/gcc_64/lib`，全部通过。
- 本报告未将 `exportOnMissing:"error"` 整行跳过标记为问题；该行为符合 `export-reverse-lookup` 规范。
- 本报告未将 SchemaEligibility 拒绝复合 PK 标记为问题；按用户说明视为已知 MVP 限制。

## Critical 问题

无。

## High 问题

### H-01：lookup / reverse-lookup 对 NUMERIC 或无声明类型的 G 列错误执行 `toString()`，违反 affinity coercion 表并会导致误报 lookup miss

证据：

- `openspec/specs/row-lookup/spec.md:103-110` 明确规定 lookup match-key 按 G 列 affinity coercion：`TEXT` 才 `toString()`，`INTEGER` 转 `toLongLong()`，`REAL` 转 `toDouble()`，而 `BLOB`、`NUMERIC`、无声明 affinity 必须保留原始 `QVariant`。
- `openspec/specs/export-reverse-lookup/spec.md:127-129` 要求 reverse lookup 复用与 import-side lookup 相同的 affinity coercion 表。
- `src/service/ImportService.cpp:43-68` 的 `castToAffinity()` 只特殊处理 `INT`、`REAL/FLOA/DOUB`、`BLOB`，其余全部 `return QVariant(raw.toString())`，注释也写成 `TEXT / NONE`。
- `src/service/ExportService.cpp:141-160` 使用相同逻辑，导致 reverse lookup 也把 `NUMERIC` 或空声明类型错误转为字符串。
- import 侧 prefetch 时用 cast 后的 key 存入 `keyMap`（`src/service/ImportService.cpp:291-304`），但从 G 表查询结果重建 cache key 时直接使用 `q.value()`（`src/service/ImportService.cpp:381-386`）。当 G 列是 `NUMERIC` / 无声明类型时，绑定侧被错误字符串化，结果侧可能由 SQLite/Qt 返回整数或浮点 `QVariant`，两边 type-tagged key 不同，从而把实际存在的匹配行误判为 `E_LOOKUP_NOT_FOUND`。
- export 侧同样在 H 值收集、prefetch result key、逐行 resolution 三处使用错误 coercion（`src/service/ExportService.cpp:366-382`、`418-423`、`469-475`），会造成反向 lookup 误判 `E_REVERSE_LOOKUP_NOT_FOUND` 或错误写空/跳行。

影响：

- 合法 profile 与合法参考表数据，在 G 匹配列声明为 `NUMERIC`、`DECIMAL`、`BOOLEAN`、空类型等 SQLite NUMERIC/none affinity 场景下，可能导入阶段错误跳过 route，或导出阶段错误跳过/置空行。
- 该问题不是优化建议，而是 OpenSpec 明确 coercion 表与当前实现不一致。

最小修复建议：

- 抽出一份共享的 affinity coercion helper，供 import lookup 与 export reverse lookup 共用。
- 按 SQLite affinity 判断：
  - 包含 `INT` → `toLongLong(&ok)`
  - 包含 `REAL` / `FLOA` / `DOUB` → `toDouble(&ok)`
  - 包含 `CHAR` / `CLOB` / `TEXT` → `toString()`
  - 包含 `BLOB` 或声明类型为空 → 原始 `QVariant`
  - 其他（NUMERIC affinity，如 `NUMERIC` / `DECIMAL` / `BOOLEAN` / `DATE` / `DATETIME`）→ 原始 `QVariant`
- 增加 import 与 export 各一个回归用例：G 列为 `NUMERIC` 或空类型，确认不被错误 `toString()`，并确认 cache key 与 row-time key 一致。

## Medium 问题

无。

## Low 问题

无。

## 总结

本轮重点复核的上轮 C/H/M 修复项主路径均已闭合，未发现新的同步生命周期、gate、DB path registry、pkHash 编码或 FK preflight 过滤回归。

当前唯一需要修复的是 lookup/reverse-lookup 的 affinity coercion：实现把除 INT/REAL/BLOB 外的类型全部字符串化，和 `row-lookup` / `export-reverse-lookup` 对 NUMERIC 与无声明类型的明确规则冲突。建议优先修复为共享 helper，并补覆盖 NUMERIC/none affinity 的双向测试。
