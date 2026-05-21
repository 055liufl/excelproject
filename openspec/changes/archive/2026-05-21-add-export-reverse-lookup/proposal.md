## Why

`row-lookup`（已归档变更 `add-row-lookup-and-multi-fk-inject`）让 import 时可以"用 Excel 行 I 的某个字段值到参数表 G 中查询字段集合 H，把 H 注入到 payload"。结果是：

- 原 Excel 行里的"业务键"（如客户编号、商品编号）作为 `lookup.match` 的 Excel header；
- 派生的"业务属性"（如客户名、商品类目）作为 `lookup.select` 写到 DB；
- DB 里只剩派生属性 H，**原 Excel 字段 A 已经被 lookup 消费掉、不写 DB**。

导出时面对的局面：DB 里只有 H，但用户想要拿回**原本的 Excel 视图**——也就是字段 A，而不是 H。当前导出会把 H 列直接写到 Excel，造成"导入→导出"非无损往返，且面向最终用户的 Excel 视图与原始 Excel 不一致。

需要"反向 lookup"——导出时用 DB 行里的 H 列作为 match-key 反查 G，把 A 还原回 Excel，让往返闭环。

## What Changes

- **复用现有 `lookups` 声明**：同一份 `lookups[]` 数组在 import 和 export 两个方向上**对称**生效，**不**新开 `reverseLookups` 段。
  - import 方向（保持现状）：`match` 的 Excel header 作为查询键，`select` 的列写到 payload；
  - export 方向（本变更新增）：`select` 的 dbColumn 作为查询键反查 G，`match` 的 Excel header 还原到导出行。
- 在 `LookupSpec` 上新增可选字段 `exportRoundtrip: bool`（默认 `true`）。设为 `false` 时，该 lookup 在 export 方向跳过反查、保留 H 列原样输出（少数场景需要）。
- 在 `LookupSpec` 上新增可选字段 `exportOnMissing: "error" | "null" | "skip"`（默认 `"error"`，与 import 侧严格语义对称）：
  - `"error"` → DB 行无法在 G 中找到匹配 → 行级 `E_REVERSE_LOOKUP_NOT_FOUND`，该 row 在 Excel 中被跳过（保持与 import 同等严格）；
  - `"null"` → 无匹配时把 `match` 的 Excel 列写空，行其它列继续；
  - `"skip"` → 无匹配时该单元格写空，**不**报错（用于历史脏数据已知场景）；
- export 方向的反查也使用**批量 prefetch + identity 合并**，与 import 完全对称：
  - 一个 lookup-identity 在 export 上的 batch SQL 是 `SELECT match.G_column..., select.G_column... FROM G WHERE (select.G_column,...) IN (...)`；
  - K = 0 → 零 SELECT；K > 0 → `ceil(K / chunk_limit)` 次 SELECT；
  - 同一 identity 跨 routes / classes 合并（与 import 一致）。
- **替换语义**：在 export Excel 输出中，凡是参与 `lookups[*].select` 的 dbColumn（即 H 列）**默认不再出现**；取而代之的是 `lookups[*].match` 中的 Excel header（即 A 列），值来自反查结果。
  - 若同一 Excel header 在多处出现（例如它既是 `lookup.match` 又是其它 route 的 `ColumnSpec.source`），以 `ColumnSpec.source` 的值优先（即用户在某 route 上显式声明的导出值），反查结果仅在"无别的来源"时填入。
- 与 `time-format`、`export-column-order` 两个并行 change 完全正交：
  - 反查值还原到 Excel header `A` 之后，若 A 也声明在 `dateFormat` 等时间槽下，时间格式照常生效；
  - `columnOrder` 中可以列入 `A`（而 H 不再出现，所以列 H 也不应出现在 `columnOrder` 中——本 change 的 spec 会规定 columnOrder 校验对这种情况报错或剔除）。

## Capabilities

### New Capabilities
- `export-reverse-lookup`: export 方向反向消费 `lookups[]` 声明，把 H 列替换回 A 列；batch prefetch 与 identity 合并对称；`exportRoundtrip`、`exportOnMissing` 两个开关；与 `columnOrder` / `time-format` 的协同规则。

### Modified Capabilities
- `row-lookup`: 现有"Route-local visibility of lookup outputs" 与 "dbColumn naming uniqueness within a route" 两条 requirement 的语义保持不变，但新增**两个字段**（`exportRoundtrip`、`exportOnMissing`）的声明、默认值、校验规则属于该 spec 的扩展点。本 change 提供 delta 文件追加这两个声明字段，并显式说明它们**不影响** import 方向行为。

## Impact

- **代码**：
  - `src/profile/ProfileSpec.h`：`LookupSpec` 增加 `bool exportRoundtrip = true; QString exportOnMissing = "error";`
  - `src/profile/ProfileLoader.cpp`：解析两个新字段；`exportOnMissing` 枚举校验（只允许三值）；
  - `src/profile/ProfileValidator.cpp`：与 `exportRoundtrip=false` + `exportOnMissing` 显式声明的组合需校验（后者在前者为 false 时无意义，但允许、给 info-level 提示）；
  - **新增** `src/mapping/ReverseLookupResolver.{h,cpp}`（或在 ExportService 内一个 private class）：实现 export 端的 batch prefetch、identity 合并、cardinality 检查、map 构造；
  - `src/service/ExportService.cpp`：
    - 在执行主 SELECT 之前先收集所有 lookup-identity → run prefetch；
    - 在每行投影到 Excel 之前调用 ReverseLookupResolver，把 H 列值翻译为 A 列值，按 `exportOnMissing` 处理失败；
    - **改变投影列集**：不再投影 H 列；改投影 A 列（来自反查 map）；
  - `src/sql/SqlBuilder.cpp`：`buildAutoJoinSelect` 需要知道"哪些 dbColumn 是 lookup-derived 不参与 SELECT 投影"——通过传入新的 hint 或在 ExportService 调整投影后处理（倾向后者，保 SqlBuilder 不变）；
  - 新增错误码 `E_REVERSE_LOOKUP_NOT_FOUND`、`E_REVERSE_LOOKUP_AMBIGUOUS`、`E_REVERSE_LOOKUP_QUERY_FAILED`。
- **profile 文件**：纯增量。已声明 `lookups[]` 的 profile **自动**获得反向 lookup 能力（`exportRoundtrip` 默认 `true`），这对**部分存量行为**是一个**非破坏性但可见的变化**——之前导出时 H 列会出现在 Excel，现在 A 列会出现。spec 中会写明这一过渡，并提供"`exportRoundtrip: false` 一行回到旧行为"的迁移路径。
- **测试**：
  - 单 lookup 双向往返（import 后 export 拿到原始 Excel）；
  - 多 lookup、多 match 键、复合 match key 的反查；
  - cardinality 严格 / null / skip 三种 `exportOnMissing` 路径；
  - identity 合并（跨 route / 跨 class）的 prefetch 次数；
  - `exportRoundtrip: false` 关掉反向；
  - 与 `columnOrder` / `time-format` 协同；
  - Mixed 模式 per-class 反查解析。
- **依赖**：无新增第三方依赖。
