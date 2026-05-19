## Context

当前导入流水线（参见 `src/service/ImportService.cpp`）按"开 xlsx → 读 header → 逐行解析 → 路由分发 → fkInject → preflight → SqlBuilder UPSERT"线性执行。两个新诉求会改变这条流水线的形态：

1. **lookup**：行解析阶段需要"附带从库内另一张参考表 G 拿一组字段"。如果按"逐行 1 SQL"实现，1 万行就是 1 万次 round-trip，不可接受。同时 G 表可能根本不在当前 profile 的 routes 集合内，`SchemaCatalog` 现有加载逻辑也不会主动拉它的列定义。
2. **fkInject 多列 + 多父表**：当前 `FkInjectSpec` 是 `optional<single>`、`ForeignKeyPreflight::check` 是单列 `WHERE c=?`、`doFkInject` 取首列覆盖 conflictVals[0]。这些假设全部要拆。

两者的共同 surface 是 `ProfileSpec` 与 `RoutePayload`。再加上"lookup 输出字段允许作为 fkInject.from"（两跳）让两者在内存数据流上耦合，合并到一个 design 比拆两个干净。

约束：

- 单一 `QSqlDatabase` 句柄，G 表与目标表必须在同一个 SQLite 文件内（用户决策 Q1）
- Excel 流式 reader 现状不支持原地 rewind；要实现"两遍扫"要么缓存整张 sheet 到内存，要么 `reader.open()` 再来一次
- 现有 fixture profile 与 docs 示例使用旧 `fkInject` 写法，必须随本 change 一起改写（无兼容期）

## Goals / Non-Goals

**Goals:**

- 一次 SQL 取回每张 G 表所需的全部 lookup 结果，prefetch 失败立即 fatal（不为单行重试）
- lookup 拿回的字段只在声明它的 route 内可见；要跨 route 流转必须显式经由 `fkInject.from`
- `fkInject` 升级后表达力涵盖：单列、多列（复合业务键）、多父表（同一子行同时引用多张父表）
- preflight 复合化但保持"批内命中即免 SQL"的优化路径
- 错误语义保持现有风格：能定位到 row/column 的失败 → `ErrorCollector` 行级错误；能定位到表/sheet 的失败 → 表级错误并 abort

**Non-Goals:**

- `ATTACH DATABASE` / 跨 SQLite 文件 lookup
- 非 SQLite 数据源 lookup（HTTP、其他 driver）
- lookup 级联（lookupA.select 作为 lookupB.match key）—— 由"lookup → fkInject"两跳满足
- 自增主键回填式 fkInject
- "命中 N 行取首行" 宽松模式
- 旧 `fkInject:{from,to}` 单列写法的解析兼容层 / 自动迁移脚本

## Decisions

### D1. lookup 在 RouteSpec 上声明，使用数组形式

```cpp
struct LookupSpec {
    QString name;                                  // 仅用于错误信息
    QString fromTable;                             // G
    QVector<QPair<QString, QString>> match;        // [(G.col, Excel header)]
    QVector<QPair<QString, QString>> select;       // [(G.col, route-local dbColumn)]
};

struct FkInjectSpec {
    QString fromTable;                              // 父表
    QVector<QPair<QString, QString>> pairs;         // [(parent.col, child.col)]
};

struct RouteSpec {
    QString table;
    QString parent;
    ConflictSpec conflict;
    QVector<FkInjectSpec> fkInject;                 // 数组：多父表
    QVector<LookupSpec>   lookups;                  // 数组：多 lookup
    QVector<ColumnSpec>   columns;
};
```

**为什么数组而不是 dict（map name→spec）？** JSON 对象在 Qt JsonObject 里 key 顺序不保证；将来若想引入"按声明顺序级联 lookup"会麻烦。数组+`name` 字段（必填，route 内唯一，用于错误信息）成本更低且向前兼容。

**为什么 `match` / `select` 的 wire format 也是数组 `[[a,b],...]` 而不是 dict `{a:b,...}`？** 与 `fkInject.pairs` `[[parent.col, child.col]]` 严格同构（同向：左源右目标），用户写两个功能不需切换思维；JSON 文本里顺序显式可见；并且永久消除"Qt `QJsonObject` key 顺序不稳"对复合 match / 多列 select 的影响（顺序决定 prefetch SQL 列序、in-memory tuple 列序、row-time tuple 构造列序）。ProfileLoader 直接按数组下标遍历，比 dict 顺序排序更直接。

