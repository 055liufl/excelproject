## Context

`ExportService` 的两条分支今天对"输出列顺序"有截然不同的实现路径：

- **SingleTable / MultiTable 分支**（`ExportService.cpp:164-186`）：调用 `SqlBuilder::buildAutoJoinSelect` 生成一条 `SELECT route.col AS source, ...` 的 SQL，列顺序 = SQL 字段顺序 = ColumnSpec 声明顺序。然后 `execAndWrite` 直接把 `q.record()` 的字段名作为 Excel header，按下标顺序写每行。
- **Mixed 分支**（`ExportService.cpp:74-163`）：每个 class 各自生成一条 SQL 跑一次，拿到结果后用一个 `headerSet + allHeaders` 按"首次出现"合并所有 class 的列；`classColumn`（如声明）`prepend` 到首列；然后对每行按 `allHeaders` 的顺序投影写出。

因此本变更需要在**导出输出层**插入一个"按用户声明重排列"的步骤，而不是动 SqlBuilder（让 SQL 继续是其内部决定的顺序，输出层独自负责面向用户的列顺序）——SqlBuilder 已经被导入路径/拓扑排序等共享，硬塞 columnOrder 风险大。

`ColumnSpec.source` 就是 Excel header 名（与 `validators`、`dbColumn` 同级），用户在表里能直接看到，因此自然适合作为 `columnOrder` 中的标识。

## Goals / Non-Goals

**Goals:**
- 让用户用 Excel header 名直接表达列顺序；
- Mixed 模式下行为可预测：先用户声明序、其次首次出现序；
- `classColumn` 位置可预测且可显式控制；
- 与 `explicitSql` 的边界明确（互斥）；
- 与 `orderBy` 正交：两件事两条规则，互不干扰；
- 实现集中在 `ExportService` 一层，不改 `SqlBuilder` 的接口。

**Non-Goals:**
- 不重新设计 `ColumnSpec` 写法（不引入"按数组形式声明 columns"）——会拽出大量迁移面；
- 不动 SqlBuilder 的 SELECT 投影逻辑——它仍按今日规则产出 SQL；
- 不引入 per-class 的 `columnOrder`（Mixed 模式只支持 profile 级一份）；
- 不支持"显式标记某列不导出"——本 change 不做隐藏，只重排；
- 不对 `orderBy` 做任何语义改动。

## Decisions

### D1: 重排发生在 `ExportService` 输出层，不动 SqlBuilder

**选**：`SqlBuilder` 维持现状；`ExportService` 在拿到 `QSqlQuery::record()` / `allHeaders` 之后，构造一个"原序列 → 目标序列"的索引重排映射，按目标顺序输出 header 行与每行数据。

**为什么**：导出列顺序是面向用户的视图问题；SQL 是内部表达。两者分层。`SqlBuilder` 已被其他路径共享，扩展它的输出形状会污染。

**备选**：在 `SqlBuilder::buildAutoJoinSelect` 中按 `columnOrder` 生成 SELECT 字段顺序。否决——若 `columnOrder` 部分覆盖列，仍要在内存中补"未列入列"，复杂度并没省。

### D2: 标识使用 Excel header（`ColumnSpec.source`），而非 dbColumn

**选**：`columnOrder` 中每一项匹配某 `ColumnSpec.source`。

**为什么**：用户在 profile 里写 `columns: { db_col: { source: "OrderNo", ... } }`，"OrderNo"是用户看得见的、SQL 中通过 `AS source` 也用了同样的名字（`SqlBuilder.cpp:61`）。dbColumn 是内部名。

**结果**：`ExportService` 用 query 的 `record().fieldName(i)` 来匹配——而 SqlBuilder 已经把字段 alias 成 `source`（见上面），所以可以直接对 record 字段名做查找。Mixed 同理（headers 也来自 `record.fieldName`）。

### D3: 未列入列追加到末尾，未知 header 加载期硬报错

**选**：
- `columnOrder` 是部分列表 → 未列入的列按"原有顺序"追加在末尾；
- `columnOrder` 出现了没有任何路由 `ColumnSpec.source` 匹配的字符串 → 加载期报错 `E_EXPORT_UNKNOWN_HEADER`，附上未匹配的串。

**为什么**：宽容追加避免"加了一列就让旧 profile 炸"，但 typo 不能容忍——一旦放过就是静默漂移。两条规则方向相反但目的一致：让用户对自己写过的东西负责，对没写过的从宽。

**Mixed 模式下"原有顺序"的精确含义**：当下 ExportService 是按 class 顺序遍历 + 首次出现合并；本变更保持这个语义不变——未列入的 header 按"先看 columnOrder 之外的部分要不要插，再按既有 allHeaders 顺序中未被列入的那些拼回去"的方式追加。

### D4: `classColumn` 默认首列；显式列入 `columnOrder` 则按 `columnOrder` 位置

**选**：
- Mixed 模式且 `classColumn` 不为空且**未出现**在 `columnOrder` → 沿用今日行为（`prepend` 首列）；
- `classColumn` **出现**在 `columnOrder` 中 → 按用户在 `columnOrder` 中的位置摆放，不再 `prepend`。

