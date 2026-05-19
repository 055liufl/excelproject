## 1. ProfileSpec 数据结构升级

- [x] 1.1 在 `src/profile/ProfileSpec.h` 新增 `LookupSpec { QString name; QString fromTable; QVector<QPair<QString,QString>> match; QVector<QPair<QString,QString>> select; }`。`match` / `select` 为有序数组（不是 dict），由 JSON 数组形式 `[[a,b],...]` 解析而来
- [x] 1.2 把 `FkInjectSpec` 改为 `{ fromTable; QVector<QPair<QString,QString>> pairs; }`，删除旧 `fromColumn` / `toTable` / `toColumn` 字段
- [x] 1.3 把 `RouteSpec::fkInject` 从 `std::optional<FkInjectSpec>` 改为 `QVector<FkInjectSpec>`，并新增 `QVector<LookupSpec> lookups`
- [x] 1.4 全量编译，定位所有调用旧字段（`fkInject.has_value()` / `.fromColumn` / `.toColumn`）的位置，标记后续待改

## 2. ProfileLoader 解析

- [x] 2.1 解析 `lookups[]`：数组形式，按对象逐个读 `name`（string，必填非空）/ `from` / `match[[G_col, excel_header],...]` / `select[[G_col, target_dbColumn],...]`，构造 `LookupSpec`
- [x] 2.2 `match` / `select` **必须是 JSON 数组形式**；遇到 JSON object 形式 `match:{}` / `select:{}` 显式报错并提示 `[[a,b],...]` 写法
- [x] 2.3 把 `fkInject` 的解析从对象改为数组遍历，每个元素读 `from`（裸表名，不再是 `table.col` 形式）+ `pairs[][2]`
- [x] 2.4 遇到旧 `fkInject:{...}` 对象形式时显式报错（错误信息提示新数组写法）
- [x] 2.5 错误消息一律带 route table 名 + lookup name（如适用），便于定位

## 3. ProfileValidator 校验扩展

- [x] 3.1 每个 lookup：`fromTable` 必须存在于 SchemaCatalog；`match.first` / `select.first` 必须是 G 表实际列；`match.second` 必须命中 sheet header
- [x] 3.2 每个 lookup 的 `select.second` 与本 route 的 `columns` dbColumn、其它 lookup 的 `select.second`、本 route 任一 `fkInject` 的子列名 在 route 内全局唯一
- [x] 3.3 每个 `fkInject` group：`fromTable` **必须命中本 profile 中某 route 的 `table`**（只在 schema 但未声明为 route 的表 → 拒绝，错误消息建议改用 `lookups`）；每个 `pair.first` 在父 route 的 columns 或 lookup 输出中存在；`pair.second` 在本 route 目标表中存在；`pairs` 非空
- [x] 3.4 不允许 lookup 级联：若 `match.second` 不是 sheet header，报错（即使它恰好命中某 lookup 的输出名）
- [x] 3.5 同一 `fkInject` group 内 pairs 不能混合 lookup-derived 与非 lookup-derived 的 `parent_column`；违反时错误消息要建议拆成两个独立 group
- [x] 3.6 同一 route 的所有 fkInject groups 合并起来，`child_column` 在所有 pair 中不重复；重复目标拒绝
- [x] 3.7 `fkInject` 字段缺失 / `[]` / `null` 等价为 no-op，不报错（验证逻辑明确允许并跳过该 route 的 fkInject 段）
- [x] 3.8 实现建议：维护"本 route 各 dbColumn 来源映射"（Excel / lookup / fkInject），把 3.2 / 3.5 / 3.6 在同一遍扫里完成
- [x] 3.9 每个 lookup 的 `name` 非空（trim 后非空）；同 route 内 `name` 不重复
- [x] 3.10 每个 lookup 的 `select` 数组内部，target 名（每对的第二个元素）不重复
- [x] 3.11 dbColumn 三方唯一性（Excel `columns:` / lookup `select` target / fkInject `pairs` child_column）统一在此校验；fk-injection #7 的范围仅限 fkInject 内部

## 4. ImportService Phase A.5 lookup prefetch