**为什么 `match` 是 `(G.col, Excel header)` pair 而不是 `(Excel header, G.col)`？** 与 `fkInject.pairs` `(parent.col, child.col)` 朝向一致——左侧永远是"数据源（外部）一侧"，右侧永远是"目标（本 route）一侧"。

### D2. lookup 数据只对 declaring route 可见，但允许"经 fkInject 二跳"

- lookup 把 `(G.col → route-local dbColumn)` 的结果直接 append 到 `RoutePayload.dbColumns / binds`，与 `columns:` 映射结果共享同一个命名空间。**route 内 dbColumn 不能重名**（validator 阻止）。
- 其它 route 不能直接读取这些列；要拿到唯一合法的路径是声明 `fkInject: [{ from: <declaring_route.table>, pairs: [[<lookup_dbColumn>, <child_dbColumn>]] }]`。`doFkInject` 实现上反正就是从 `RoutePayload` 里取列，不区分该列源自 Excel 映射、lookup 还是上一跳的 fkInject，所以两跳天然支持。
- 文档上明确写："lookup 字段对其它 route 不可见。如需在子 route 使用，请用 fkInject 显式传递。"

### D3. lookup 走两遍扫 + 批量 prefetch + identity 归并 + Mixed 模式作用域

新增 `Phase A.5` 在 `ImportService::run` 中位于 Phase A（open + header）与 Phase B（逐行解析路由）之间：

```
Phase A.5  收集所有 lookups（含 Mixed 模式下 classes 各 route 内声明的）
  ├─ 按 lookup-identity = (from, match-pairs 有序列表, select-pairs 有序列表) 三元组归并等价类
  │    两个 route / 两个 class 声明同 identity 的 lookup → 共享一份 prefetch 结果
  ├─ 第一遍扫 Excel（sheet 全局，Mixed 模式也是一次性扫整张）：
  │    for each row, for each lookup-identity:
  │      取每个 match.G_col 对应的 Excel header 值
  │      按 G 列 affinity 做强制 cast（TEXT→toString / INTEGER→toLongLong / REAL→toDouble / 其它原样透传）
  │      若任一 match 列 empty/null（isNull 或 trim 后空串）→ 该 identity 在本行无贡献，row-time 才报 E_LOOKUP_KEY_EMPTY
  │      若任一 match 列 cast 失败 → 该 identity 在本行无贡献，row-time 才报 E_LOOKUP_KEY_INVALID
  │      其余 → 拼成 key tuple → 加入 keySet[identity]（自动去重）
  ├─ 对每个 identity（按声明顺序）：
  │    K = keySet[identity].size()
  │    if K == 0: 0 个 SELECT，跳过
  │    else: 按 chunk_limit 切分 ceil(K / chunk_limit) 次 SELECT
  │      SQL: SELECT <match cols>, <select cols>
  │           FROM <G>
  │           WHERE (<match col1>, <match col2>, ...) IN ( (?,?,...), (?,?,...), ... )
  │      （SQLite 支持 row-value IN；只 1 match col 时退化成普通 IN）
  │    每次 SELECT 触发 prefetch query counter callback（D10）
  │    结果建 QHash<key-tuple, QVector<select-values>>
  │    并记录每个 key 命中次数（>1 标记 ambiguous，用于第二遍 row-error）
  └─ 任一 SELECT 失败 → 表级错误 E_LOOKUP_QUERY_FAILED，abort 整张 sheet

Phase B  正式逐行：
  对每行：
    if Mixed: 用 discriminator 解析出 classId；选择该 class 的 routes
    else:     用顶层 routes
    对每个声明 lookup 的 route：
      取 lookup 所属 identity（与 prefetch 同一等价类）
      重新读 match key 并做同样 cast（确定性，与 prefetch 阶段一致）
      ├─ cast 失败 → row-error E_LOOKUP_KEY_INVALID
      ├─ 任一 match 列 empty/null → row-error E_LOOKUP_KEY_EMPTY
      ├─ map miss → row-error E_LOOKUP_NOT_FOUND
      ├─ 命中次数 > 1 → row-error E_LOOKUP_AMBIGUOUS
      └─ 命中 == 1 → append select 列到 RoutePayload（NULL 值原样透传，不报错）
    所有错误 attribute 到 (excelRow, classId-if-Mixed, routeTable, lookupName)
```

