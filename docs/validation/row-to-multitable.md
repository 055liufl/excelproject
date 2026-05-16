# 验证流程：一行 → 多表 与 多行 → 不同表集合

> 本文是 dbridge 的一份**端到端验证流程文档**，承载 MVP 设计 §1 中"行 ↔ 表集合"映射目标的两个核心场景。它是项目主 [`README.md`](../../README.md) 的补充材料：主 README 讲怎么用 dbridge，本文讲怎么证明它确实把"一行→多表"和"多行→不同表集合"两件事做对了。
>
> 项目代码结构与公共 API 见主 README §3、§14；本文不重复。

---

## 0. 验证全景

本文按"两个并列场景"组织，分别覆盖 MVP 设计 §1 中关于「行 ↔ 表集合」映射的两个硬目标：

| 场景 | 一句话 | 对应 MVP 目标 | 仓库夹具 |
|---|---|---|---|
| **场景 I**：单类行 → 多表集合 | 同一个 Sheet 的**每一行**同时进入父表+子表（m 集合） | §1.4 | ✅ `tests/data/profiles/order_m_set.json` + `tests/data/sql/02_orders.sql`（Orders.xlsx 本地构造） |
| **场景 II**：多类行 → 各自的不同表集合 | 同一个 Sheet 中**多行 A/B/C**，每一类各自落入不同的多表集合（m / n / o） | §1.5 | ✅ `tests/data/profiles/mixed_abc_multitable.json` + `tests/data/sql/04_mixed_multitable.sql`（Mixed.xlsx 本地构造）；另有 `tests/data/profiles/mixed_abc.json` 作为单表精简版（m1/n1/o1）保留，与本场景互不替代 |

两个场景互补：
- 场景 I 证明"**一行**可以同时进**多张表**"。
- 场景 II 证明"**多行**之间可以各自路由到**完全不同的表集合**"。

只有同时通过，"一行对多表 + 多行对多表集合"这一整组需求才视为验证闭环。

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

### I-3.2 Excel 输入（本地构造，约定路径 `tests/data/xlsx/Orders.xlsx`）

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
      "fkInject": { "from": "orders.order_no", "to": "order_items.order_no" },
      "conflict": {"columns": ["order_no", "line_no"]},
      "columns": { "line_no": {}, "sku": {}, "qty": {} }
    }
  ],
  "export": { "orderBy": ["orders.order_no", "order_items.line_no"] }
}
```

Profile 故意**不**显式映射 `OrderNo → order_items.order_no`，强制由 `fkInject` 注入，用以验证 I-V3。

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

| 场景 | 制造方式 | 期望错误码 |
|---|---|---|
| 父业务键缺失 | 第 2 行 `OrderNo` 清空 | `E_VALIDATE_NULL` |
| 同批 conflict 冲突且非 key 字段不一致 | 第 1、2 行 `Amount` 改成不同值 | `E_VALIDATE_DUPLICATE` |
| Profile 拓扑成环 | 给 `orders` 加 `parent: "order_items"` | `E_PROFILE_TOPOLOGY_CYCLE` |
| FK 引用不存在 | 子行 `OrderNo` 改成数据库中不存在的订单号且父行已被删 | `E_VALIDATE_FK` |

每个用例运行后 `SELECT COUNT(*)` 两张表都应为 0。

---

# 场景 II：多类行 → 各自的不同表集合（m / n / o）

> **夹具状态**：本场景的 schema / Profile 已签入仓库（路径见 §II-3.1 / §II-3.3）。`Mixed.xlsx` 是 xlsx 二进制不签入仓库，按 §II-3.2 表格在本地构造即可。本场景已在 master HEAD 通过端到端验证：6 张表行数、FK 完整性、A/B/C 集合互不污染、日期字段 ISO 形态均符合预期；mixed 模式下的 FK 预校验路径由 `tst_fk_preflight` 单元测试守护，避免历史误报 `E_VALIDATE_FK` 的回归。
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

### II-3.2 Excel 输入（本地构造，约定路径 `tests/data/xlsx/Mixed.xlsx`）

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
          "fkInject": { "from": "orders.order_no", "to": "order_items.order_no" },
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
          "fkInject": { "from": "shipments.shipment_no", "to": "shipment_legs.shipment_no" },
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
          "fkInject": { "from": "invoices.invoice_no", "to": "invoice_lines.invoice_no" },
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
| 未匹配类 | 把第 3 行 `Type` 改为 `X` | `E_ROUTE_UNMATCHED`（指向第 3 行 `Type=X`） | 6 张表全部 0 |
| 跨集合污染尝试 | 把 C 行 `Type` 误填为 `A`，但 `OrderNo` 列为空 | `E_VALIDATE_NULL`（A class 的 `OrderNo`） | 6 张表全部 0 |
| n 集合 FK 缺失 | B 行 `ShipmentNo` 清空 | `E_VALIDATE_NULL` | 6 张表全部 0 |
| 子行业务键越界 | C 行 `InvLineNo` 改成 0 | `E_VALIDATE_INT`（`int>=1`） | 6 张表全部 0 |
| FK 父行批/库均缺失 | 子行 FK 值在批内与 DB 中都查不到 | `E_VALIDATE_FK` —— 单元回归用例 `tst_fk_preflight::testParentMissing` 覆盖 | 6 张表全部 0 |

每个用例运行后均执行：

```sql
SELECT (SELECT COUNT(*) FROM orders)
     + (SELECT COUNT(*) FROM order_items)
     + (SELECT COUNT(*) FROM shipments)
     + (SELECT COUNT(*) FROM shipment_legs)
     + (SELECT COUNT(*) FROM invoices)
     + (SELECT COUNT(*) FROM invoice_lines) AS total_rows;