- [x] 4.1 在 `ImportService::run` 中、Phase A（header 读取）之后、Phase B（逐行）之前插入 Phase A.5
- [x] 4.2 第一遍扫 Excel：实现期决定"全量缓存到内存"或"再 `reader.open()` 一次"，封成内部策略，row 序列对 Phase B 透明
- [x] 4.3 **lookup-identity 归并**：把所有 routes（含 Mixed 模式各 classes 内的）声明的 lookups 按 `(from, match-pairs 有序, select-pairs 有序)` 三元组归并等价类；同 identity 的 lookups 共享 1 份 prefetch 结果。identityKey 建议形如 `"<from>::<match-cols-joined>::<select-cols-joined>"`
- [x] 4.4 对每个 identity：扫 sheet 收集 match-key tuple 集合，按 G 列 affinity 强制 cast（详 §5.cast 步骤）；cast 失败或 empty 的 key 不进集合（row-time 才报错）；集合自动去重
- [x] 4.5 K = identity 的 distinct key 数。**K == 0 时 0 个 SELECT，直接跳过**
- [x] 4.6 构造批量 SELECT：`SELECT <match cols>, <select cols> FROM <G> WHERE (<match cols>) IN ((?,?),(?,?),...)`；单列退化为普通 `IN`
- [x] 4.7 若 SQLite 版本不支持 row-value IN，降级为 `(c1=? AND c2=?) OR (...)` 拼接（仍按 chunk 批量）
- [x] 4.8 按 `SQLITE_MAX_VARIABLE_NUMBER` 分块，分批执行后 merge map；共发出 `ceil(K / chunk_limit)` 次 SELECT
- [x] 4.9 prefetch 结果存为 `QHash<QVector<QVariant>, LookupHit>`，其中 `LookupHit { QVector<QVariant> values; int hitCount; }`，按 identityKey 索引
- [x] 4.10 SELECT 任一失败：调 ErrorCollector::addTable 写 `E_LOOKUP_QUERY_FAILED`，立即返回（fatal）
- [x] 4.11 **加 prefetch query counter hook**：`std::function<void(const QString& identityKey)>` 注入到 ImportService；每次实际执行 SELECT 时回调一次；production 默认 noop，test-time 注入计数器。spec R2 Sc1 / K=0 skip / identity 归并断言都靠这个 hook
- [x] 4.12 **Mixed 模式下** prefetch 仍 sheet 全局（不需要先跑 discriminator）；row-time 才按 class 分流

## 5. ImportService Phase B 行级 lookup 解析

- [x] 5.1 路由分发时（Mixed 模式先用 discriminator 解析 classId 选 routes 子集），对每个声明 lookup 的 route：从行内取出 match key，按 G 列 affinity 做与 prefetch 同样的 cast
- [x] 5.2 **空判定**：QVariant `isNull()` 或 `toString().trimmed().isEmpty()` → row-error `E_LOOKUP_KEY_EMPTY`。数值零 / `"0"` / `0.0` **不算空**
- [x] 5.3 **cast 失败**（如 "abc" → INTEGER）→ row-error `E_LOOKUP_KEY_INVALID`
- [x] 5.4 拼 key tuple，按 identityKey 查 prefetch map：map miss → row-error `E_LOOKUP_NOT_FOUND`
- [x] 5.5 `hitCount > 1` → row-error `E_LOOKUP_AMBIGUOUS`（错误消息含 "consider deduplicating G on match columns"）
- [x] 5.6 命中 1 行：把 lookup.select 的 (G.col → target dbColumn) 结果 append 到当前 RoutePayload；**G 的 select 列为 NULL 时透传 NULL**（不报错；下游 fkInject / db 约束裁决）
- [x] 5.7 等值比较使用原始 cast 后值的严格 QVariant equality；**不做 case-fold / trim 归一**（trim 只在 5.2 空判定时使用）
- [x] 5.8 所有 row-level 错误 attribute 到 (excelRow, classId-if-Mixed, routeTable, lookupName)
- [x] 5.9 lookup append 结果与本 route 的 columns 映射结果合并 —— 经 §3 校验后名字不会冲突，直接 append 即可

## 6. ImportService doFkInject 多列 + 多父表