**为什么不做"逐行 + 进程内 LRU"？** 用户决策 Q4 明确选两遍扫。理由：(a) prefetch 一次失败比 N 次单行失败更易定位；(b) 命中数计数（用于 ambiguous 检测）天然要全量结果；(c) identity 归并需要全量去重，预取模型最直接。

**第一遍扫的实现选择留给实现期**：可以缓存整张 sheet 行 vector 到内存复用，也可以 `ExcelReader.open()` 再来一次。两种都不改本设计的契约。

**IN-list 太大怎么办？** SQLite 默认 `SQLITE_MAX_VARIABLE_NUMBER` 较高（999 或 32766），万行级业务键够用；逼近上限时按 chunk 分批后 merge map。chunk 数体现在 query counter 上（spec R2 Sc1 用 `ceil(K / chunk_limit)` 表达）。

**Mixed 模式为什么 prefetch 仍 sheet 全局？** 不需要先跑 discriminator 才能 prefetch。多取若干"本不属于本 identity 的 key"对 map 无副作用（map 是只读、按 tuple 命中）；换来的好处是实现简单（不需要 prefetch 与 discriminator 解析交错），且 identity 归并自然覆盖跨 class 同 lookup 的情况。

**等值比较的归一化政策**：cast 之外**不做任何归一**（无 case-fold、无 trim、无全/半角折叠）。trim 仅在"判断 match key 是否 empty"那一步使用，equality 比较仍走原始 cast 后值。这一政策对 prefetch SQL 与 in-memory map 都适用，由 spec 的 "Strict equality at match time" requirement 锁定。

### D4. lookup 拿回的列豁免 FK preflight（group-level）

`ForeignKeyPreflight` 现在的角色是"检测子表里某个 fkInject 注入值是否能在父表的对应列里找到"。当 `fkInject.from` 实际指向 **lookup-declaring 的同 route**（D2 的两跳路径）时：

- "父行" 在数据库里并不直接存在 —— 它是 lookup 阶段从 G 表取回、本 batch 才生成的中间值；
- lookup 自身在 prefetch 阶段已经验证了"该 key 在 G 中存在"，再 preflight 一次是冗余且多半会查不到（lookup-declaring 的 route table ≠ G）。

**豁免粒度为 group，不是 pair**。判定规则：preflight 时按 `fk.fromTable` 在 `allRoutes` 中查 RouteSpec；若该 route 含 lookup，**且本 group 内每一个 pair 的 `parent_column` 都命中该父 route 的 lookup.select target 集合**，则整个 group 跳过 SQL probe（batch 内命中仍按原逻辑）。

**为什么不允许 pair-级混合？** R4 的复合 WHERE 是"所有 pair 一条 SQL"，partial WHERE 会让被抑制列从"已验证"退化为"未验证但被信任"——是定时炸弹。validator（见 D7 规则 #4）在 profile 加载期就禁止"同一 group 内混合 lookup-derived 与非 lookup-derived"，强迫用户拆成两个独立 group，每个 group 形态干净。

### D5. ForeignKeyPreflight 复合化

```sql
-- 单列保持不变
SELECT 1 FROM <parent> WHERE c = ? LIMIT 1
-- 多列
SELECT 1 FROM <parent> WHERE c1 = ? AND c2 = ? [...] LIMIT 1
```

batch-内命中优化保留并修正一个隐含 bug：

