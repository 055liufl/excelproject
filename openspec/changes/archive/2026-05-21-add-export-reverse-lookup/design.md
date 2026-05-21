## Context

```
import 方向（已实现，row-lookup spec 已固化）：
  Excel.match.header ──查询──▶ G ──取值──▶ payload.select.dbColumn ──▶ 写 DB

export 方向（本变更）：
  DB.select.dbColumn ──查询──▶ G ──取值──▶ Excel.match.header ──▶ 写 Excel
  （同一份 lookups 声明的箭头反过来读）
```

现状的 export 路径（`src/service/ExportService.cpp`）一次 SELECT、流式或全量写 row 到 Excel，**完全不感知 `lookups[]`**。SqlBuilder 在 import 路径中也只用 `lookups` 做 payload 注入，**导出时 SqlBuilder 把 lookup-derived dbColumn 当成普通列投影**（因为它们已经被写入 DB，长得像普通列）。

要实现反向 lookup，需要在 export 路径插入一层"列翻译"：
1. 主 SELECT 不再投影 `lookups[*].select` 的 dbColumn（H 列），改为只投影非 lookup-derived 的列；
2. 跑主 SELECT 前对每个 lookup-identity 做一次 prefetch 把 G 读到内存 map；
3. 主 SELECT 的每行流到 Excel 之前，用 H 的 dbColumn 值查 prefetch map，把 `match` 字段（A 列）的值取出来填到对应 Excel header；
4. cardinality 检查与 `exportOnMissing` 策略；
5. 输出 Excel 时把 A 作为 header 出现，H 不出现。

这个改动**与 import 路径完全对称**——可以最大程度复用 import 端的 `LookupPrefetcher` / `LookupResolver`（如果已模块化的话）的设计，只是箭头反向。

## Goals / Non-Goals

**Goals:**
- 让"导入→导出"在声明了 `lookups[]` 的 profile 上做到无损往返；
- 复用 `lookups[]` 声明，**不**新开 `reverseLookups`；
- 保留 import 严格 1-对-1 语义；export 默认对称严格，但允许 per-lookup 放宽；
- 复用 import 端的 batch prefetch + identity 合并优化（同一 identity 跨 route / class 共享）；
- 与 `add-export-column-order`、`add-time-format-profile` 两个并行 change 协同（顺序无关，但 spec 中要点明）。

**Non-Goals:**
- 不在反向 lookup 中支持"链式反查"（即 lookup A 的输出当作 lookup B 的 match-key）—— row-lookup spec 已显式禁止链式，本变更继承；
- 不引入"反向方向才声明的字段"（如反向特有的 select），保持声明 100% 对称；
- 不动 import 方向的任何已固化语义；
- 不引入对 G 表写操作（反查就是 SELECT）；
- 不支持"H 列既保留又添加 A 列"——本 change 走"替换"语义（见 D3）。
- 不在本 change 处理"G 表跨数据库文件"——row-lookup spec 已规定 G 必须在同一 SQLite 数据库文件，沿用。

## Decisions

### D1: 复用 `lookups[]` 声明，不开 `reverseLookups`

**选**：同一份 `lookups[]` 数组在两个方向上对称使用，仅新增两个**导出方向才生效**的可选字段 `exportRoundtrip` 与 `exportOnMissing`。

**为什么**：
- spec 层面"无损往返"的对称性是这个 feature 的核心价值；分裂声明就会出现两边不一致的可能；
- import spec 中的 `match[i] = [G_col, Excel_header]` 已经天然包含"Excel header 名"——这正是反向时要还原到 Excel 的那个 header 名；
- 校验路径几乎为零开销：现有 ProfileValidator 检查的内容（`from` 存在、`match[].G_column` 存在、`select[].G_column` 存在、`name` 唯一等）在导出方向**完全适用**。

**备选**：开 `reverseLookups: []` 段——更灵活但失去对称约束，且需要再写一遍 match/select 解析、再做一遍 schema 校验。否决。

### D2: cardinality 严格默认 + per-lookup 旋钮

**选**：`exportOnMissing` 三选一：
- `"error"`（默认）：K=0 / K>1 均行级错误，row 跳过（同 import）；
- `"null"`：K=0 时把 `match` 的 Excel 列值置 NULL；K>1 仍然行级 `E_REVERSE_LOOKUP_AMBIGUOUS`（多匹配是真问题，不能掩盖）；
- `"skip"`：K=0 时静默写 NULL，**不**报错；K>1 仍然行级错误（同上）。

**为什么**：导出场景"G 已经更新 / 历史数据"是真实存在的，必须允许放宽；但**多匹配**永远是配置或数据问题，三种模式都不能掩盖。

**为什么 K=1 时是确定的、不需要旋钮**：一对一是唯一无歧义的情况，无需选项。

### D3: 替换语义 —— H 列消失，A 列出现

**选**：export Excel 中，被 `lookups[*].select` 命中的 dbColumn（H）默认不再作为列出现；取而代之的是 `lookups[*].match` 中的 Excel header（A）。

