## Why

导出到 Excel 时列的顺序由 `SqlBuilder::buildAutoJoinSelect` 拼装的 SELECT 字段顺序决定（`src/sql/SqlBuilder.cpp:60-62`，按"路由声明顺序 × 路由内 columns 声明顺序"展开）。在 Mixed 模式下进一步演变为"各 class 顺序处理 + 首次出现合并"（`src/service/ExportService.cpp:108-114`）。两者都对用户不可见，调整列顺序意味着改动 profile 的声明顺序——这在 multi-route / Mixed 模式下很别扭、不直观。

需要一个显式的"导出列顺序"声明，让用户用 Excel header 名直接表达"我要这样的列顺序"。

## What Changes

- 在 `exportSpec` 新增可选 `columnOrder: [<excel-header>, ...]` 字段；
- `columnOrder` 中的标识使用 **Excel header 名**（即 `ColumnSpec.source`），与 ColumnSpec 中暴露给用户的"列名"一致；
- 实际输出顺序规则：
  1. 在 `columnOrder` 中出现的 header 按声明顺序排到前面；
  2. 未列入 `columnOrder` 的 header 按"原有顺序"追加到末尾（SingleTable / MultiTable：SQL 投影顺序；Mixed：首次出现顺序）；
  3. Mixed 模式下 `classColumn` 默认放首列；若 `classColumn` 在 `columnOrder` 中显式出现，则按 `columnOrder` 指定的位置；
- profile 加载期硬校验：`columnOrder` 中的每个值**必须**匹配至少一个路由的某个 `ColumnSpec.source`；不命中则报错 `E_EXPORT_UNKNOWN_HEADER`（避免 typo 静默漂移）；
- 与 `explicitSql` 互斥：若同一 profile 中 `exportSpec.sql` 与 `exportSpec.columnOrder` 同时非空，加载期报错 `E_EXPORT_ORDER_WITH_RAW_SQL`（原生 SQL 用户自己排）；
- `orderBy` 与 `columnOrder` 完全独立：前者排"行"，后者排"列"，互不影响、可同时使用。

## Capabilities

### New Capabilities
- `export-column-order`: profile 中 `exportSpec.columnOrder` 声明的语义、Mixed 模式合并规则、classColumn 默认位置与显式覆盖、未列入列的追加策略、未知 header 校验、与 explicitSql 的互斥规则。

### Modified Capabilities
（无——`row-lookup` 与 `fk-injection` 不涉及导出列顺序。`time-format`（独立 change）即便先落地也无影响：列顺序只决定"投影到 Excel 哪个位置"，与每个单元格的内容如何序列化无关。）

## Impact

- **代码**：
  - `src/profile/ProfileSpec.h`：`ExportSpec` 新增 `QStringList columnOrder`；
  - `src/profile/ProfileLoader.cpp`：解析 `exportSpec.columnOrder`；
  - `src/profile/ProfileValidator.cpp`：未知 header 校验、与 `explicitSql` 互斥校验；
  - `src/service/ExportService.cpp`：在 SingleTable/MultiTable 路径上对 query 结果做"内存重排"（按 `columnOrder` 重排 headers，并相应重排每行数据）；在 Mixed 路径上对 `allHeaders` 重排，`classColumn` 默认首列 / 显式定位逻辑；
  - 新增错误码 `E_EXPORT_UNKNOWN_HEADER`、`E_EXPORT_ORDER_WITH_RAW_SQL`。
- **profile 文件**：纯增量，不写 `columnOrder` 行为完全等同今日。
- **测试**：
  - 顺序生效（SingleTable / MultiTable / Mixed）；
  - 未列入列追加到末尾；
  - `classColumn` 默认首列 / 显式定位；
  - 未知 header 加载期报错；
  - 与 `explicitSql` 互斥；
  - `orderBy` 与 `columnOrder` 共存正常。
- **依赖**：无新增第三方依赖。
