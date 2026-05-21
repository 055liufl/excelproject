# 验证流程：一行 → 多表 与 多行 → 不同表集合

> 本文是 dbridge 的一份**端到端验证流程文档**，承载 MVP 设计 §1 中"行 ↔ 表集合"映射目标的两个核心场景。它是项目主 [`README.md`](../../README.md) 的补充材料：主 README 讲怎么用 dbridge，本文讲怎么证明它确实把"一行→多表"和"多行→不同表集合"两件事做对了。
>
> 项目代码结构与公共 API 见主 README §3、§14；本文不重复。

---

## 0. 验证全景

本文按"两个并列场景"组织，分别覆盖 MVP 设计 §1 中关于「行 ↔ 表集合」映射的两个硬目标：

| 场景 | 一句话 | 对应 MVP 目标 | 仓库夹具 |
|---|---|---|---|
| **场景 I**：单类行 → 多表集合 | 同一个 Sheet 的**每一行**同时进入父表+子表（m 集合） | §1.4 | ✅ `tests/data/profiles/order_m_set.json` + `tests/data/sql/02_orders.sql` + `tests/data/xlsx/Orders.xlsx` |
| **场景 II**：多类行 → 各自的不同表集合 | 同一个 Sheet 中**多行 A/B/C**，每一类各自落入不同的多表集合（m / n / o） | §1.5 | ✅ `tests/data/profiles/mixed_abc_multitable.json` + `tests/data/sql/04_mixed_multitable.sql` + `tests/data/xlsx/Mixed.xlsx`；另有 `tests/data/profiles/mixed_abc.json` 作为单表精简版（m1/n1/o1）保留，与本场景互不替代 |
| **场景 III**：时间格式导入/导出 | 导入时 Excel 日期字符串按 `excelFormat` 解析并以 `dbFormat` 写入 SQLite；导出时反向还原；`excelFormatFallback` 梯级回退 | 能力 `time-format` | ✅ `tests/data/profiles/time_formats.json` + `tests/data/sql/05_time_formats.sql` + `tests/data/xlsx/Events.xlsx` |
| **场景 IV**：导出列顺序控制 | `columnOrder` 使声明列优先排序，未声明列以自然顺序追加；Mixed 模式 `classColumn` 可嵌入或自动前置 | 能力 `export-column-order` | ✅ `tests/data/profiles/column_order.json` + `tests/data/sql/06_column_order.sql` + `tests/data/xlsx/OrdersColOrder.xlsx` |
| **场景 V**：导出反向 lookup（无损往返） | 导入时 lookup 把 A 列（Excel 表头）替换为 H 列（DB 列）存入库；导出时反向 lookup 恢复 A 列，H 列不出现于 Excel | 能力 `export-reverse-lookup` | ✅ `tests/data/profiles/reverse_lookup.json` + `tests/data/sql/07_reverse_lookup.sql` + `tests/data/xlsx/ReverseLookup.xlsx` |

五个场景互补：
- 场景 I 证明"**一行**可以同时进**多张表**"。
- 场景 II 证明"**多行**之间可以各自路由到**完全不同的表集合**"。
- 场景 III 证明"**日期 / 时间字段**跨 Excel ↔ SQLite 边界的格式转换在导入和导出两个方向正确运作"。
- 场景 IV 证明"**导出列顺序**可通过 `columnOrder` 显式控制，不影响数据完整性"。
- 场景 V 证明"**反向 lookup** 使导入时的字段替换在导出时完全可逆，实现无损往返"。

五个场景同时通过，才视为整套功能的验证闭环。

> 下文出现的 CLI 命令统一以仓库自带的 `examples/cli/dbridge-cli` 为准，调用形式见 `examples/cli/main.cpp`：
> ```
> dbridge-cli <db_path> <profile_json> <xlsx_path> [import|export]
> ```

---

# 场景 I：单类行 → 多表集合（m 集合）

## I-1 验证目标

| 序号 | 验收点 | 来源 |
|---|---|---|
| I-V1 | 一行 Excel 经 Mapper 拆为多个 RoutePayload | 设计文档 §1.4 / 实现文档 §6.6 |
| I-V2 | 父表 `orders` 与子表 `order_items` 按拓扑顺序写入 | 实现文档 §6.3 |
| I-V3 | 子表外键 `order_no` 由父行业务键注入，无需自增 ID 回填 | 设计文档 §15 / 实现文档 §6.6 |
| I-V4 | 整批校验失败时整体回滚，库内无残留 | 设计文档 §6 / §15 |
| I-V5 | 从 `orders` + `order_items` 反向导出回单 Sheet，数据可对账 | 实现文档 §6.2 |
| I-V6 | 负向：父子同批缺业务键、conflict 冲突、拓扑环依赖被正确拦截 | 实现文档 §6.4 / §6.5 |

## I-2 验证拓扑

```
Excel: Orders.xlsx (单 Sheet "Orders")
   │  OrderNo, Customer, Amount, LineNo, Sku, Qty
   ▼
DataBridge.importExcel(...)
   ├── Mapper            # 一行 → [orders payload, order_items payload]
   ├── FkInjector        # 把 orders.order_no 注入到 order_items.order_no
   ├── TopoSorter        # orders 先写，order_items 后写
   └── SqlBuilder        # INSERT ... ON CONFLICT DO UPDATE
   ▼
SQLite: scenarioI.db
   ├── orders(order_no PK, customer, amount)
   └── order_items(order_no, line_no, sku, qty, PRIMARY KEY(order_no, line_no))
   ▼
DataBridge.exportExcel(...)  →  Orders.exported.xlsx
```

## I-3 数据准备

### I-3.1 Schema（复用 `tests/data/sql/02_orders.sql`）

```sql
CREATE TABLE IF NOT EXISTS orders (
    order_no TEXT PRIMARY KEY,
    customer TEXT NOT NULL,
    amount REAL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS order_items (
    order_no TEXT NOT NULL,
    line_no INTEGER NOT NULL,
    sku TEXT NOT NULL,
    qty INTEGER DEFAULT 1,
    PRIMARY KEY (order_no, line_no),
    FOREIGN KEY (order_no) REFERENCES orders(order_no)
);
```

初始化：

```bash
sqlite3 /tmp/scenarioI.db < tests/data/sql/02_orders.sql
sqlite3 /tmp/scenarioI.db "PRAGMA foreign_keys = ON; SELECT sqlite_version();"   # ≥ 3.24.0
```

### I-3.2 Excel 输入（`tests/data/xlsx/Orders.xlsx`）

单 Sheet 名 `Orders`，第 1 行为表头，**共 4 行业务数据**：

| OrderNo | Customer | Amount | LineNo | Sku   | Qty |
|---------|----------|--------|--------|-------|-----|
| SO-001  | Alice    | 120.50 | 1      | SKU-A | 2   |
| SO-001  | Alice    | 120.50 | 2      | SKU-B | 1   |
| SO-002  | Bob      | 80.00  | 1      | SKU-A | 4   |
| SO-003  | Carol    | 15.00  | 1      | SKU-C | 1   |

- 同一张订单 `SO-001` 占据两行；父字段 (`Customer`、`Amount`) 在两行重复，验证主 README §11 / 实现文档 §6.4「父行去重」。
- 每一行都同时贡献到 `orders` 和 `order_items` 两张表 = 验证目标 I-V1。

### I-3.3 Profile（复用 `tests/data/profiles/order_m_set.json`）

仓库中已存在的 profile 内容（节选关键路由）：

```json
{
  "profileName": "order_m_set",
  "sheet": "Orders",
  "mode": "multiTable",
  "routes": [
    { "table": "orders", "conflict": {"columns": ["order_no"]}, "columns": { } },
    {
      "table": "order_items",
      "parent": "orders",
      "fkInject": [{ "from": "orders", "pairs": [["order_no","order_no"]] }],
      "conflict": {"columns": ["order_no", "line_no"]},
      "columns": { "line_no": {}, "sku": {}, "qty": {} }
    }
  ],
  "export": { "orderBy": ["orders.order_no", "order_items.line_no"] }
}
```

Profile 故意**不**显式映射 `OrderNo → order_items.order_no`，强制由 `fkInject` 注入，用以验证 I-V3。

> **fkInject 格式说明**：自本 change 起，`fkInject` 由单列对象改为数组形式。每个元素 `{ "from": "<父表>", "pairs": [["父列","子列"], ...] }` 支持多父表 / 多列注入；旧的 `{ "from": "t.col", "to": "t.col" }` 单列写法已**删除**。

### lookup 用法扩展（场景 I+）

若需在导入时从同库参考表拉取字段合并到当前行，可在 route 里加 `lookups`：

```jsonc
{
  "table": "orders",
  "conflict": { "columns": ["order_no"] },
  "lookups": [
    {
      "name": "cust",
      "from": "ref_customers",
      "match":  [["c_no", "CustNo"]],
      "select": [["c_name", "customer_name"], ["c_tier", "tier"]]
    }
  ],
  "columns": {
    "order_no":  { "source": "OrderNo" }
    // customer_name / tier 由 lookup 填入，无需在 columns 里声明
  }
}
```