- [x] 6.1 重写 `static void doFkInject(...)`：外层遍历 routes，中层遍历 `route.fkInject` 数组，内层遍历 `pairs`
- [x] 6.2 父 payload 查找：按 `fk.fromTable` 在 `table → payloadIdx` map 中查
- [x] 6.3 对每个 pair：从父 payload 取 `parent_column` 的 bind，写入子 payload 的 `child_column`（覆盖或追加）
- [x] 6.4 同步更新 conflictVals：按 conflictKey 列名定位下标（不再假定 index 0），多列分别写
- [x] 6.5 删除 `src/mapping/FkInjector.cpp` 中的旧死代码占位实现（或保留 stub，由 6.1 实际实现替代）
- [x] 6.6 NULL 严格策略：注入前判 `parentBind.isNull()` → 行级 `E_VALIDATE_FK`，不写 NULL 到子 payload，并把本 route 标记为本行失败（供 6.7 级联）
- [x] 6.7 行级级联抑制：ImportService 维护 per-Excel-row 的 `failedRoutes` 集合；处理子 payload 时按 `parent` 链向上查，若祖先在集合中 → 本 payload drop，不报二次错误（详见 design D11）

## 7. ForeignKeyPreflight 复合化

- [x] 7.1 `batchParentKeys` 从 `QHash<QString, QVector<QVariant>>` 改为 `QHash<QString, QVector<RoutePayload>>`（存完整父 payload，按列名查 tuple）
- [x] 7.2 **tuple 值从 `parentPayload.binds[ indexOf(pair.parent_column) ]` 取，不从 `conflictVals` 取**（修旧实现的"fk-from == parent conflict key" happy-accident 依赖；详见 design D5）
- [x] 7.3 batch-内命中比较改成 tuple equality（按列对齐）
- [x] 7.4 SQL probe 改成 `SELECT 1 FROM <parent> WHERE c1=? AND c2=? ... LIMIT 1`
- [x] 7.5 实现 group-level 跳过规则：preflight 时按 `fk.fromTable` 在 `allRoutes` 中查 RouteSpec；若该 route 含 lookup **且本 group 中每一个 `pair.first` 都命中该 route 的 lookup.select target**，则整个 group 跳过 SQL probe（batch 内命中仍按原逻辑）。混合 group 已在 validator 阻拦（3.5），preflight 不需要 partial-WHERE 分支
- [x] 7.6 复合 FK 缺失时，错误消息 include 完整 tuple 字符串（message 字段含 `key=val` 格式）
- [x] 7.7 **加 query counter hook**：在 `ForeignKeyPreflight` 里加一个可注入的 `std::function<void(const QString& table)>` 或简单 `int* probeCount`，每次实际执行 SQL probe 时回调一次；测试期注入计数器，生产期注入 noop。**spec 中 R4 Sc2（in-batch 命中不发 SQL）和 R5（group-level 跳过）都依赖这个 hook 才能可观测**

## 8. 错误码与公开 API

- [x] 8.1 在 `include/dbridge/Errors.h` 增加**五个**常量：`E_LOOKUP_KEY_EMPTY` / `E_LOOKUP_NOT_FOUND` / `E_LOOKUP_AMBIGUOUS` / `E_LOOKUP_QUERY_FAILED` / `E_LOOKUP_KEY_INVALID`
- [x] 8.2 公开头 `ProfileSpec`（如对外暴露）同步更新；若仅 detail 命名空间则跳过
- [x] 8.3 `ImportResult` 加一个可选字段 `QVector<RowContext> dryRunPayloads`（默认空 vector）；只有当 `ImportOptions.dryRun==true` 时才填充
- [x] 8.4 `ImportOptions` 加 `bool dryRun = false`：true 时执行所有 prefetch / lookup / fkInject / preflight / 错误收集，但跳过 SqlBuilder UPSERT，并把构建好的 RowContext 列表写到 `ImportResult.dryRunPayloads`
- [x] 8.5 README 加一段简短说明 dryRun 用法；明确"不鼓励生产路径使用"

## 9. SqlBuilder 回归确认

- [x] 9.1 阅读 `src/sql/SqlBuilder.cpp` UPSERT 路径，确认它从 `RoutePayload.dbColumns` 取列、从 `conflictKey/conflictVals` 取冲突解决；多列 conflict 已在原实现支持
- [x] 9.2 确认 dryRun 模式下完全跳过 SqlBuilder 调用（在 ImportService 加 if-gate，不在 SqlBuilder 内部加 dryRun 分支）
- [x] 9.3 不需要新增代码；写一条 unit test 直接覆盖"lookup 注入 + 复合 fkInject + 多列 conflict UPSERT" 的端到端路径以确认无回归

## 10. Tests