- 旧实现：`batchParentKeys[table]` 是 `QVector<QVariant>`，从 `payload.conflictVals[0]` 取值。这能用是因为旧 fkInject 单列时 fk-from 列总是恰好等于父表的 conflict key 子集（happy accident）。
- 新实现：改成 `QHash<QString, QVector<QVector<QVariant>>>` 存 tuple，但 **tuple 的值不能再从 `conflictVals` 取**——它要从 `parentPayload.binds` 里按 fk pair 中 `parent_column` 的列名定位下标取。理由：多父表 / 多列场景下，fk-from 列与父表的 conflict key 不一定有关联（举例：`fkInject from ref_tenants pairs [[tenant_id, tenant_id]]`，而 `ref_tenants` 的 conflict key 可能是 `id`）。

形式化：

```cpp
// 构建阶段（按父表分桶）
for (each parent payload P) {
  for (each fkInject group `fk` of every child route referencing P.table) {
    QVector<QVariant> tuple;
    for ((pCol, _) in fk.pairs)
      tuple.append( P.binds[ P.indexOf(pCol) ] );    // 从 binds 取，不是 conflictVals
    batchParentKeys[P.table].append(tuple);
  }
}
// 命中阶段
QVector<QVariant> needed;
for ((_, cCol) in fk.pairs)
  needed.append( child.binds[ child.indexOf(cCol) ] );
return batchParentKeys[fk.fromTable].contains(needed);   // tuple equality
```

注：`batchParentKeys` 的预构建可按"按需 lazy 填充"实现，但语义等价。

### D6. doFkInject：双重循环 + 严格列对齐

```cpp
for (route in routes) {
  for (fk in route.fkInject) {                    // 多父表
    parentPayload = payloads[ table→idx[fk.fromTable] ];
    for ((pCol, cCol) in fk.pairs) {              // 多列
      v = parentPayload.binds[ parentPayload.indexOf(pCol) ];
      idx = childPayload.indexOf(cCol);
      if (idx >= 0) childPayload.binds[idx] = v;
      else          append cCol/v to childPayload;
      // 同步更新 conflictVals：找到 cCol 在 conflictKey 中的位置
      ci = childPayload.conflictKey.indexOf(cCol);
      if (ci >= 0) childPayload.conflictVals[ci] = v;
    }
  }
}
```

注意 conflictVals 现在按 conflictKey 列顺序逐位写入，不再假定 index 0；保证多列业务键的 UPSERT 路径正确。

### D7. ProfileValidator 增量

每个 route 校验序列加入（按建议执行顺序）：

1. 每个 `lookup.fromTable` 在 SchemaCatalog 中存在
2. 每个 `lookup.match.first` 在 G 表列集合内；`lookup.match.second` 在 Excel sheet header 内（不允许把另一 lookup 的输出当 match key —— 即"无级联"）
3. 每个 `lookup.select.first` 在 G 表列集合内；`lookup.select.second` 与 (a) 本 route 的 `columns` dbColumn (b) 其它 lookup 的 select.second (c) 同 route 任一 fkInject pair 的子列名 都不冲突
4. 每个 `fkInject` group 的 `fromTable` **必须命中本 profile 中某个 route 的 `table`**——只在 SchemaCatalog 中存在但未声明为 route 的表不被接受（建议用户改写为 `lookups`）。错误消息要把这一替代方案指出来
5. 每个 fkInject pair 的 `parent_column` 在父 route 的 columns 或其 lookup 输出中存在；`child_column` 在 SchemaCatalog 中本 route 的目标表里存在
6. **同一 fkInject group 内，pairs 不能混合 lookup-derived 与非 lookup-derived 的 `parent_column`**。group 要么全 lookup-derived（→ R5 group-level 跳过 preflight），要么全非 lookup-derived（→ 走完整复合 WHERE）。违反时错误消息要建议拆成两个 group
7. **同一 route 的所有 fkInject groups 合并起来，每个 `child_column` 最多被一个 pair 注入**。重复目标必须拒绝（避免"后写覆盖"的隐式语义）
8. 至少 1 pair；空 group 对象拒绝（spec 中 R1 Sc4）
9. `fkInject` 字段缺失、空数组 `[]`、`null` 三者等价为 no-op，不报错（spec 中 R1 Sc5）

**lookup 相关新增（row-lookup spec 对应 requirement）**：