- `match`：`[[参考表列, Excel 表头], ...]`，用 Excel 当前行的 `CustNo` 值到 `ref_customers.c_no` 查找。
- `select`：`[[参考表列, 目标 DB 列], ...]`，命中后把 `c_name` / `c_tier` 合并写入本 route 的 payload。
- 所有 key 均在 Phase A.5（导入前）批量预取（`SELECT ... WHERE c_no IN (...)`），Phase B 按行命中，无逐行 SQL。
- 命中 0 行 → `E_LOOKUP_NOT_FOUND`；命中多行 → `E_LOOKUP_AMBIGUOUS`；key 为空 → `E_LOOKUP_KEY_EMPTY`；key 类型不兼容参考表列 → `E_LOOKUP_KEY_INVALID`（均为 row-level 错误，不中断整批）。

## I-4 执行步骤

### I-4.1 导入

```bash
./build/examples/cli/dbridge-cli \
  /tmp/scenarioI.db \
  tests/data/profiles/order_m_set.json \
  tests/data/xlsx/Orders.xlsx \
  import
```

期望 stdout 形如 `Imported <N> rows`，无错误。

### I-4.2 SQL 断言

```sql
-- I-V1+I-V3：一行 Excel 拆出了父子两条记录，且子表 order_no 与父业务键一致
SELECT o.order_no, o.customer, o.amount, i.line_no, i.sku, i.qty
FROM   orders o
JOIN   order_items i ON i.order_no = o.order_no
ORDER  BY o.order_no, i.line_no;
-- 期望返回 4 行，与 Excel 4 行一一对应

PRAGMA foreign_key_check;                                  -- I-V2：无输出
SELECT COUNT(*) FROM order_items WHERE order_no IS NULL;   -- I-V3：期望 0
```

### I-4.3 导出（I-V5）

```bash
./build/examples/cli/dbridge-cli \
  /tmp/scenarioI.db \
  tests/data/profiles/order_m_set.json \
  /tmp/Orders.exported.xlsx \
  export
```