- [x] 10.1 新增 `tests/unit/tst_lookup_prefetch.cpp`：单 lookup 批量 SELECT 生成、chunk 分批、SQL 失败 fatal、**K=0 时 0 SELECT**（用 4.11 prefetch counter 断言）、**identity 归并：两 route 同 identity → counter 一次性数到 ceil(K/L)**、**Mixed 模式跨 class identity 归并**
- [x] 10.2 新增 `tests/unit/tst_lookup_semantics.cpp`：0/1/N 命中、empty match key（含 `""` / `" "` / null）、**数值零不为空**、**case-fold/trim 严格不归一**、**cast 失败 → E_LOOKUP_KEY_INVALID 且该 key 不进 prefetch IN-list**、route-local 不外泄（用 8.4 dryRun + dryRunPayloads inspect 断言）、"lookup→fkInject 两跳" 端到端、**G select 列 NULL 透传**
- [x] 10.3 扩 `tests/unit/tst_profile_loader.cpp`：新数组 fkInject 解析、旧 `{from,to}` 对象形式被拒、lookup 解析、`match`/`select` 必须数组形式（dict 被拒）、**lookup `name` 必填**、**lookup `name` 同 route 唯一**、**select 内部 target 重复被拒**、配额错误消息
- [x] 10.4 扩 `tests/unit/tst_fk_preflight.cpp`：复合 WHERE、batch-内 tuple 命中（用 7.7 的 query counter 断言 probe count == 0）、group-level lookup-derived 跳过（counter == 0）、纯非 lookup-derived group 走完整 probe（counter > 0）、复合 FK 缺失错误消息
- [x] 10.5 全量改写 `tests/data/profiles/*.json` 中的 fkInject 字段为新数组形式（order_m_set.json + mixed_abc_multitable.json）；新增至少一个 fixture 演示 lookup 用法（待后续 session）
- [x] 10.6 扩 `tst_profile_loader.cpp` / 新增 `tst_profile_validator.cpp`：覆盖 3.3 (`from` 必须是 route)、3.5 (混合 group 拒绝)、3.6 (重复 child_column 拒绝)、3.7 (`fkInject: []` / 缺失 / `null` 三态接受)；每条至少 1 正 1 反测试
- [x] 10.7 端到端覆盖 NULL 严格策略（6.6）：构造 parent 行 `order_no=NULL`，子 fkInject 行报 `E_VALIDATE_FK`，子 payload 不持久化（用 dryRunPayloads 断言 payload 缺失）
- [x] 10.8 端到端覆盖错误级联抑制（6.7 / design D11）：A.lookup 失败 → B(parent:A) 在 ErrorCollector 不再叠错；C(无 parent 链关联) 同行仍正常处理
- [x] 10.9 端到端覆盖 lookup 输出参与 conflict.columns（spec "lookup outputs may participate in conflict.columns"）：lookup 拿回 tenant_id 后参与 `(tenant_id, line_no)` UPSERT 冲突解决，重复 line_no 应触发 ON CONFLICT 路径
- [x] 10.10 端到端覆盖 Mixed 模式 lookup：同一 lookup identity 在两个 class 下声明，prefetch counter 断言 == 1 次（or ceil(K/L)）；row-time 错误消息含正确 classId

## 11. Docs 同步与 BREAKING 通告

- [x] 11.1 `README.md` 的"Profile 字段"章节顶部加 BREAKING 区块，说明旧单列 fkInject 已删除，附前后写法对照
- [x] 11.2 `README.md` 新增"lookups" 字段说明 + 至少 1 个示例
- [x] 11.3 改写 `README.md` 内所有 fkInject 示例
- [x] 11.4 改写 `docs/validation/row-to-multitable.md` 中所有示例；新增 lookup 章节并指向 fixture
- [x] 11.5 改写 `specs/MVP-Qt-SQLite-Excel-批量导入导出-实现设计.md` / `specs/Qt-SQLite-Excel-批量导入导出-设计文档.md` / `specs/Qt-SQLite-Excel-批量导入导出-实现文档.md` 中所有 fkInject 示例
- [x] 11.6 FAQ 增条目："如何用 lookup 从参考表带字段进来" + "复合外键 fkInject 怎么写"

## 12. CI 残留校验

- [x] 12.1 在 verify 阶段加一条检查：`rg '"fkInject"\s*:\s*\{' --type=json --type=md` 命中即视为漏改，导致 verify 失败
- [x] 12.2 跑全套 unit / integration test，确认绿
- [x] 12.3 用 `tools/xlsx2csv.py` 或现有 e2e 路径手动跑一个含 lookup + 多列 fkInject 的 fixture，确认导入/导出全链路 OK