10. 每个 lookup 的 `name` 非空（trim 后非空）；同 route 内 `name` 不重复
11. 每个 lookup 的 `select` 数组内部，target 名（pair 第二个元素）不重复
12. `match` / `select` 必须是 JSON 数组形式 `[[a,b],...]`；遇到 JSON object 形式 → 拒绝并提示数组形态
13. dbColumn 唯一性的三方校验（Excel `columns:` / lookup `select` target / fkInject `pairs` 的 child_column）由 row-lookup spec 的 "dbColumn naming uniqueness within a route" requirement 统一承担。fk-injection 规则 #7（D7 上面那条）只看 fkInject 内部 child_column 重复

实现建议：在 validator 中维护一张"本 route 各 dbColumn 来源"小表（Excel / lookup / fkInject），规则 3 / 6 / 7 / 13 都可以在一遍扫里完成；name 唯一性用小 set 单独走。

### D8. 错误码语义

| 错误码 | 触发 | 级别 | 进 ErrorCollector 还是 fatal |
|---|---|---|---|
| `E_LOOKUP_KEY_EMPTY` | 某行 match key 列在 Excel 为空 | row | row-error，行内其它 routes 继续 |
| `E_LOOKUP_NOT_FOUND` | prefetch map miss | row | row-error |
| `E_LOOKUP_AMBIGUOUS` | prefetch 命中 >1 | row | row-error |
| `E_LOOKUP_QUERY_FAILED` | prefetch SQL 失败（语法/权限/IO） | table | fatal，整张 sheet abort |
| `E_LOOKUP_KEY_INVALID` | row 的 match key 在 cast 到 G 列 affinity 时失败（如 "abc" → INTEGER）| row | row-error；该 key 不进 prefetch IN-list |
| `E_VALIDATE_FK`（已存在，复用） | preflight 找不到父行（多列同等适用）；fkInject 注入值为 NULL | row | row-error |

prefetch 阶段不向 row 报错（因为 row 还没被解析）；它只能产生 fatal `E_LOOKUP_QUERY_FAILED`。"命中数 >1" 也在 prefetch 阶段就能确定，但 row-level 的错误延迟到 Phase B 才报，以便 ErrorCollector 能记录到具体行号。

**NULL 在 fkInject 链路上的严格策略**（spec 中的 NULL 行级错误 requirement）：注入时若父 payload 的 `parent_column` 为 NULL/`isNull()`，行级 `E_VALIDATE_FK`，不写入 NULL 到子 payload。与 lookup 的"key 空即 row-error"对称。MVP 不开 `nullable:true` 后门；若未来真有"父键允许 NULL"的合法场景，再引入显式声明。

### D10. 测试可观察性：双 query counter + dryRun

spec 中多处断言"执行 N 次 / 0 次 SQL"——若不暴露 hook 则不可测。为支持这些断言，引入两条独立的可注入 callback：

- **`ForeignKeyPreflight` 注入** `std::function<void(const QString& parentTable)>`：每次实际 SQL probe 触发一次（用于 fk-injection R4 Sc2 in-batch 命中、R5 group-level skip）
- **`ImportService` Phase A.5 注入** `std::function<void(const QString& identityKey)>`：每次 prefetch SELECT 触发一次（用于 row-lookup R2 Sc1 ceil(K/L)、K=0 跳过、identity 归并断言）

两个 counter 互相独立、不共用；production 期默认 noop，test 期注入计数器。`identityKey` 建议形如 `"<from>::<match-cols-joined>::<select-cols-joined>"`，方便测试做按 identity 分组断言。

为支持 "lookup 输出未 append"、"sibling 不可见"、"级联抑制下子 payload 被 drop"、"lookup 注入 + 复合 fkInject 端到端 payload 形态" 这类 spec 断言（row-lookup 多个 scenario / fk-injection 级联 scenario），`ImportOptions` 新增 `dryRun: bool = false`：

- dryRun = true 时所有 prefetch / lookup / fkInject / preflight 仍然执行（保留所有错误路径与 ErrorCollector）
- 但跳过 SqlBuilder 的 UPSERT 执行（不写库）
- `ImportResult` 暴露构建完成的 `QVector<RowContext>`（含每行的 payloads 列表）供测试 inspect dbColumns / binds / conflictKey / conflictVals