-- 期望 0
```

---

## 通过准则（两个场景共用）

只有同时满足以下条件，"一行→多表 + 多行→多表集合"才算验证通过：

1. 场景 I §I-4 全部断言通过。
2. 场景 II §II-4 全部断言通过；尤其要确认 A/B/C 行**没有**互相污染对方的表集合。
3. 两个场景的负向用例都返回预期错误码，库内计数仍为 0。
4. 两个场景的导出对账 CSV 哈希一致。

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
| `tests/data/xlsx/Orders.xlsx` | ❌ 本地构造 | 场景 I 输入 Excel（按 §I-3.2 构造，xlsx 二进制不签入仓库） |
| `tests/data/sql/04_mixed_multitable.sql` | ✅ | 场景 II schema |
| `tests/data/profiles/mixed_abc_multitable.json` | ✅ | 场景 II Profile |
| `tests/data/xlsx/Mixed.xlsx` | ❌ 本地构造 | 场景 II 输入 Excel（按 §II-3.2 构造，xlsx 二进制不签入仓库） |
| `tools/xlsx2csv.py` | ✅ | 对账辅助脚本（纯 Python 标准库实现，无外部依赖） |
| `tests/unit/tst_fk_preflight.cpp` | ✅ | mixed 模式 FK 预校验回归测试（4 用例，含 `testMixedParentInBatch` / `testParentMissing`） |

`Orders.xlsx` 与 `Mixed.xlsx` 是二进制文件，按惯例不直接签入仓库——由验证执行者依照 §I-3.2 / §II-3.2 的表格内容用 Excel / LibreOffice / `openpyxl` 构造即可。后续若要把这两份 xlsx 也沉淀为可重复夹具，建议增加 `tools/build_fixtures.py` 从 JSON 描述用 QXlsx/openpyxl 重新生成。

---

## 相关文档

- 项目主 [`README.md`](../../README.md) — dbridge 用法、API、构建（§14 完整使用指南）
  - [§14.13.2 `tools/xlsx2csv.py` 简介](../../README.md#14132-toolsxlsx2csvpy验证对账脚本)
  - [§14.16 端到端验证流程入口](../../README.md#1416-端到端验证流程)
- `specs/MVP-Qt-SQLite-Excel-批量导入导出-实现设计.md` — MVP 行为契约（§1.4 单类多表 / §1.5 混编 / §5 Profile 设计）
- `specs/Qt-SQLite-Excel-批量导入导出-实现文档.md` — 模块/算法/SQL 模板（§6.3 拓扑 / §6.6 FK 注入 / §6.7 Discriminator）
- `specs/Qt-SQLite-Excel-批量导入导出-设计文档.md` — 完整方案与长期取舍
- `specs/长期架构演进-Qt-SQLite-Excel-批量导入导出.md` — 非 MVP 演进项（如代理键 FK 回填）