对账（工具详情见 [§工具：`tools/xlsx2csv.py`](#工具toolsxlsx2csvpy)）：

```bash
python3 tools/xlsx2csv.py tests/data/xlsx/Orders.xlsx  > /tmp/in_I.csv
python3 tools/xlsx2csv.py /tmp/Orders.exported.xlsx    > /tmp/out_I.csv
sort /tmp/in_I.csv  | sha256sum
sort /tmp/out_I.csv | sha256sum   # 应与 in_I 完全一致
```

### I-4.4 负向场景（I-V4 / I-V6）

| 场景 | 制造方式 | 期望错误码 | 期望库状态 |
|---|---|---|---|
| 父业务键缺失 | 第 2 行 `OrderNo` 清空 | `E_VALIDATE_NULL` | **行级错误**：第 2 行跳过，第 1、3 行正常写入（两表非空） |
| 同批 conflict 冲突且非 key 字段不一致 | 第 1、2 行 `OrderNo` 相同但 `Amount` 不同 | `E_VALIDATE_DUPLICATE` | **行级错误**：重复行跳过，另一行写入（两表有部分数据） |
| Profile 拓扑成环 | 给 `orders` 加 `parent: "order_items"` | `E_PROFILE_TOPOLOGY_CYCLE` | **表级错误（row=0）**：整批中止，两张表均为 0 行 |
| FK 引用不存在 | 子行 `OrderNo` 改成数据库中不存在的订单号且父行已被删 | `E_VALIDATE_FK` | **行级错误**：对应行跳过，其余行正常写入 |

> **行级 vs 表级区分**（代码依据 `ImportService.cpp:652-666`）：`row > 0` 错误（如 `E_VALIDATE_*`）仅跳过失败行，其余行继续导入并提交；`row == 0` 错误（如 `E_PROFILE_TOPOLOGY_CYCLE`、`E_PROFILE_PARSE`）在 Phase D 写入前中止整批，DB 不写任何行。

---

# 场景 II：多类行 → 各自的不同表集合（m / n / o）

> **夹具状态**：本场景全部夹具（schema / Profile / `Mixed.xlsx`）已签入仓库（路径见 §II-3.1 / §II-3.2 / §II-3.3）；可用 `python3 tools/build_fixtures.py` 重新生成 xlsx。本场景已在 master HEAD 通过端到端验证：6 张表行数、FK 完整性、A/B/C 集合互不污染、日期字段 ISO 形态均符合预期；mixed 模式下的 FK 预校验路径由 `tst_fk_preflight` 单元测试守护，避免历史误报 `E_VALIDATE_FK` 的回归。
>
> 仓库中另有 `tests/data/profiles/mixed_abc.json` + `tests/data/sql/03_mixed.sql`，把每个 class 路由到**单张**目标表（m1 / n1 / o1），用以最小化覆盖 §1.5 鉴别逻辑——与本场景并存，互不替代。

## II-1 验证目标

| 序号 | 验收点 | 来源 |
|---|---|---|
| II-V1 | 同一个 Sheet 中可以混杂 A/B/C 三类行 | 设计文档 §1.5 |
| II-V2 | A 行只落入 m 集合（`orders` + `order_items`），不污染 n/o 集合 | 实现文档 §6.7 Discriminator |
| II-V3 | B 行只落入 n 集合（`shipments` + `shipment_legs`） | 同上 |
| II-V4 | C 行只落入 o 集合（`invoices` + `invoice_lines`） | 同上 |
| II-V5 | 每个 class 内部父子表均通过 `fkInject` 业务键注入并按拓扑写入 | 实现文档 §6.3 / §6.6 |
| II-V6 | 任一类校验失败 → 整批回滚，三个集合都为 0 | 设计文档 §6 |
| II-V7 | 反向导出能把 m/n/o 数据重组回同一个 Sheet 且带回 `Type` 鉴别列 | 实现文档 §6.8 expandRows |
| II-V8 | 负向：无法匹配的行触发 `E_ROUTE_UNMATCHED`，整批回滚 | MVP 设计 §15 |

## II-2 验证拓扑

```
Excel: Mixed.xlsx (单 Sheet "Mixed")
   │  Type, OrderNo,Customer,Amount,LineNo,Sku,Qty,
   │  ShipmentNo,Carrier,ETA,LegNo,Origin,Dest,
   │  InvoiceNo,BillTo,Total,InvLineNo,Item,Price
   ▼
DataBridge.importExcel(profile=mixed_abc_multitable.json)
   ├── Router        # 按 Type 列 → class A / B / C
   ├── Mapper        # 每个 class 走自己的 routes[]
   ├── FkInjector    # m 集合 / n 集合 / o 集合 各自父→子注入
   ├── TopoSorter    # 每个 class 内独立拓扑排序
   └── SqlBuilder
   ▼
SQLite: scenarioII.db
   ├── m 集合: orders + order_items          ← 仅来自 A 行
   ├── n 集合: shipments + shipment_legs      ← 仅来自 B 行
   └── o 集合: invoices + invoice_lines       ← 仅来自 C 行
```

## II-3 数据准备

### II-3.1 Schema（路径 `tests/data/sql/04_mixed_multitable.sql`）

> 以下为可读形态；签入版本使用 `CREATE TABLE IF NOT EXISTS`，便于重复初始化。`PRAGMA foreign_keys = ON;` 由宿主在 `DataBridge::open` 后自行开启，签入文件不重复写。

```sql
PRAGMA foreign_keys = ON;

-- m 集合（A 行的目标）
CREATE TABLE orders (
    order_no  TEXT PRIMARY KEY,
    customer  TEXT NOT NULL,
    amount    REAL NOT NULL
);
CREATE TABLE order_items (
    order_no  TEXT NOT NULL,
    line_no   INTEGER NOT NULL,
    sku       TEXT NOT NULL,
    qty       INTEGER NOT NULL,
    PRIMARY KEY (order_no, line_no),
    FOREIGN KEY (order_no) REFERENCES orders(order_no)
);

-- n 集合（B 行的目标）
CREATE TABLE shipments (
    shipment_no  TEXT PRIMARY KEY,
    carrier      TEXT NOT NULL,
    eta          TEXT NOT NULL          -- ISO date
);
CREATE TABLE shipment_legs (
    shipment_no  TEXT NOT NULL,
    leg_no       INTEGER NOT NULL,
    origin       TEXT NOT NULL,
    dest         TEXT NOT NULL,
    PRIMARY KEY (shipment_no, leg_no),
    FOREIGN KEY (shipment_no) REFERENCES shipments(shipment_no)
);

-- o 集合（C 行的目标）
CREATE TABLE invoices (
    invoice_no  TEXT PRIMARY KEY,
    bill_to     TEXT NOT NULL,
    total       REAL NOT NULL
);
CREATE TABLE invoice_lines (
    invoice_no  TEXT NOT NULL,
    line_no     INTEGER NOT NULL,
    item        TEXT NOT NULL,
    price       REAL NOT NULL,
    PRIMARY KEY (invoice_no, line_no),
    FOREIGN KEY (invoice_no) REFERENCES invoices(invoice_no)
);
```

### II-3.2 Excel 输入（`tests/data/xlsx/Mixed.xlsx`）

单 Sheet 名 `Mixed`，第 1 行表头是 **A/B/C 三类列的并集 + `Type` 鉴别列**。每一行只填写自己 class 需要的列，其他单元格留空。**共 6 行业务数据**：

| Type | OrderNo | Customer | Amount | LineNo | Sku   | Qty | ShipmentNo | Carrier | ETA        | LegNo | Origin    | Dest      | InvoiceNo | BillTo | Total  | InvLineNo | Item    | Price  |
|------|---------|----------|--------|--------|-------|-----|------------|---------|------------|-------|-----------|-----------|-----------|--------|--------|-----------|---------|--------|
| A    | SO-001  | Alice    | 120.50 | 1      | SKU-A | 2   |            |         |            |       |           |           |           |        |        |           |         |        |
| A    | SO-001  | Alice    | 120.50 | 2      | SKU-B | 1   |            |         |            |       |           |           |           |        |        |           |         |        |
| B    |         |          |        |        |       |     | SH-100     | DHL     | 2026-05-20 | 1     | Shanghai  | Singapore |           |        |        |           |         |        |
| B    |         |          |        |        |       |     | SH-100     | DHL     | 2026-05-20 | 2     | Singapore | Sydney    |           |        |        |           |         |        |
| C    |         |          |        |        |       |     |            |         |            |       |           |           | INV-9001  | Alice  | 240.00 | 1         | Widget  | 120.00 |
| C    |         |          |        |        |       |     |            |         |            |       |           |           | INV-9001  | Alice  | 240.00 | 2         | Gadget  | 120.00 |

数据分布：

| Class | 行数 | 目标表集合 | 父去重后写入 |
|---|---|---|---|
| A | 2 | m 集合：`orders` + `order_items` | 1 + 2 |
| B | 2 | n 集合：`shipments` + `shipment_legs` | 1 + 2 |
| C | 2 | o 集合：`invoices` + `invoice_lines` | 1 + 2 |
| **合计** | **6** | 6 张表 | 3 + 6 = 9 条 payload |

关键性质：

- 6 行 Excel **分别**对应三组完全不同的表集合 = 验证目标 II-V1 ~ II-V4。
- 每一类内部都至少 2 张表 + 父子 FK 注入 = 验证 II-V5；这是"集合"二字的物质化体现。
- A 行的 `SO-001` 在 2 行中重复 → 复用 §6.4 父行去重逻辑。

### II-3.3 Profile（路径 `tests/data/profiles/mixed_abc_multitable.json`）

```json
{
  "profileName": "mixed_abc_multitable",
  "sheet": "Mixed",
  "headerRow": 1,
  "mode": "mixed",
  "discriminator": { "source": "Type" },
  "classes": [
    {
      "id": "A",
      "match": { "equals": "A" },
      "routes": [
        {
          "table": "orders",
          "conflict": { "columns": ["order_no"] },
          "columns": {
            "order_no": { "source": "OrderNo", "validators": ["notNull"] },
            "customer": { "source": "Customer", "validators": ["notNull"] },
            "amount":   { "source": "Amount",   "validators": ["decimal"] }
          }
        },
        {
          "table": "order_items",
          "parent": "orders",
          "fkInject": [{ "from": "orders", "pairs": [["order_no","order_no"]] }],
          "conflict": { "columns": ["order_no", "line_no"] },
          "columns": {
            "line_no": { "source": "LineNo", "validators": ["int>=1"] },
            "sku":     { "source": "Sku",     "validators": ["notNull"] },
            "qty":     { "source": "Qty",     "validators": ["int>=1"] }
          }
        }
      ]
    },
    {
      "id": "B",
      "match": { "equals": "B" },
      "routes": [
        {
          "table": "shipments",
          "conflict": { "columns": ["shipment_no"] },
          "columns": {
            "shipment_no": { "source": "ShipmentNo", "validators": ["notNull"] },
            "carrier":     { "source": "Carrier",     "validators": ["notNull"] },
            "eta":         { "source": "ETA",         "validators": ["date:yyyy-MM-dd"] }
          }
        },
        {
          "table": "shipment_legs",
          "parent": "shipments",
          "fkInject": [{ "from": "shipments", "pairs": [["shipment_no","shipment_no"]] }],
          "conflict": { "columns": ["shipment_no", "leg_no"] },
          "columns": {
            "leg_no": { "source": "LegNo",  "validators": ["int>=1"] },
            "origin": { "source": "Origin", "validators": ["notNull"] },
            "dest":   { "source": "Dest",   "validators": ["notNull"] }
          }
        }
      ]
    },
    {
      "id": "C",
      "match": { "equals": "C" },
      "routes": [
        {
          "table": "invoices",
          "conflict": { "columns": ["invoice_no"] },
          "columns": {
            "invoice_no": { "source": "InvoiceNo", "validators": ["notNull"] },
            "bill_to":    { "source": "BillTo",    "validators": ["notNull"] },
            "total":      { "source": "Total",     "validators": ["decimal"] }
          }
        },
        {
          "table": "invoice_lines",
          "parent": "invoices",
          "fkInject": [{ "from": "invoices", "pairs": [["invoice_no","invoice_no"]] }],
          "conflict": { "columns": ["invoice_no", "line_no"] },
          "columns": {
            "line_no": { "source": "InvLineNo", "validators": ["int>=1"] },
            "item":    { "source": "Item",      "validators": ["notNull"] },
            "price":   { "source": "Price",     "validators": ["decimal"] }
          }
        }
      ]
    }
  ],
  "export": {
    "classColumn": "Type"
  }
}
```

- `mode: "mixed"` + `discriminator.source: "Type"` 让 Router 按 `Type` 列分发。
- 每个 `class` 都有自己的 `routes[]`，三组互不重叠 → A/B/C 各自落入 m/n/o。
- 混编模式禁止顶层 `export.sql`（实现文档 §4.1），由引擎为每个 class 各自生成 LEFT JOIN 后按 `Type` 拼回单 Sheet。

## II-4 执行步骤

### II-4.1 导入

```bash
sqlite3 /tmp/scenarioII.db < tests/data/sql/04_mixed_multitable.sql

./build/examples/cli/dbridge-cli \
  /tmp/scenarioII.db \
  tests/data/profiles/mixed_abc_multitable.json \
  tests/data/xlsx/Mixed.xlsx \
  import
```

期望：`Imported <N> rows`，无错误。

### II-4.2 SQL 断言（II-V2 ~ II-V5）

```sql
-- A 行只落入 m 集合
SELECT COUNT(*) FROM orders;          -- 期望 1（SO-001 父去重）
SELECT COUNT(*) FROM order_items;     -- 期望 2

-- B 行只落入 n 集合
SELECT COUNT(*) FROM shipments;       -- 期望 1（SH-100 父去重）
SELECT COUNT(*) FROM shipment_legs;   -- 期望 2

-- C 行只落入 o 集合
SELECT COUNT(*) FROM invoices;        -- 期望 1（INV-9001 父去重）
SELECT COUNT(*) FROM invoice_lines;   -- 期望 2

-- 集合互不污染：A 行不会出现在 n/o 表
SELECT COUNT(*) FROM shipments     WHERE shipment_no IN ('SO-001','SO-002','SO-003');  -- 0
SELECT COUNT(*) FROM invoices      WHERE invoice_no  IN ('SO-001','SO-002','SO-003');  -- 0

-- FK 完整性：每个集合父子都对齐
PRAGMA foreign_key_check;             -- 应无输出
SELECT COUNT(*) FROM order_items     WHERE order_no    NOT IN (SELECT order_no FROM orders);         -- 0
SELECT COUNT(*) FROM shipment_legs   WHERE shipment_no NOT IN (SELECT shipment_no FROM shipments);   -- 0
SELECT COUNT(*) FROM invoice_lines   WHERE invoice_no  NOT IN (SELECT invoice_no FROM invoices);     -- 0
```

### II-4.3 导出（II-V7）

```bash
./build/examples/cli/dbridge-cli \
  /tmp/scenarioII.db \
  tests/data/profiles/mixed_abc_multitable.json \
  /tmp/Mixed.exported.xlsx \
  export
```

期望导出的 Sheet：

- 保留 `Type` 列，每行能反向定位到 A/B/C。
- A 行 / B 行 / C 行各自占据相应的列分组，其他列为空 —— 与输入对称。
- 行总数 = `order_items` 2 + `shipment_legs` 2 + `invoice_lines` 2 = 6，恰好等于输入 6 行。

对账（工具详情见 [§工具：`tools/xlsx2csv.py`](#工具toolsxlsx2csvpy)）：

```bash
python3 tools/xlsx2csv.py tests/data/xlsx/Mixed.xlsx  > /tmp/in_II.csv
python3 tools/xlsx2csv.py /tmp/Mixed.exported.xlsx    > /tmp/out_II.csv
sort /tmp/in_II.csv  | sha256sum
sort /tmp/out_II.csv | sha256sum   # 应一致
```

### II-4.4 负向场景（II-V6 / II-V8）

| 场景 | 制造方式 | 期望错误码 | 期望库状态 |
|---|---|---|---|
| 未匹配类 | 把第 3 行 `Type` 改为 `X` | `E_ROUTE_UNMATCHED`（指向该行 `Type=X`） | **行级错误**：该行跳过，其余行正常写入（各表非空） |
| 跨集合污染尝试 | 把 C 行 `Type` 误填为 `A`，但 `OrderNo` 列为空 | `E_VALIDATE_NULL`（A class 的 `OrderNo`） | **行级错误**：该行跳过，同批其余行正常写入 |
| n 集合 FK 缺失 | B 行 `ShipmentNo` 清空 | `E_VALIDATE_NULL` | **行级错误**：该行跳过，同批其余行正常写入 |
| 子行业务键越界 | C 行 `InvLineNo` 改成 0 | `E_VALIDATE_TYPE`（`int>=1`） | **行级错误**：该行跳过，同批其余行正常写入 |
| FK 父行批/库均缺失 | 子行 FK 值在批内与 DB 中都查不到 | `E_VALIDATE_FK` —— 单元回归用例 `tst_fk_preflight::testParentMissing` 覆盖 | **行级错误**：该行跳过，同批其余行正常写入 |

> **行级 vs 表级区分**：以上全部场景均为 `row > 0` 行级错误，只跳过失败行，不中止整批。验证方式：执行负向用例后确认**报错行不在 DB 中**，同时**同批其他行均已写入**（各对应表行数 > 0）。
>
> 若需验证"整批中止"（6 张表全部 0）的路径，需触发 `row == 0` 的表级错误（如 `E_PROFILE_TOPOLOGY_CYCLE`）。

---

# 场景 III：时间格式导入/导出

## III-1 验证目标

| 序号 | 验收点 | 来源 |
|---|---|---|
| III-V1 | Profile 级 `dateFormat`/`datetimeFormat`/`timeFormat` 独立加载，三槽互不串联 | 能力 `time-format` §三独立槽 |
| III-V2 | 导入时 Excel 字符串按 `excelFormat` 解析，再按 `dbFormat` 序列化写入 SQLite | 能力 §导入解析路径 |
| III-V3 | `excelFormatFallback` 按数组顺序依次尝试，首个成功的生效 | 能力 §fallback 梯级回退 |
| III-V4 | 列级 `dateFormat` 字段级覆盖：只声明 `excelFormat` 时 `dbFormat` 继承 Profile 级 | 能力 §列级覆盖字段合并 |
| III-V5 | 导出时 DB 字符串按 `dbFormat` 解析，再按 `excelFormat` 序列化写回 Excel | 能力 §导出序列化路径 |
| III-V6 | 导入失败 `E_TIME_PARSE` → 仅该行跳过，其余行继续；导出失败 `E_TIME_PARSE_DB` → 仅该格置空，该行其余格继续 | 能力 §失败语义行级隔离 |
| III-V7 | 使用非 `yyyy` 开头 `dbFormat` 的列作为 `orderBy` 对象 → Profile 加载成功并触发 `W_TIME_ORDERBY_NONSORTABLE` | 能力 §不可排序警告 |

## III-2 验证拓扑

```
Excel: Events.xlsx (单 Sheet "Events")
   │  EventID, Title, EventDate, EventDateTime, StartTime, LegacyDate, DateWithFallback
   ▼
DataBridge.importExcel(profile=time_formats.json)
   ├── event_date       ← excelFormat:"yyyy/M/d"        →解析→ dbFormat:"yyyy-MM-dd"
   ├── event_datetime   ← excelFormat:"d/M/yyyy H:mm"   →解析→ dbFormat:"yyyy-MM-dd HH:mm:ss"  [列级覆盖]
   ├── start_time       ← excelFormat:"HH:mm"           →解析→ dbFormat:"HH:mm:ss"
   ├── legacy_date      ← validators:["date:yyyy-MM-dd"]          [旧式兼容]
   └── date_with_fallback ← excelFormat:"yyyy-MM-dd" + fallback:["d/M/yyyy","MM/dd/yyyy"]
   ▼
SQLite: scenarioIII.db  →  event 表（时间列均以 dbFormat 字符串存储）
   ▼
DataBridge.exportExcel(...)  →  Events.exported.xlsx（时间列还原回 excelFormat）
```

## III-3 数据准备

### III-3.1 Schema（路径 `tests/data/sql/05_time_formats.sql`）

```sql
CREATE TABLE IF NOT EXISTS event (
    event_id           INTEGER PRIMARY KEY,
    title              TEXT    NOT NULL,
    event_date         TEXT,            -- 存储格式 yyyy-MM-dd
    event_datetime     TEXT,            -- 存储格式 yyyy-MM-dd HH:mm:ss
    start_time         TEXT,            -- 存储格式 HH:mm:ss
    legacy_date        TEXT,            -- 存储格式 yyyy-MM-dd（旧式 validator）
    date_with_fallback TEXT             -- 存储格式 yyyy-MM-dd
);
```

初始化：

```bash
sqlite3 /tmp/scenarioIII.db < tests/data/sql/05_time_formats.sql
```

### III-3.2 Excel 输入（`tests/data/xlsx/Events.xlsx`）

单 Sheet 名 `Events`，**共 3 行业务数据**：

| EventID | Title | EventDate | EventDateTime | StartTime | LegacyDate | DateWithFallback |
|---------|-------|-----------|---------------|-----------|------------|-----------------|
| 1 | Workshop | 2026/5/21 | 21/5/2026 9:30 | 09:30 | 2026-05-21 | 2026-05-21 |
| 2 | Conference | 2026/6/1 | 1/6/2026 14:00 | 14:00 | 2026-06-01 | 01/06/2026 |
| 3 | Seminar | 2026/7/15 | 15/7/2026 18:30 | 18:30 | 2026-07-15 | 07/15/2026 |

- `EventDate` 用 Profile 级 `dateFormat.excelFormat = "yyyy/M/d"` 解析（III-V2）。
- `EventDateTime` 用**列级覆盖** `excelFormat = "d/M/yyyy H:mm"` 解析；`dbFormat` 继承 Profile 级 `"yyyy-MM-dd HH:mm:ss"`（III-V4）。
- `DateWithFallback` 第 1 行 `"2026-05-21"` 命中主格式；第 2 行 `"01/06/2026"` 回退 `"d/M/yyyy"`（d=01, M=06）；第 3 行 `"07/15/2026"` 回退 `"MM/dd/yyyy"`（III-V3）。
- `LegacyDate` 使用旧式 `validators:["date:yyyy-MM-dd"]`，不走新 `dateFormat` 路径，行为不变。

### III-3.3 Profile（复用 `tests/data/profiles/time_formats.json`）

文件已签入，关键字段节选：

```json
{
  "profileName": "time_formats",
  "sheet": "Events",
  "mode": "singleTable",
  "table": "event",
  "conflict": { "columns": ["event_id"] },
  "dateFormat":     { "excelFormat": "yyyy/M/d",          "dbFormat": "yyyy-MM-dd" },
  "datetimeFormat": { "excelFormat": "yyyy/M/d HH:mm:ss", "dbFormat": "yyyy-MM-dd HH:mm:ss" },
  "timeFormat":     { "excelFormat": "HH:mm",             "dbFormat": "HH:mm:ss" },
  "columns": {
    "event_datetime": {
      "source": "EventDateTime",
      "datetimeFormat": { "excelFormat": "d/M/yyyy H:mm" }
    },
    "start_time": {
      "source": "StartTime",
      "timeFormat": {}
    },
    "date_with_fallback": {
      "source": "DateWithFallback",
      "dateFormat": {
        "excelFormat": "yyyy-MM-dd",
        "dbFormat":    "yyyy-MM-dd",
        "excelFormatFallback": ["d/M/yyyy", "MM/dd/yyyy"]
      }
    }
  },
  "export": { "orderBy": ["event_date"] }
}
```

## III-4 执行步骤

### III-4.1 导入

```bash
./build/examples/cli/dbridge-cli \
  /tmp/scenarioIII.db \
  tests/data/profiles/time_formats.json \
  tests/data/xlsx/Events.xlsx \
  import
```

期望：无错误，`Imported 3 rows`。

### III-4.2 SQL 断言（III-V2 / III-V3 / III-V4）

```sql
-- III-V2：event_date 均以 yyyy-MM-dd 存储
SELECT event_id, event_date FROM event ORDER BY event_id;
-- 期望：1→'2026-05-21'，2→'2026-06-01'，3→'2026-07-15'

-- III-V4 列级覆盖：event_datetime 按列级 excelFormat 解析，按 profile 级 dbFormat 存储
SELECT event_datetime FROM event ORDER BY event_id;
-- 期望：'2026-05-21 09:30:00'，'2026-06-01 14:00:00'，'2026-07-15 18:30:00'

-- III-V2 timeFormat
SELECT start_time FROM event ORDER BY event_id;
-- 期望：'09:30:00'，'14:00:00'，'18:30:00'

-- III-V3 fallback：三行 date_with_fallback 均正确写入
SELECT date_with_fallback FROM event ORDER BY event_id;
-- 期望：'2026-05-21'，'2026-06-01'，'2026-07-15'
```

### III-4.3 导出（III-V5）

```bash
./build/examples/cli/dbridge-cli \
  /tmp/scenarioIII.db \
  tests/data/profiles/time_formats.json \
  /tmp/Events.exported.xlsx \
  export
```

目视核查导出文件（用 `tools/xlsx2csv.py` 辅助，或直接开 Excel/LibreOffice）：

| 列 | 期望导出格式 | 示例值 |
|---|---|---|
| `EventDate` | `excelFormat:"yyyy/M/d"` | `2026/5/21`，`2026/6/1`，`2026/7/15` |
| `EventDateTime` | 列级 `excelFormat:"d/M/yyyy H:mm"` | `21/5/2026 9:30`，`1/6/2026 14:00`，`15/7/2026 18:30` |
| `StartTime` | `excelFormat:"HH:mm"` | `09:30`，`14:00`，`18:30` |
| `DateWithFallback` | `excelFormat:"yyyy-MM-dd"` | `2026-05-21`，`2026-06-01`，`2026-07-15` |

### III-4.4 负向场景（III-V6 / III-V7）

| 场景 | 制造方式 | 期望结果 |
|---|---|---|
| 导入 `E_TIME_PARSE`（主格式不匹配） | 第 2 行 `EventDate` 改为 `"abc"` | 第 2 行跳过，emit `E_TIME_PARSE`；第 1、3 行正常写入，库中存 2 行 |
| 导入 `E_TIME_PARSE`（fallback 全失败） | 第 3 行 `DateWithFallback` 改为 `"9999-99-99"` | 第 3 行跳过，emit `E_TIME_PARSE`；第 1、2 行正常写入 |
| 导出 `E_TIME_PARSE_DB` | 直接 `UPDATE event SET event_date='20260521' WHERE event_id=1` 后导出 | row-1 的 `EventDate` 格写空，emit `E_TIME_PARSE_DB`；同行其余格正常写出 |
| `W_TIME_ORDERBY_NONSORTABLE` | 新建 Profile，令 `datetimeFormat.dbFormat = "d/M/yyyy H:mm"`，`orderBy: ["event_datetime"]` | Profile 加载成功，`result.warnings` 包含 `W_TIME_ORDERBY_NONSORTABLE`，消息含 `"event_datetime"` 和 `"d/M/yyyy H:mm"` |

---

## III-E epoch 子场景：DB 存 Unix epoch seconds 的端到端验证

本子场景覆盖 `type: "epochSec"` 路径：SQLite 列类型为 `INTEGER`，以 Unix 纪元秒存储。

### III-E.1 夹具

| 文件 | 路径 |
|---|---|
| Schema | `tests/data/sql/08_epoch_time.sql` |
| Profile | `tests/data/profiles/epoch_time.json` |
| Excel 输入 | `tests/data/xlsx/EpochEvents.xlsx` |

Profile 关键配置：

```json
{
  "datetimeFormat": {
    "excel": { "type": "string", "format": "yyyy-MM-dd HH:mm:ss" },
    "db":    { "type": "epochSec" }
  }
}
```

### III-E.2 导入断言

```bash
dbridge-cli /tmp/epochE.db tests/data/profiles/epoch_time.json tests/data/xlsx/EpochEvents.xlsx import
```

```sql
-- 验证 happen_at 存储为整数（非字符串）
SELECT typeof(happen_at) FROM epoch_event;
-- 期望每行均为 integer

-- 验证 epoch=0 对应 "1970-01-01 00:00:00"（行 3）
SELECT happen_at FROM epoch_event WHERE event_id=3;
-- 期望：epoch_seconds_of_local_"1970-01-01 00:00:00"
-- （本地时区 UTC+8 时为 -28800，UTC+0 时为 0）

-- 验证往返：epoch 解析回字符串与输入一致
SELECT datetime(happen_at, 'unixepoch', 'localtime') FROM epoch_event WHERE event_id=1;
-- 期望：2024-05-21 10:00:00（仅在 UTC 环境正确；本地时区用宿主侧工具验证）
```

### III-E.3 导出断言（III-E 往返）

```bash
dbridge-cli /tmp/epochE.db tests/data/profiles/epoch_time.json /tmp/EpochEvents.exported.xlsx export
```

导出后与原始 `EpochEvents.xlsx` 比对：`HappenAt` 列的字符串值应与输入完全一致（在相同时区环境下）。

### III-E.4 epochSec 特殊边界

| 场景 | 制造方式 | 期望结果 |
|---|---|---|
| NULL → 空单元格 | `UPDATE epoch_event SET happen_at=NULL WHERE event_id=1`（需先 `ALTER` 去掉 NOT NULL） | 导出时该格为空，不报错 |
| 0 → 不视为空 | `UPDATE epoch_event SET happen_at=0 WHERE event_id=1` | 导出时该格为 `"1970-01-01 00:00:00"`（本地时区），不为空 |
| 非整数字符串 | 直接 `UPDATE epoch_event SET happen_at='abc'` 后导出 | 该格置空，emit `E_TIME_PARSE_DB`，行继续 |
| epochSec 用于 `dateFormat` | 在 `dateFormat.db` 声明 `type: "epochSec"` | Profile 加载时报 `E_PROFILE_PARSE`，阻止加载 |
| epochSec 用于 `excel` side | 在 `datetimeFormat.excel` 声明 `type: "epochSec"` | Profile 加载时报 `E_PROFILE_PARSE`，阻止加载 |
| `orderBy` 含 epochSec 列 | `"export": {"orderBy": ["happen_at"]}` | Profile 加载成功，无 `W_TIME_ORDERBY_NONSORTABLE`（整数自然有序） |

---

## III-F 新 side-object 形态与 side 级整体覆盖验证

本子场景覆盖 `add-time-explicit-type` 引入的核心新语义：显式 `type` 字段、`excel`/`db` sub-object 形态，以及与旧形态的混用规则。夹具复用 §III-3 的 `Events.xlsx` 和 `05_time_formats.sql`；Profile 变体均以 JSON 片段内联，无需额外签入文件。

### III-F.1 新形态等价性（type=string 路径）

将 §III-3.3 Profile 的时间 slot 改写为新形态后，导入结果应与 §III-4.2 完全一致。

**等价关系**：

```jsonc
// 旧形态（§III-3.3 已签入）
"dateFormat": { "excelFormat": "yyyy/M/d", "dbFormat": "yyyy-MM-dd" }

// 等价新形态（type 可省略，省略时默认 "string"）
"dateFormat": {
  "excel": { "type": "string", "format": "yyyy/M/d" },
  "db":    { "type": "string", "format": "yyyy-MM-dd" }
}
```

将 `tests/data/profiles/time_formats.json` 中所有 slot 改写为新形态后另存为 `/tmp/time_formats_newstyle.json`，用同一 `Events.xlsx` 导入新库：

```bash
sqlite3 /tmp/scenarioIII_new.db < tests/data/sql/05_time_formats.sql

./build/examples/cli/dbridge-cli \
  /tmp/scenarioIII_new.db \
  /tmp/time_formats_newstyle.json \
  tests/data/xlsx/Events.xlsx \
  import
```

执行 §III-4.2 中相同的 SQL 断言——结果应与旧形态完全一致：

```sql
SELECT event_date FROM event ORDER BY event_id;
-- 期望：'2026-05-21'，'2026-06-01'，'2026-07-15'

SELECT event_datetime FROM event ORDER BY event_id;
-- 期望：'2026-05-21 09:30:00'，'2026-06-01 14:00:00'，'2026-07-15 18:30:00'
```

**预期**：Profile 加载无警告，数据行为与旧形态无差异。

### III-F.2 side 级整体覆盖：列仅覆盖 db side

**语义差异**（旧形态字段级合并 vs 新形态 side 级整体覆盖）：

| 列级声明方式 | 覆盖范围 |
|---|---|
| 旧：`{ "datetimeFormat": {"excelFormat":"d/M/yyyy H:mm"} }` | 仅覆盖 excelFormat 字段；dbFormat 继承 profile 级 **该字段** |
| 新：`{ "datetimeFormat": {"excel": {...}} }` | excel side **整体**替换；db side **整体**继承 profile |
| 新：`{ "datetimeFormat": {"db": {...}} }` | db side **整体**替换（含 type）；excel side **整体**继承 profile |

验证「列仅声明 db side，excel side 从 profile 整体继承」：

```jsonc
// Profile 级（新形态）
"datetimeFormat": {
  "excel": { "type": "string", "format": "d/M/yyyy H:mm" },
  "db":    { "type": "string", "format": "yyyy-MM-dd HH:mm:ss" }
},
"columns": {
  "event_datetime": {
    "source": "EventDateTime",
    "datetimeFormat": {
      // 仅覆盖 db side：db 改用带秒的格式
      "db": { "type": "string", "format": "yyyy-MM-dd HH:mm:00" }
      // excel side 未声明 → 整体继承 profile 级 excel（"d/M/yyyy H:mm"）
    }
  }
}
```

导入 `Events.xlsx`（第 2 行 `EventDateTime = "1/6/2026 14:00"`）后断言：

```sql
SELECT event_datetime FROM event WHERE event_id=2;
-- 期望：'2026-06-01 14:00:00'
-- （excel side 沿用 profile 的 "d/M/yyyy H:mm" 完成解析；db side 使用列级格式序列化）
```

**预期**：Profile 加载成功；excel 解析路径使用 profile 级 `"d/M/yyyy H:mm"`；db 序列化路径使用列级声明的 `"yyyy-MM-dd HH:mm:00"`。

### III-F.3 旧新跨层混用：profile 级旧形态 + 列级新形态

Profile 级保留旧形态，列级使用新形态覆盖 db side：

```jsonc
// Profile 级旧形态（excelFormat/dbFormat 平铺）
"dateFormat": { "excelFormat": "yyyy/M/d", "dbFormat": "yyyy-MM-dd" },

"columns": {
  "event_date": {
    "source": "EventDate",
    "dateFormat": {
      // 列级新形态（仅覆盖 db side，整体替换）
      "db": { "type": "string", "format": "yyyy.MM.dd" }
      // excel side 未声明 → 整体继承 profile 旧形态正规化后的 excel side
    }
  }
}
```

导入后断言：

```sql
SELECT event_date FROM event WHERE event_id=1;
-- 期望：'2026.05.21'（db side 被列级新形态整体替换为点分格式）

SELECT event_date FROM event WHERE event_id=2;
-- 期望：'2026.06.01'（列级覆盖对每一行均生效）
```

**预期**：Profile 加载成功（跨层混用合法）；列级 `db` 整体覆盖（包含 type）；`excel` 继承 profile 旧形态（正规化为 `{type:"string", format:"yyyy/M/d"}`）。

**逆向混用**（Profile 级新形态 + 列级旧形态）：同样合法，列级旧形态正规化为新形态后执行 side 整体覆盖。

### III-F.4 负向：Profile 加载时 E_PROFILE_PARSE

以下场景均在 Profile 加载阶段报错，不进入导入/导出流程；可通过 `dbridge-cli` 触发后检查 stderr，或在单元测试中直接断言：

| 触发场景 | 触发构造 | 期望错误信息要点 |
|---|---|---|
| `type="string"` 无 format | `"dateFormat": {"db": {"type":"string"}}` | 含 slot 名、side 名、"format required" 语义 |
| `type="epochSec"` 带非空 format | `"datetimeFormat": {"db": {"type":"epochSec","format":"yyyy-MM-dd HH:mm:ss"}}` | 含 "epochSec must have no format" |
| 未知 type 值 | `"dateFormat": {"db": {"type":"unix_ms"}}` | 含 `"unix_ms"` 和 `"epochSec"` 可用值列表 |
| 同 slot 对象内新旧形态共存 | `"dateFormat": {"excelFormat":"yyyy/M/d", "excel":{"type":"string","format":"yyyy/M/d"}}` | 含 slot 名和不可混用提示 |
| `type="epochSec"` 用于 dateFormat.db | `"dateFormat": {"db": {"type":"epochSec"}}` | 含 "epochSec is only allowed on datetimeFormat.db" |
| `type="epochSec"` 用于 excel side | `"datetimeFormat": {"excel": {"type":"epochSec","format":""}}` | 含 "epochSec is only allowed on db side" |

> **与 §III-E 的分工**：§III-E 验证 epochSec 在 **运行时**（导入/导出）的端到端行为；§III-F 验证新形态在 **Profile 加载时**的语义正确性和等价性。两者互补。

---

# 场景 IV：导出列顺序控制

## IV-1 验证目标

| 序号 | 验收点 | 来源 |
|---|---|---|
| IV-V1 | `columnOrder` 声明的列依序排在 Excel 最前，未声明列以 SQL 投影顺序追加于后 | 能力 `export-column-order` §声明列优先 |
| IV-V2 | `columnOrder` 包含所有列时，输出恰为 `columnOrder` 本身，无追加后缀 | 能力 §全量声明 |
| IV-V3 | Mixed 模式 `classColumn` 不在 `columnOrder` → 自动前置为第一列 | 能力 §classColumn 默认前置 |
| IV-V4 | Mixed 模式 `classColumn` 在 `columnOrder` 中 → 按声明位置放置，不再自动前置 | 能力 §classColumn 显式定位 |
| IV-V5 | `columnOrder` 与 `orderBy` 正交：行排序与列顺序可同时独立生效 | 能力 §正交性 |
| IV-V6 | 列顺序重排不修改、不丢失、不重复任何列的数据值 | 能力 §数据完整性 |
| IV-V7 | 负向：未知表头 `E_EXPORT_UNKNOWN_HEADER`（大小写敏感）；重复条目 `E_EXPORT_DUPLICATE_ORDER`；与 raw SQL 并存 `E_EXPORT_ORDER_WITH_RAW_SQL` | 能力 §验证约束 |

## IV-2 验证拓扑

```
Excel: OrdersColOrder.xlsx (单 Sheet "Orders")
   │  OrderNo, TenantId, Total
   ▼
DataBridge.importExcel(profile=column_order.json)
   ▼
SQLite: scenarioIV.db  →  orders(order_no PK, tenant_id, total)
   ▼
DataBridge.exportExcel(columnOrder=["Total","OrderNo","TenantId"], orderBy=["order_no"])
   ▼
OrdersColOrder.exported.xlsx
   └── 列顺序：Total | OrderNo | TenantId
       行排序：order_no 升序
```

## IV-3 数据准备

### IV-3.1 Schema（路径 `tests/data/sql/06_column_order.sql`）

```sql
CREATE TABLE IF NOT EXISTS orders (
    order_no  TEXT PRIMARY KEY,
    tenant_id TEXT NOT NULL,
    total     REAL DEFAULT 0
);
```

初始化：

```bash
sqlite3 /tmp/scenarioIV.db < tests/data/sql/06_column_order.sql
```

### IV-3.2 Excel 输入（`tests/data/xlsx/OrdersColOrder.xlsx`）

单 Sheet 名 `Orders`，**共 3 行业务数据**：

| OrderNo | TenantId | Total |
|---------|----------|-------|
| SO-001  | TENANT-A | 100.0 |
| SO-002  | TENANT-B | 200.0 |
| SO-003  | TENANT-A | 50.0  |

### IV-3.3 Profile（复用 `tests/data/profiles/column_order.json`）

```json
{
  "profileName": "column_order",
  "sheet": "Orders",
  "mode": "singleTable",
  "table": "orders",
  "conflict": { "columns": ["order_no"] },
  "columns": {
    "order_no":  { "source": "OrderNo",  "validators": ["notNull"] },
    "tenant_id": { "source": "TenantId", "validators": ["notNull"] },
    "total":     { "source": "Total" }
  },
  "export": {
    "orderBy": ["order_no"],
    "columnOrder": ["Total", "OrderNo", "TenantId"]
  }
}
```

SQL 投影自然顺序（按 `columns` 声明顺序）：`order_no → tenant_id → total`。`columnOrder` 包含全部 3 列，期望输出顺序为 `Total → OrderNo → TenantId`（IV-V2）。

## IV-4 执行步骤

### IV-4.1 导入

```bash
./build/examples/cli/dbridge-cli \
  /tmp/scenarioIV.db \
  tests/data/profiles/column_order.json \
  tests/data/xlsx/OrdersColOrder.xlsx \
  import
```

期望：无错误，`Imported 3 rows`。

### IV-4.2 SQL 断言

```sql
SELECT order_no, tenant_id, total FROM orders ORDER BY order_no;
-- 期望：SO-001/TENANT-A/100.0，SO-002/TENANT-B/200.0，SO-003/TENANT-A/50.0
```

### IV-4.3 导出（IV-V1 / IV-V2 / IV-V5 / IV-V6）

```bash
./build/examples/cli/dbridge-cli \
  /tmp/scenarioIV.db \
  tests/data/profiles/column_order.json \
  /tmp/OrdersColOrder.exported.xlsx \
  export
```

```bash
python3 tools/xlsx2csv.py /tmp/OrdersColOrder.exported.xlsx > /tmp/out_IV.csv
head -1 /tmp/out_IV.csv   # 应为：Total,OrderNo,TenantId
```

目视核查：
- 第 1 行（表头）：`Total, OrderNo, TenantId`（IV-V1 / IV-V2）。
- 数据行按 `order_no` 升序：SO-001、SO-002、SO-003（IV-V5）。
- 每行数值与导入输入完全一致，无精度损失（IV-V6）。

### IV-4.4 Mixed 模式 classColumn 位置验证（IV-V3 / IV-V4）

使用如下**本地构造** Profile（`mixed_colorder_test.json`，无需签入）配合场景 II 的 `scenarioII.db`：

```json
{
  "sheet": "Mixed",
  "mode": "mixed",
  "discriminator": { "source": "Type" },
  "classes": [
    {
      "id": "A",
      "match": { "equals": "A" },
      "routes": [{
        "table": "orders",
        "conflict": { "columns": ["order_no"] },
        "columns": {
          "order_no": { "source": "OrderNo" },
          "total":    { "source": "Total" }
        }
      }]
    }
  ],
  "export": {
    "classColumn": "Type",
    "columnOrder": ["OrderNo", "Total"]
  }
}
```

**IV-V3**（classColumn 不在 columnOrder）：期望 Excel 表头 = `Type, OrderNo, Total`（Type 自动前置）。

修改 `columnOrder` 为 `["OrderNo", "Type", "Total"]` 后再导出（**IV-V4**）：期望 Excel 表头 = `OrderNo, Type, Total`（Type 按声明位置插入，不再自动前置）。

### IV-4.5 负向场景（IV-V7）

> **重要：`columnOrder` 错误在 `import` 阶段触发，不在 `export` 阶段。**
> `ProfileValidator`（由 `ImportService` 调用）在导入时对 Profile 做三方对账；`ExportService` 直接使用已验证的 Profile，不再重新校验。因此，下列负向用例需用 `dbridge-cli ... import`（不是 `export`）来触发错误。

| 场景 | 制造方式 | 触发命令 | 期望错误码 | 期望消息 |
|---|---|---|---|---|
| 未知表头 | `columnOrder: ["Total", "OrderNo", "Typo"]` | `import` | `E_EXPORT_UNKNOWN_HEADER` | 含 `"Typo"` |
| 大小写不匹配 | `columnOrder: ["total", "OrderNo"]`（源为 `"Total"`）| `import` | `E_EXPORT_UNKNOWN_HEADER` | 含 `"total"` |
| 重复条目 | `columnOrder: ["OrderNo", "Total", "OrderNo"]` | `import` | `E_EXPORT_DUPLICATE_ORDER` | 含 `"OrderNo"` |
| 与 raw SQL 并存 | 同时设 `export.explicitSql: "SELECT ..."` 和 `columnOrder: ["Total"]` | `import` | `E_EXPORT_ORDER_WITH_RAW_SQL` | — |

---

# 场景 V：导出反向 lookup（无损往返）

## V-1 验证目标

| 序号 | 验收点 | 来源 |
|---|---|---|
| V-V1 | `exportRoundtrip: true`（默认）时，导出行中 H 列（`select[].dbColumn`）消失，A 列（`match[].Excel_header`）出现并填入反向查找值 | 能力 `export-reverse-lookup` §H 列移除 A 列恢复 |
| V-V2 | `exportRoundtrip: false` 时 H 列原样出现，A 列不出现 | 能力 §exportRoundtrip 开关 |
| V-V3 | `exportOnMissing: "null"` → 未命中时 A 列写空格，行继续，无错误 | 能力 §exportOnMissing |
| V-V4 | `exportOnMissing: "skip"` → 行为同 `"null"`（A 列写空，行继续），不计入错误统计；用于已知旧数据兼容（区别仅为 error counting，不影响行输出） | 能力 §exportOnMissing |
| V-V5 | `exportOnMissing: "error"`（默认）→ 未命中时 emit `E_REVERSE_LOOKUP_NOT_FOUND`，行跳过，其余行继续 | 能力 §exportOnMissing |
| V-V6 | 多行命中（歧义）→ `E_REVERSE_LOOKUP_AMBIGUOUS`，不可被任何 `exportOnMissing` 值压制 | 能力 §歧义强制报错 |
| V-V7 | 预取 SELECT 失败 → `E_REVERSE_LOOKUP_QUERY_FAILED`，整 sheet 中止 | 能力 §预取失败 |
| V-V8 | 批量预取：N 行 K 个不同 H 值 → 恰好执行 `ceil(K / chunk_limit)` 次 SELECT，不逐行查询 | 能力 §批量预取 |
| V-V9 | 同 identity lookup 跨 route 共享一次预取结果（identity merging）| 能力 §identity 合并 |
| V-V10 | 导入 + 导出组合往返后，输出 Excel 中 A 列值与导入 Excel 中 A 列值一致（无损往返） | 端到端 roundtrip |

## V-2 验证拓扑

```
ref_customers(c_no PK, c_name)  ←── 参考表（预先种入，不经 Excel 导入）
   │
   │ import: match c_no = 客户编号
   │         select c_name → customer_name（H 列，存入 orders）
   ▼
Excel: ReverseLookup.xlsx (Sheet "Orders")
   │  OrderNo, OrderDate, 客户编号
   ▼
DataBridge.importExcel(profile=reverse_lookup.json)
   └── orders(order_no PK, order_date, customer_name)   ← H 列存库，A 列不存库
   ▼
DataBridge.exportExcel(exportRoundtrip=true)
   └── 反向 lookup：customer_name → ref_customers → c_no → 写入"客户编号"列
   ▼
ReverseLookup.exported.xlsx
   └── 列：OrderNo | OrderDate | 客户编号（A 列还原，customer_name 列不出现）
```

## V-3 数据准备

### V-3.1 Schema（路径 `tests/data/sql/07_reverse_lookup.sql`）

```sql
CREATE TABLE IF NOT EXISTS ref_customers (
    c_no   TEXT PRIMARY KEY,
    c_name TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS orders (
    order_no      TEXT PRIMARY KEY,
    order_date    TEXT,
    customer_name TEXT            -- H 列：导入时由 lookup select 填入
);
```

初始化并种入参考数据：

```bash
sqlite3 /tmp/scenarioV.db < tests/data/sql/07_reverse_lookup.sql
sqlite3 /tmp/scenarioV.db "
  INSERT INTO ref_customers VALUES ('C001', 'Alice Corp');
  INSERT INTO ref_customers VALUES ('C002', 'Bob Inc');
"
```

### V-3.2 Excel 输入（`tests/data/xlsx/ReverseLookup.xlsx`）

单 Sheet 名 `Orders`，**共 3 行业务数据**：

| OrderNo | OrderDate  | 客户编号 |
|---------|------------|---------|
| SO-001  | 2026-05-21 | C001    |
| SO-002  | 2026-05-22 | C002    |
| SO-003  | 2026-05-23 | C001    |

- `客户编号` 是 A 列（`match[].Excel_header`）；导入时用它匹配 `ref_customers.c_no`，拉取 `c_name` 写入 `orders.customer_name`（H 列）。
- 导出时反向：`customer_name` → 查 `ref_customers` → 还原 `c_no` → 写回 `客户编号` 列。

### V-3.3 Profile（路径 `tests/data/profiles/reverse_lookup.json`，需本地创建）

```json
{
  "profileName": "reverse_lookup",
  "sheet": "Orders",
  "headerRow": 1,
  "mode": "singleTable",
  "table": "orders",
  "conflict": { "columns": ["order_no"] },
  "lookups": [
    {
      "name": "cust",
      "from": "ref_customers",
      "match":  [["c_no", "客户编号"]],
      "select": [["c_name", "customer_name"]],
      "exportRoundtrip": true,
      "exportOnMissing": "error"
    }
  ],
  "columns": {
    "order_no":   { "source": "OrderNo",   "validators": ["notNull"] },
    "order_date": { "source": "OrderDate" }
  },
  "export": { "orderBy": ["order_no"] }
}
```

- `columns` 中无需声明 `customer_name`——它由 lookup 的 `select` 自动填入。
- `exportRoundtrip: true`（默认）使导出路径执行反向查找。
- `exportOnMissing: "error"`（默认）在未命中时报错跳行。

## V-4 执行步骤

### V-4.1 导入

```bash
./build/examples/cli/dbridge-cli \
  /tmp/scenarioV.db \
  tests/data/profiles/reverse_lookup.json \
  tests/data/xlsx/ReverseLookup.xlsx \
  import
```

期望：无错误，`Imported 3 rows`。

### V-4.2 SQL 断言（V-V1 前置确认）

```sql
-- 确认 H 列（customer_name）已由 lookup 填入，不是 NULL
SELECT order_no, customer_name FROM orders ORDER BY order_no;
-- 期望：SO-001→'Alice'，SO-002→'Bob'，SO-003→'Alice'

-- 确认 A 列（客户编号）没有存入 orders 表（orders 表无此列）
PRAGMA table_info(orders);
-- customer_name 列存在；不存在名为 '客户编号' 的列（汉字列名未落库）
```

### V-4.3 导出往返（V-V1 / V-V8 / V-V10）

```bash
./build/examples/cli/dbridge-cli \
  /tmp/scenarioV.db \
  tests/data/profiles/reverse_lookup.json \
  /tmp/ReverseLookup.exported.xlsx \
  export
```

```bash
python3 tools/xlsx2csv.py tests/data/xlsx/ReverseLookup.xlsx     > /tmp/in_V.csv
python3 tools/xlsx2csv.py /tmp/ReverseLookup.exported.xlsx       > /tmp/out_V.csv
```

**注意：列顺序差异属预期行为。** 输入 Excel 列顺序为 `OrderNo,OrderDate,客户编号`，而导出列顺序为 `OrderDate,OrderNo,客户编号`（DB schema 的字典序，因未声明 `columnOrder`）。因此 `sort | sha256sum` 哈希**不会相同**。V-V10 无损往返的通过准则是**值等价**，而非哈希一致：

```bash
# V-V10 值等价验证：客户编号列按 order_no 排序后应与输入一致
cut -d, -f3 /tmp/in_V.csv  | tail -n +2 | sort   # 输入 A 列
cut -d, -f3 /tmp/out_V.csv | tail -n +2 | sort   # 输出 A 列（应相同）
```

目视核查导出文件：
- 表头包含 `客户编号`（A 列已恢复）。
- 表头**不包含** `customer_name`（H 列已移除）（V-V1）。
- `客户编号` 值为 `C001`、`C002`、`C001`，与导入输入一致。

### V-4.4 `exportRoundtrip: false` 验证（V-V2）

修改 Profile 中 `"exportRoundtrip": false`，重新导出：

```bash
./build/examples/cli/dbridge-cli \
  /tmp/scenarioV.db \
  /tmp/reverse_lookup_no_roundtrip.json \
  /tmp/ReverseLookup.noroundtrip.xlsx \
  export
```

期望：
- 表头包含 `customer_name`（H 列原样出现）。
- 表头**不包含** `客户编号`（A 列不恢复）（V-V2）。

### V-4.5 负向场景（V-V3 ~ V-V7）

| 场景 | 制造方式 | 期望结果 |
|---|---|---|
| `exportOnMissing: "error"`（默认），无命中 | 向 orders 插入 `order_no='SO-999', customer_name='Unknown Corp'`，保留 ref_customers 不变 | `E_REVERSE_LOOKUP_NOT_FOUND`（含 `"cust"` 和 `"Unknown Corp"`）；SO-999 行跳过，其余行正常输出 |
| `exportOnMissing: "null"`，无命中 | 同上，Profile 改 `"exportOnMissing": "null"` | SO-999 行输出，`客户编号` 格为空，无错误 |
| `exportOnMissing: "skip"`，无命中 | 同上，Profile 改 `"exportOnMissing": "skip"` | 行为同 `"null"`（A 列写空，行继续）；区别仅为该缺失不计入错误统计，用于已知旧数据兼容 |
| 歧义多行命中（V-V6） | 向 ref_customers 插入 `('C003','Alice Corp')`（c_name 重复），导出含 SO-001/SO-003 的行 | `E_REVERSE_LOOKUP_AMBIGUOUS`（消息含 `"cust"`、命中数 `2`）；无论 `exportOnMissing` 设何值，均不可压制 |
| 预取 SELECT 失败（V-V7） | 将 `ref_customers` 表改名后执行导出 | `E_REVERSE_LOOKUP_QUERY_FAILED`，整 sheet 导出中止，不输出任何行 |

---

## 通过准则（五个场景共用）

只有同时满足以下条件，整套功能才视为验证通过：

1. 场景 I §I-4 全部断言通过。
2. 场景 II §II-4 全部断言通过；尤其确认 A/B/C 行**没有**互相污染对方的表集合。
3. 场景 III §III-4 全部 SQL 断言和负向场景通过；导出时间字段呈现正确 `excelFormat` 格式。
4. 场景 III §III-E epochSec 路径通过：`happen_at` 存储为整数、NULL vs 0 区分正确、错误格式触发 `E_TIME_PARSE_DB`。
5. 场景 III §III-F 新形态路径通过：新形态等价旧形态、side 级整体覆盖语义正确、旧新跨层混用加载成功、六项 `E_PROFILE_PARSE` 均准确触发。
6. 场景 IV §IV-4 全部断言通过；CSV 表头顺序与 `columnOrder` 声明完全一致；负向错误码准确触发。
7. 场景 V §V-4 往返数据值等价（A 列 `客户编号` 排序后与输入一致；列顺序因无 `columnOrder` 而按 DB schema 字典序，`sort | sha256sum` 不相等属预期行为）；负向场景错误码准确触发；`exportRoundtrip: false` 时 H/A 列切换正确。
8. 所有场景的负向用例均返回预期错误码，不出现引擎崩溃或静默数据污染。

---

## 工具：`tools/xlsx2csv.py`

`tools/xlsx2csv.py` 是为本验证流程配套的 xlsx → CSV 转储器，**纯 Python 标准库实现**（`zipfile` + `xml.etree.ElementTree`），与 dbridge 主仓库独立，不需要 `pip install`。它在 §I-4.3 / §II-4.3 的导出对账步骤中把输入与输出两份 xlsx 拉平成 CSV 后做哈希比对。

### 调用形式

```bash
python3 tools/xlsx2csv.py <path.xlsx> [--sheet <name>]
```

| 选项 | 含义 |
|---|---|
| `<path.xlsx>` | 输入 xlsx 文件路径（位置参数，必填） |
| `--sheet <name>` | 指定 sheet 名；省略时取 `xl/workbook.xml` 中**第一个**声明的 sheet |

CSV 写到 stdout。退出码：

| code | 含义 |
|---|---|
| `0` | 成功 |
| `2` | sheet 不存在 / 无 sheet（stderr 会打印可用的 sheet 名列表） |

### 解析能力

| Excel 形态 | 处理结果 |
|---|---|
| Shared string（`t="s"` + `<v>` index） | 解 `xl/sharedStrings.xml` 还原原文 |
| Inline string（`t="inlineStr"` + `<is><t>`） | 直接读出 |
| 数值（`t="n"` 或无 `t`） | 原样输出 |
| 布尔（`t="b"`） | `TRUE` / `FALSE` |
| 错误（`t="e"`） | 输出错误码字符串（如 `#DIV/0!`） |
| 公式（`<f>` + cached `<v>`） | 取缓存值，不二次求值 |
| 日期单元格（数值 + 日期样式） | 读 `xl/styles.xml`，识别内建 numFmtId（14–22 / 27–36 / 45–47 / 50–58）与含 `y`/`d` 的自定义格式 → 转 ISO 日期 `2026-05-20` |
| 时间单元格（数值 + 仅含 `h`/`s` 的格式） | 转 `HH:MM:SS` |
| 含日期与时间的格式 | 转 `YYYY-MM-DD HH:MM:SS` |
| 稀疏行 / 稀疏列 | 缺失单元格填空字符串，行宽对齐全表最大列 |

### 日期换算说明（Excel 1900 闰年 bug）

Excel 1900 模式有"1900-02-29 不存在却被算作存在"的著名 bug。脚本按惯例处理：

- `serial >= 60` → 锚点 `1899-12-30`（跳过虚构的 1900-02-29）
- `serial < 60` → 锚点 `1899-12-31`

实例：`46162` → `1899-12-30 + 46162 天` = **`2026-05-20`**，与 Excel UI 显示一致。

### 局限

- 单次调用只 dump 一个 sheet；多 sheet 需多次调用。
- 公式不重新求值，依赖 Excel 写出来的 `<v>` 缓存值（与 Excel UI 显示一致即可）。
- 日期识别基于 numFmtId / 格式码字面；用户自造的奇怪格式（不含 `y/d/h/s` 字符）会被视作普通数值。
- 输出 CSV 用 Python 内置 `csv` 模块，不带 BOM，UTF-8。

### 与 dbridge 配合的对账配方

```bash
# 1. 导出 xlsx
./build/examples/cli/dbridge-cli /tmp/scenarioII.db \
    tests/data/profiles/mixed_abc_multitable.json \
    /tmp/Mixed.exported.xlsx export

# 2. 拉平两份 xlsx 为 CSV
python3 tools/xlsx2csv.py tests/data/xlsx/Mixed.xlsx > /tmp/in.csv
python3 tools/xlsx2csv.py /tmp/Mixed.exported.xlsx > /tmp/out.csv

# 3. 排序后做哈希比对（顺序无关）
sort /tmp/in.csv  | sha256sum
sort /tmp/out.csv | sha256sum
```

注意：dbridge 导入侧走 QXlsx（也读得懂日期样式），所以**库内 `eta` 列存的是 ISO 字符串**；导出时再写成 xlsx，eta 单元格会回归"日期格式 + serial"。两份 xlsx 经 `xlsx2csv.py` 拉平后日期都会被还原为同一形态的 ISO 字符串，对账 sha256 才能命中。

### 自检（开发用）

工具自身的最小回归就是这套验证流程：场景 I / II 的对账步骤实际跑通即可。如果未来给它加新形态（多 sheet / 流式 / 日期时区），建议同时在 `docs/validation/row-to-multitable.md` 末尾追加最小测试用例描述，避免再次出现"工具显示与 dbridge 实际行为不一致"那种坑。

---

## 文件清单

| 文件 | 是否已签入 | 说明 |
|---|---|---|
| `tests/data/sql/02_orders.sql` | ✅ | 场景 I schema |
| `tests/data/profiles/order_m_set.json` | ✅ | 场景 I Profile |
| `tests/data/xlsx/Orders.xlsx` | ✅ | 场景 I 输入 Excel |
| `tests/data/sql/04_mixed_multitable.sql` | ✅ | 场景 II schema |
| `tests/data/profiles/mixed_abc_multitable.json` | ✅ | 场景 II Profile |
| `tests/data/xlsx/Mixed.xlsx` | ✅ | 场景 II 输入 Excel |
| `tests/data/sql/05_time_formats.sql` | ✅ | 场景 III schema（`event` 表） |
| `tests/data/profiles/time_formats.json` | ✅ | 场景 III Profile |
| `tests/data/xlsx/Events.xlsx` | ✅ | 场景 III 输入 Excel |
| `tests/data/sql/06_column_order.sql` | ✅ | 场景 IV schema（`orders` 简化版，只含 order_no / tenant_id / total） |
| `tests/data/profiles/column_order.json` | ✅ | 场景 IV Profile |
| `tests/data/xlsx/OrdersColOrder.xlsx` | ✅ | 场景 IV 输入 Excel |
| `tests/data/sql/07_reverse_lookup.sql` | ✅ | 场景 V schema（`ref_customers` + `orders`） |
| `tests/data/profiles/reverse_lookup.json` | ✅ | 场景 V Profile |
| `tests/data/xlsx/ReverseLookup.xlsx` | ✅ | 场景 V 输入 Excel |
| `tools/xlsx2csv.py` | ✅ | 对账辅助脚本（纯 Python 标准库实现，无外部依赖） |
| `tools/build_fixtures.py` | ✅ | 生成全部 xlsx 夹具的脚本（纯 Python stdlib；重新生成：`python3 tools/build_fixtures.py`） |
| `tests/unit/tst_fk_preflight.cpp` | ✅ | FK 预校验回归测试（7 用例，含 `testMixedParentInBatch` / `testParentMissing` / `testLookupDerivedGroupSkipsProbe`） |

所有夹具（schema SQL、Profile JSON、xlsx 二进制）均已签入仓库。若需重新生成 xlsx，运行 `python3 tools/build_fixtures.py` 即可。

---

## 相关文档

- 项目主 [`README.md`](../../README.md) — dbridge 用法、API、构建（§14 完整使用指南）
  - [§14.13.2 `tools/xlsx2csv.py` 简介](../../README.md#14132-toolsxlsx2csvpy验证对账脚本)
  - [§14.16 端到端验证流程入口](../../README.md#1416-端到端验证流程)
- `specs/MVP-Qt-SQLite-Excel-批量导入导出-实现设计.md` — MVP 行为契约（§1.4 单类多表 / §1.5 混编 / §5 Profile 设计）
- `specs/Qt-SQLite-Excel-批量导入导出-实现文档.md` — 模块/算法/SQL 模板（§6.3 拓扑 / §6.6 FK 注入 / §6.7 Discriminator）
- `specs/Qt-SQLite-Excel-批量导入导出-设计文档.md` — 完整方案与长期取舍
- `specs/长期架构演进-Qt-SQLite-Excel-批量导入导出.md` — 非 MVP 演进项（如代理键 FK 回填）