**为什么**：今天的"`prepend` 首列"是一个隐式默认，保留它不破现状；但用户既然写了 `columnOrder` 把 classColumn 列入其中，必然想自己控制位置。

**校验**：`classColumn` 若出现在 `columnOrder` 中，加载期不需要它匹配任何 `ColumnSpec.source`——它是个合成列，由 ExportService 注入；ProfileValidator 在做 `E_EXPORT_UNKNOWN_HEADER` 校验时要把 `classColumn`（如声明且非空）加入"可接受标识"白名单。

### D5: `explicitSql` 与 `columnOrder` 互斥

**选**：profile 加载期若同时声明则报 `E_EXPORT_ORDER_WITH_RAW_SQL`。

**为什么**：原生 SQL 是逃生口，用户负责一切包括列顺序；再让 `columnOrder` 做"事后重排"会让 SQL 阅读人员困惑——SQL 里看到的字段顺序未必是 Excel 里看到的。两件事走开，明确边界。

**实现复杂度**：极低——一次 `!sql.isEmpty() && !columnOrder.isEmpty()` 检查即可。

### D6: 与 `orderBy` 完全正交

**选**：`orderBy` 不变，`columnOrder` 不变，两者各管各。

**为什么**：一个是 row 维度（SQL 的 `ORDER BY`），一个是 column 维度（Excel 列序）。spec 里点明即可，不需要任何耦合代码。

### D7: 重排在哪一步发生（Mixed 分支细节）

**选**：对 Mixed 分支，重排发生在 `allHeaders` 构造完成且 `classColumn` 处理完毕**之后**的"最终列表确定"步骤。该步骤产生一个最终的 `QStringList finalHeaders` 与一个 `QHash<QString, int> oldToNewIndex`（实际上更简单：直接根据 finalHeaders 在 row 写出时按 header 名查 `mr.data` 即可，因为 mr.data 已是 `QHash<QString, QVariant>`）。

对 SingleTable/MultiTable 分支，重排发生在 `execAndWrite` 之内（或拆出一个 `writeReorderedRows` helper）：先收集所有 row 到内存的 `QVector<QVector<QVariant>>`（每行按 `record` 顺序），再按 `finalHeaders` 输出。

**内存开销提示**：SingleTable/MultiTable 当前是流式写——`execAndWrite` 一边 `q.next()` 一边 `writer.writeRow`。引入重排意味着必须先把所有行读到内存才能写。Mixed 分支今天本来就是这样（已经把所有行读到 `allRows`），所以 Mixed 不增加开销；SingleTable/MultiTable 会从"流式"变成"先全量加载"。

**取舍**：对当前规模（用户场景是批处理、表通常 < 几十万行），全量加载可接受；要保留流式选项的话，可以在 `columnOrder` 为空时走老路径（流式），声明了再切换到内存重排路径。**采用此方案**：profile 未声明 `columnOrder` 时维持流式，零开销。

## Risks / Trade-offs

- **[Risk] 引入 columnOrder 后 SingleTable/MultiTable 失去流式导出能力**
  → Mitigation：D7 的"仅在声明 columnOrder 时切换到内存重排路径"，未声明则维持流式，对存量 profile 零影响。

- **[Risk] 用户在 columnOrder 中写错大小写（Excel header 区分大小写）**
  → Mitigation：`E_EXPORT_UNKNOWN_HEADER` 在加载期即报错，附上"已知 header 列表的前 N 个"作为提示（参考 row-lookup 校验的错误消息风格）。

- **[Risk] Mixed 模式下，某 class 没有"某列入的 header"，列入后该列在那些 class 的行里都是 NULL**
  → 这是**特性而非缺陷**——用户主动把列入了 `columnOrder` 就接受了它跨类的语义；ExportService 今天就是这样工作（同一 `allHeaders` 跨 class 投影，未命中的就是 `QVariant()`）。spec 中要写明这点。

- **[Trade-off] 不引入 per-class columnOrder**
  → 简化心智；如果未来真有需求再独立 change（可以把 profile 级当默认，per-class 覆盖）。

- **[Trade-off] 不支持"显式隐藏列"**
  → 本变更只重排不删除；隐藏列是另一个语义问题（涉及到导入时这些列是否还要 round-trip 等），等真有需求再做。

## Migration Plan

纯增量。已有 profile 不写 `columnOrder` 行为完全等同今日。

回滚：删除 profile 中的 `columnOrder` 字段。代码层面只增不改，下线 columnOrder 字段不影响其它路径。

## Open Questions

1. Mixed 模式中"原有顺序"的精确定义需要 spec 落到一句话——上面 D3 给了候选定义（"按既有 allHeaders 顺序中未被列入的那些拼回去"），需要在 spec 中以 Scenario 形式定下来。✓ 已纳入 specs。
2. `columnOrder` 中是否允许重复元素？倾向**拒绝**（profile 加载期报错），重复无意义且容易误导。归到 spec。