**为什么**：
- 这才是真正的对称往返。如果同时输出 H 和 A，再次 import 会触发 `dbColumn naming uniqueness within a route` 校验（row-lookup spec 已有要求）—— import 端拒收，闭环就破了；
- A 是用户原始视图里就有的字段（导入时来自 Excel header），保留 A 才是"对用户来说的 Excel 原样"；
- 用 `exportRoundtrip: false` 的旋钮在少数场景下选择保留 H（详 D4）。

**关键含义**：SqlBuilder 自动生成的 SELECT 在导出方向不再包含 H 列。具体怎么做（见 D6）。

### D4: `exportRoundtrip: bool` 旋钮

**选**：每个 lookup 可选 `exportRoundtrip`（默认 `true`）。`false` 时：
- export 不做反查，**保留 H 列**直接写到 Excel；
- 该 lookup 在 export 方向就像"普通已固化数据"；
- `exportOnMissing` 在 `exportRoundtrip: false` 时无意义——profile 加载期发 info-level 提示（不阻断）。

**为什么**：偶尔有"`lookups` 注入的是衍生派生信息（如计算字段），用户在 Excel 里就是想看派生值不想看原 key"的场景。给逃生口。

### D5: 命名冲突的优先级

**选**：A（lookup 还原出来的 Excel header）若**也是**当前 export 的某个 route 的 `ColumnSpec.source`，则该 route 的 `ColumnSpec` 值优先（即用户显式声明的列优先），反查结果作为"无别的来源时的兜底"。

**为什么**：
- 不破坏现存 profile 中"同一个 Excel header 出现在不同 route 的 ColumnSpec.source"的合法形态；
- 反查的本质是"恢复一份原 Excel 视图"，如果用户主动声明了这列的值来源，就应当尊重；
- 单条 export 行里"A 既有 ColumnSpec.source 也有反查值"的情况实际上极少——通常只在 MultiTable / Mixed 模式 cross-route 时出现。

**形式化**：导出投影时构造 "Excel header → 值来源" 的优先级：
1. 主 SELECT 直接命中（非 lookup-derived）的 ColumnSpec.source；
2. 反查 map 命中；
3. 都没有 → NULL。

### D6: SqlBuilder 不变，ExportService 调整投影集

**选**：`SqlBuilder::buildAutoJoinSelect` **不**新增"排除 lookup-derived 列"的能力；改在 ExportService 层面：
- 调用 SqlBuilder 拿到 SQL 之后，识别哪些 dbColumn 是 lookup-derived；
- 执行查询时，对结果列分两类：`naturalCols`（直接进 Excel）与 `derivedCols`（喂给反查模块、不进 Excel）；
- 反查模块返回的是"Excel header → 值"的 map，与 naturalCols 合并后写 Excel。

**为什么**：
- SqlBuilder 共享给 import / export，硬塞一个"导出时排除某些列"的开关污染严重；
- ExportService 是导出路径的唯一调用方，单点改动；
- 性能上 SELECT 多取几列代价远小于"重做 SQL 生成"。

**例外**：如果以后实测发现"H 列又大又多"导致 SELECT 显著变慢，可作为后续优化引入 SqlBuilder 的 hint —— 本 change 不做。

### D7: prefetch 与 identity 合并对称设计

**选**：导出方向的反查同样按 `(from, match-pairs in order, select-pairs in order)` 三元组作为 identity，相同 identity 的 lookup 跨 route / 跨 class 共享同一份 prefetch 结果。

**SQL 形态**（导出反向）：
```
SELECT <match.G_columns>, <select.G_columns>
FROM <from>
WHERE (<select.G_columns>) IN ((row1_select_vals), (row2_select_vals), ...)
```

K = 主 SELECT 结果中所有行**去重**后的 select-values 元组数；分块同 import（一次 SELECT 处理 chunk_limit 行）。

**关键差异**：
- import 侧 prefetch 在**读 Excel 之后、写 DB 之前**触发——可以一次扫 Excel 拿到所有 match-key；
- export 侧 prefetch 必须在**主 SELECT 跑完之后**触发——需要先知道 H 列的所有去重值。结果是 export 流水必然变为"先全量加载主 SELECT、再 prefetch、再写 Excel"，**不再是流式**。

**取舍**：与 `add-export-column-order` 的 `columnOrder` 类似——只在 profile 声明了至少一个 `exportRoundtrip: true` 的 lookup 时切换到"全量加载 + prefetch"路径；声明里全部是 `exportRoundtrip: false`（或根本无 `lookups[]`）时保持流式。

### D8: Mixed 模式 per-class 反查

**选**：与 import 的 Mixed 模式语义对称——
- prefetch 在 sheet 范围合并执行（跨 class 同 identity 合并）；
- 但每行的反查解析时**只查该 class 所声明的 lookups**——同一份 prefetch map，不同 class 视角不同。

**为什么**：与 row-lookup spec 的 Mixed 处理一致；用户对"跨 class 行为"的心智模型不会因为方向反过来而变。

### D9: 与 `add-export-column-order` 的协同