dryRun 不是 production feature，但也不需要 `#ifdef` 隔离——它只是一个 option，对正常路径无影响。docs README 加一段简短说明，但不鼓励用户在生产里依赖它。

### D11. 行级错误的级联抑制

ImportService 在路由分发后，对每个 Excel 行维护一个"本行已失败的 route 集合"。任一 route 触发 row-level error（lookup 三种、fkInject 注入 NULL、FK preflight 缺失）→ 把该 route 加入集合 → 后续 `doFkInject` / `ForeignKeyPreflight` / SqlBuilder 阶段在处理本行 payload 前先按 `parent` 链向上查：祖先有失败者，**本 route 的 payload 整体 drop，不再报二次错误**（避免 ErrorCollector 充斥根因相同的级联错误）。

具体规则：

- 级联按 `RouteSpec.parent` 的字符串链向上传播，不按 `fkInject.from` 关系。理由：parent 关系是显式声明的"业务从属"，与用户心智一致（"orders 那行没成，order_items 这行别动"）；fkInject.from 的精确传播更细但太隐晦。
- 不相关 sibling route（不在同一 parent 链上）不受影响，仍正常处理。
- A 的 lookup 失败 → A 的 payload 在本行被整体丢弃，不只是缺某列；这样 `doFkInject` 的 `tableToPayloadIdx.find(A)` 自然 miss，B 也走 drop 路径。

## Risks / Trade-offs

- **两遍扫的内存/IO 成本**：缓存整张 sheet 到内存对万级行可接受，但十万级行会显著吃内存。**Mitigation**：实现期把"全量缓存 vs 二次 `reader.open()`"做成 ImportService 内部可切换的策略；spec 不锁死。
- **SQLite IN-list 上限**：极端业务键基数下需分批。**Mitigation**：实现期按 `SQLITE_MAX_VARIABLE_NUMBER` 做 chunk，对 spec 透明。
- **lookup 与多 fkInject 的 schema 复杂度**：用户编写 profile 难度上升。**Mitigation**：docs/validation/row-to-multitable.md 配最小例子 + 完整例子各一份；FAQ 增"lookup 怎么写"。
- **Breaking schema 影响所有现存 profile**：仓内 fixture/docs 必须一次性改写。**Mitigation**：tasks.md 显式列出全部待改文件，verify 阶段 grep 残留 `"fkInject": {`。
- **lookup-derived from 跳过 preflight 的隐式约定**：用户可能误以为子 route 仍受 FK 保护。**Mitigation**：在 spec 与 docs 都明确写出豁免规则与"lookup 已经在 prefetch 保证 key 存在"的因果。
- **prefetch 阶段的 `(c1,c2) IN ((?,?),(?,?))` row-value IN 语法**：SQLite ≥ 3.15 支持；项目最低 SQLite 版本？需 cross-check。若需兼容低版本，实现期降级为 `WHERE (c1=? AND c2=?) OR (...)` 拼接（仍批量、仅 SQL 形态变化）。

## Migration Plan

本 change 是一次性 breaking。步骤：

1. 代码侧：实现新 ProfileSpec / Loader / Validator / ImportService / ForeignKeyPreflight
2. 同 commit 改写 `tests/data/profiles/*.json` 所有 `fkInject` 字段为数组形式
3. 同 commit 改写 `README.md`、`docs/validation/row-to-multitable.md`、`specs/MVP-Qt-SQLite-Excel-批量导入导出-实现设计.md` 中的所有示例
4. 在 `README.md` 的"Profile 字段"章节顶部加一条 BREAKING 说明，告诉外部使用者旧 schema 已不被解析
5. CI / verify 阶段：`rg '"fkInject"\s*:\s*\{'` 残留命中即 fail（防止任何文件漏改）

无 rollback 策略：旧 schema 解析能力被删除，回滚需 revert 整个 change。

## Open Questions

无 —— 关键决策（Q1–Q5 + 默认 + 第二轮 grill 后的 8 条 Tier 1/2 修正）均已落档。实现期可能浮现：SQLite 最低版本是否支持 row-value IN（D5 / Risks 已留 fallback 路径），以及第一遍扫的具体实现策略（D3 已说明留给实现期）。
