# ADR 0003: 导出反向 lookup 复用 lookups[] 声明，替换语义，ExportService 单点改造

**状态：** 已采纳  
**日期：** 2026-05-21

## 背景

导入时 `lookups[]` 把 Excel A 列（如 `CustNo`）通过 G 表翻译成 DB H 列（如 `customer_name`）写库。导出时原路径直接把 H 列写进 Excel，无法还原原始 A 列，导致导入→导出不对称。

## 决策

在 export 方向对 `lookups[]` 做"反向查找"：以 H 列值查 G 表，取出对应的 A 列值写 Excel。H 列在导出中消失，A 列出现（替换语义）。

## 关键选择与权衡

### 复用 lookups[]，不开 reverseLookups

`match[i] = [G_col, Excel_header]` 已天然包含"还原到 Excel 的 header 名"，两个方向声明完全对称。分裂声明则校验逻辑需双写，且容易两边不一致。

### 替换语义（H 消失，A 出现）

同时输出 H 和 A 时，再次 import 会因 `dbColumn naming uniqueness` 校验失败，闭环破掉。替换才是真正的无损往返。提供 `exportRoundtrip: false` 逃生口，保留 H 原样输出的旧行为。

### exportRoundtrip 默认 true

声明 `lookups[]` 的用户已明确表达"H 是派生的，A 是源"，默认还原符合声明意图。零迁移成本的旧行为可一行 `exportRoundtrip: false` 恢复。

### SqlBuilder 不变，ExportService 单点调整

ExportService 在主 SELECT 执行后识别 H 列，追加到 SQL 尾（`, table.h_col`），全量加载后 prefetch，再投影输出。SqlBuilder 与 import 路径零改动，风险最低。

### Batch prefetch + identity 合并

与 import 端 LookupPrefetcher 对称：同一 `(from, match-pairs, select-pairs)` identity 跨 route/class 共享一次 prefetch；用 `IN (...)` 批量查询，分块避免 SQLite 参数上限。

### D5 命名冲突优先级

若 A 列 header 也是某 ColumnSpec.source，SQL 直接命中值（非 lookup 派生）优先；反查结果仅在直接值为 NULL 时填入，不破坏现存跨 route 语义。

### exportOnMissing 三选一

`"error"`（默认，对称 import 严格语义）/ `"null"` / `"skip"`；多匹配始终报 `E_REVERSE_LOOKUP_AMBIGUOUS`，不可掩盖。

## 被否决的方案

- **开 reverseLookups 段**：需重写 match/select 解析与 schema 校验，且与 import 声明分裂。
- **在 SqlBuilder 中排除 H 列**：SqlBuilder 为 import/export 共享，引入"导出方向 hint"污染严重。
- **默认 exportRoundtrip: false（opt-in）**：放弃了"声明 lookups 就该无损往返"的核心价值，让用户写更多配置。