如果两个 change 都已 apply：
- `columnOrder` 中**允许**出现 lookup 的 `match` Excel header（A 列）——因为 A 是导出方向的列；
- `columnOrder` 中**不应该**出现 lookup 的 `select` 目标 dbColumn（这些列在 export 中不存在）——profile 加载期校验时需要把这一条加进 `E_EXPORT_UNKNOWN_HEADER` 的反向规则（"不合法的列名"包含"被 lookup 替换掉的 H 列名"）。但**严格说**：`columnOrder` 用的是 Excel header，而 H 是 dbColumn 名——两个命名空间本就不重叠，正常情况不会撞上；只有当用户某 ColumnSpec.source 恰好等于某 H 的 dbColumn 名时才出问题，这种几率极小，且 row-lookup spec 已规定 H 与 ColumnSpec 命名不能撞（dbColumn uniqueness），所以基本不会发生。
- 顺序应用：`time-format` → `export-column-order` → `add-export-reverse-lookup` 是建议顺序（也是 explore 阶段定下的优先级）；但实际任何顺序都能 apply，因为三者代码改动相互独立。

### D10: 与 `add-time-format-profile` 的协同

- 反查还原回 A 列后，A 列若声明在 `dateFormat` 等时间槽下，时间格式化照常进行（反查只决定值的来源，不决定值的格式化）。spec 在 Scenario 中举例说明。

## Risks / Trade-offs

- **[Risk] 默认行为变化（H 列消失、A 列出现）会让存量用户的导出文件结构改变**
  → Mitigation：spec 文档中**明显标注**这是默认行为变化，并写出"一行 `exportRoundtrip: false` 立即回到旧行为"的迁移路径；CHANGELOG / README 中加显著提醒。
  → 进一步缓解：若团队不接受默认变化，可以把 `exportRoundtrip` 默认改为 `false`（用户需显式 opt-in 才能享受反向 lookup）。**当前选择 `true`**——理由：声明 `lookups[]` 的用户已经明确表达了"H 是派生的，A 是源"，默认还原才符合声明意图。如果决策反转，spec 与 proposal 同步更新即可。

- **[Risk] 主 SELECT 不再流式**
  → Mitigation：只有 `exportRoundtrip: true` 的 lookup 至少存在一个时才切换路径，零 lookup / 全 false 时保持流式（同 columnOrder 的策略）。

- **[Risk] K = 0（无 G 行）的批量 prefetch 没用，K = N（全部 distinct）会跑全表**
  → Mitigation：与 import 侧 prefetch chunking 完全对称——分块 SELECT；K 极大时 G 表本身就大，这是用户的数据规模问题，非本 change 引入。

- **[Risk] G 表有 NULL 或唯一性问题（H 列同值多行）→ 反查 K>1，整批行级错误**
  → Mitigation：与 import 侧"strict cardinality"语义对称，错误码 `E_REVERSE_LOOKUP_AMBIGUOUS` 明确指出"检查 G 中 select 列的唯一性"。

- **[Trade-off] 不引入 reverseLookups 段**
  → 选择对称简洁优于"两个方向各管各的"；如果未来出现"导出方向需要完全不同的反查表"的场景，再独立 change（届时可声明 `inverse: { from, match, select }` 等）。

- **[Trade-off] 不在 SqlBuilder 中排除 H 列**
  → SqlBuilder 多取列的性能损耗远小于增加 hint 接口带来的复杂度；保留单点修改。

## Migration Plan

1. 不写新字段、不动 `lookups[]` 的存量 profile → 应用本 change 后**导出 Excel 结构会变化**（H 消失、A 出现）；如果不接受，加 `exportRoundtrip: false` 到对应 lookup 即可保持旧行为。
2. 想充分享受新能力的用户：什么也不用改（默认即开启）。
3. 数据迁移：**不需要**——DB 中 H 列保持不变，仅 export 行为改变。
4. 回滚：禁用本 change 后所有现存 profile 维持现有行为（H 列照旧导出）；profile 中新增的 `exportRoundtrip` / `exportOnMissing` 在旧版本上会被忽略或加载报错——倾向"加载期未知字段一律忽略 + warning"，便于跨版本兼容（如果当前 ProfileLoader 对未知字段是严格的，需要在 task 中考虑这点）。

## Open Questions

1. `exportRoundtrip` 默认值 = `true` 是否是团队共识？— 我倾向 `true`（与 row-lookup spec 的"声明即承诺 H 是派生"语义一致），但这是一个**默认行为可见变化**，可能需要项目维护者拍板。归到 task 提交前同步给用户确认。
2. 主 SELECT 结果中"哪些 dbColumn 是 lookup-derived"的判定能否完全在 ExportService 里基于 ProfileSpec 反推？—— 应该可以：`profile.routes[*].lookups[*].select[*].second` 即对应的路由 payload 目标 dbColumn。但需要谨慎处理 Mixed 模式下"哪个 class 的 lookup 在生效"。归到 task。
3. `exportOnMissing: "skip"` 与 `"null"` 的细微差异需要在 spec 中举例描述清楚——已涵盖在 D2，但 spec 还要补一个"行其它列依然写出"的 scenario。✓ 已纳入 specs。
