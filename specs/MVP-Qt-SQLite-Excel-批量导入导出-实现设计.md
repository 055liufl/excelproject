# Qt + SQLite + Excel 批量导入导出 MVP 实现设计

> 目标：先实现“Qt + SQLite + Excel 导入导出”的最小可落地版本，同时覆盖本项目必须实现的五类业务目标。  
> 范围：Qt 5.12.12 + C++17 + QtSql + QXlsx + SQLite。  
> 输出形态：可被宿主程序调用的 C++ 动态库或静态库；不承诺跨编译器/跨 Qt 版本 ABI 稳定。

---

## 1. 必须实现的设计目标

MVP 必须支持以下能力：

1. **Upsert 导入**
   - 导入时根据 Profile 指定的主键或唯一键判断数据是否已存在。
   - 已存在：执行原地 `UPDATE`。
   - 不存在：执行 `INSERT`。
   - 禁止使用 `INSERT OR REPLACE`，避免 `DELETE + INSERT` 导致外键级联和历史列丢失。

2. **导入前校验与告警终止**
   - 数据落库前完成结构、类型、必填、长度、正则、枚举、批内唯一性、外键存在性等校验。
   - 只要发现错误，终止本次导入，不写入任何数据。
   - 返回可定位的问题清单：Sheet、行号、列名、原值、错误码、错误说明。

3. **未来新增表导入导出**
   - 对当前编译期未知的 SQLite 表，通过运行期自省获取表结构。
   - 单表场景可自动生成导入/导出映射：Excel 表头名与数据库列名一致。
   - 复杂多表或混编场景不靠代码重编译解决，而是通过新增 JSON Profile 描述映射关系。

4. **单类行到多表集合**
   - 一个 Sheet 中的 A 行数据可拆分导入到多张表，即 m 集合。
   - 支持父子表按依赖顺序写入。
   - 支持从 m 集合导出并组装回同一个 Sheet。

5. **混编行到多表集合**
   - 一个 Sheet 中 A/B/C 三类数据可以混杂出现。
   - A 行导入 m 集合，B 行导入 n 集合，C 行导入 o 集合。
   - 反向导出时，从 m/n/o 集合查询并组装回同一个 Sheet。

---

## 2. MVP 边界

### 2.1 本期包含

| 能力 | MVP 做法 |
|---|---|
| 数据库 | 仅 SQLite |
| Excel | QXlsx 普通读写，不自研流式 xlsx reader |
| 映射配置 | JSON Profile |
| 导入 | 单 Sheet，支持单表、多表、A/B/C 混编 |
| 导出 | 单 Sheet，支持单表、多表集合、A/B/C 混编 |
| Upsert | SQLite `INSERT ... ON CONFLICT DO UPDATE` |
| 事务 | 默认 `AllOrNothing`，一次导入一个事务 |
| 校验 | 导入前全量校验；失败则整体终止 |
| 未知表 | 单表 auto profile；复杂场景人工提供 Profile |
| 执行模型 | 单线程 DB 写入；可由宿主放到后台线程 |
| 告警 | 返回结构化错误列表，由宿主 UI 展示 |

### 2.2 本期不做

| 不做项 | 原因 |
|---|---|
| MySQL / Oracle / PostgreSQL 驱动 | MVP 先验证 SQLite 端到端 |
| 数据库插件 SPI | 抽象成本高，等 SQLite 路径稳定后再提取 |
| 自研 zip + `QXmlStreamReader` 流式 xlsx | 工作量大，先不承诺百万行 |
| `ChunkCommit` / `ContinueOnError` | 首版保持失败即回滚的简单语义 |
| 二分拆批定位 DB 错误 | 首版依赖前置校验和 SQLite 原生错误兜底 |
| C ABI / 跨编译器 ABI 矩阵 | 首版面向同工程或同工具链集成 |
| 多线程流水线 | 先保证正确性，再优化吞吐 |
| 脚本表达式引擎 | Profile 保持声明式，避免执行不可信脚本 |

---

## 3. 总体架构

```
Host Application
      |
      v
DataBridge API
      |
      +-- ProfileLoader      读取 JSON Profile
      +-- SchemaIntrospector SQLite 表结构自省
      +-- ExcelReader        QXlsx 读取 Sheet 行
      +-- Validator          导入前校验
      +-- Router             A/B/C 行分类
      +-- Mapper             Excel 行 <-> 表 RowPayload
      +-- TopoSorter         多表依赖排序
      +-- SqlBuilder         生成 SQLite Upsert / Select
      +-- ImportService      导入事务编排
      +-- ExportService      导出查询与组装
      +-- ExcelWriter        QXlsx 写出 Sheet
```

模块边界：

| 模块 | 职责 |
|---|---|
| `DataBridge` | 对外门面，管理连接、Profile、导入导出 |
| `ProfileLoader` | 解析和校验 JSON Profile |
| `SchemaIntrospector` | 读取 SQLite 表、列、主键、唯一索引、外键 |
| `ExcelReader` | 读取指定 Sheet、表头、数据行 |
| `Validator` | 校验行数据，生成错误列表 |
| `Router` | 根据鉴别列识别 A/B/C 类行 |
| `Mapper` | 将 Excel 宽行拆成多个表的 RowPayload |
| `TopoSorter` | 根据 `parent` 关系排序多表写入顺序 |
| `SqlBuilder` | 构造 SQLite Upsert 和导出 SELECT |
| `ImportService` | 控制事务、校验、写入、回滚 |
| `ExportService` | 查询多表集合并组装成 Sheet 行 |

---

## 4. Public API 草案

```cpp
namespace dbridge {

struct ConnectionSpec {
    QString sqlitePath;
    int busyTimeoutMs = 5000;
    bool enableWal = true;
};

struct ImportOptions {
    QString profileName;
    QString sheetName;
    bool abortOnError = true; // MVP 固定 true
};

struct ExportOptions {
    QString profileName;
    QString sheetName;
};

struct RowError {
    QString sheet;
    int row = 0;          // Excel 1-based 行号
    QString column;       // 表头名；表级错误可为空
    QString rawValue;
    QString code;
    QString message;
};

struct ImportResult {
    bool ok = false;
    int readRows = 0;
    int writtenRows = 0;
    QList<RowError> errors;
};

struct ExportResult {
    bool ok = false;
    int writtenRows = 0;
    QList<RowError> errors;
};

class DataBridge {
public:
    bool open(const ConnectionSpec& spec, QString* err = nullptr);
    void close();

    bool loadProfile(const QString& jsonPath, QString* err = nullptr);
    bool loadProfileFromString(const QString& json, QString* err = nullptr);

    // 未知单表：运行期根据 SQLite 表结构生成简单 Profile 草稿
    QString generateAutoProfileJson(const QString& table, QString* err = nullptr);

    ImportResult importExcel(const QString& xlsxPath, const ImportOptions& options);
    ExportResult exportExcel(const QString& xlsxPath, const ExportOptions& options);
};

} // namespace dbridge
```

说明：

- MVP API 采用同步调用，宿主可自行放入 `QThread`，避免库内生命周期复杂化。
- 后续如需要进度、取消、异步 Job，可在长期演进版本中扩展。

---

## 5. Profile 设计

### 5.1 单表 Profile

适用于普通导入导出和未来未知表 auto profile。

```json
{
  "profileName": "customer_basic",
  "sheet": "Customers",
  "headerRow": 1,
  "mode": "singleTable",
  "table": "customer",
  "conflict": {
    "columns": ["customer_no"]
  },
  "columns": {
    "customer_no": {
      "source": "CustomerNo",
      "validators": ["notNull", "len<=32"]
    },
    "name": {
      "source": "Name",
      "validators": ["notNull", "len<=128"]
    },
    "phone": {
      "source": "Phone",
      "validators": ["regex:^[-0-9+ ]*$"]
    }
  },
  "export": {
    "orderBy": ["customer_no"]
  }
}
```

### 5.2 多表集合 Profile

适用于 A 行导入 m 集合。

```json
{
  "profileName": "order_m_set",
  "sheet": "Orders",
  "headerRow": 1,
  "mode": "multiTable",
  "routes": [
    {
      "table": "orders",
      "conflict": { "columns": ["order_no"] },
      "columns": {
        "order_no": { "source": "OrderNo", "validators": ["notNull"] },
        "customer": { "source": "Customer" },
        "amount": { "source": "Amount", "validators": ["decimal"] }
      }
    },
    {
      "table": "order_items",
      "parent": "orders",
      "fkInject": [{ "from": "orders", "pairs": [["order_no","order_no"]] }],
      "conflict": { "columns": ["order_no", "line_no"] },
      "columns": {
        "line_no": { "source": "LineNo", "validators": ["int>=1"] },
        "sku": { "source": "Sku", "validators": ["notNull"] },
        "qty": { "source": "Qty", "validators": ["int>=1"] }
      }
    }
  ],
  "export": {
    "orderBy": ["orders.order_no", "order_items.line_no"]
  }
}
```

### 5.3 A/B/C 混编 Profile

适用于同一个 Sheet 混杂多类行。

```json
{
  "profileName": "mixed_abc",
  "sheet": "Mixed",
  "headerRow": 1,
  "mode": "mixed",
  "discriminator": {
    "source": "Type"
  },
  "classes": [
    {
      "id": "A",
      "match": { "equals": "A" },
      "routes": [
        { "table": "m1", "conflict": { "columns": ["m_no"] }, "columns": {} },
        { "table": "m2", "parent": "m1", "fkInject": [{ "from": "m1", "pairs": [["m_no","m_no"]] }], "conflict": { "columns": ["m_no", "line_no"] }, "columns": {} }
      ]
    },
    {
      "id": "B",
      "match": { "equals": "B" },
      "routes": [
        { "table": "n1", "conflict": { "columns": ["n_no"] }, "columns": {} }
      ]
    },
    {
      "id": "C",
      "match": { "equals": "C" },
      "routes": [
        { "table": "o1", "conflict": { "columns": ["o_no"] }, "columns": {} }
      ]
    }
  ],
  "export": {
    "classColumn": "Type",
    "orderBy": ["sort_no"]
  }
}
```

Profile 约束：

- `conflict.columns` 必须匹配 SQLite 表上的 PRIMARY KEY 或 UNIQUE 约束。
- `fkInject` MVP 只支持业务键注入，例如 `order_no`；不依赖自增主键回填。
- 每个 `routes[]` 表示一个目标表。
- 多表写入顺序由 `parent` 自动排序。
- A/B/C 混编时，未匹配行视为校验错误并终止导入。

---

## 6. 导入流程

```
open sqlite
load profile
read xlsx header
read all rows or bounded chunk rows
validate header/profile/schema
validate every row
if any error:
    return errors, do not begin write
begin transaction
for each row:
    classify row if mixed
    map row to one or more RowPayload
    sort payloads by table dependency
    execute SQLite upsert per payload
if any DB error:
    rollback
    return error
commit
return success
```

核心约束：

- 导入前校验失败时，不开启写事务，不写任何数据。
- DB 执行阶段失败时，执行 `ROLLBACK`，保证本次导入零落盘。
- MVP 不支持部分成功。
- SQLite 连接和所有 `QSqlQuery` 在同一线程使用。

---

## 7. 校验规则

### 7.1 Profile 级校验

- Profile JSON 格式合法。
- `profileName` 唯一。
- `sheet`、`headerRow` 存在。
- 每个 route 的 `table` 存在。
- 每个 `columns` 目标列存在于对应表。
- 每个 `source` 表头存在于 Excel。
- `conflict.columns` 存在并匹配 PRIMARY KEY 或 UNIQUE。
- `parent` 指向的表存在且无环。
- `fkInject.from` 和 `fkInject.to` 列存在。

### 7.2 行级校验

MVP 内置校验器：

| 校验器 | 含义 |
|---|---|
| `notNull` | 非空 |
| `len<=N` | 字符串最大长度 |
| `len>=N` | 字符串最小长度 |
| `int` | 整数 |
| `int>=N` | 整数下限 |
| `decimal` | 十进制数 |
| `date:yyyy-MM-dd` | 日期格式 |
| `regex:<pattern>` | 正则匹配 |
| `enum:a,b,c` | 枚举值 |

### 7.3 关联校验

- 批内唯一性：同一批 Excel 中，不能出现相同 conflict key 的多行互相覆盖；MVP 默认视为错误。
- 外键存在性：如果子表引用数据库已有父表，导入前执行 SELECT 检查。
- 父子同批：如果父表和子表来自同一行或同一批，先检查父 payload 是否可生成子 payload 需要的业务键。

错误示例：

```json
{
  "sheet": "Mixed",
  "row": 12,
  "column": "Qty",
  "rawValue": "abc",
  "code": "E_VALIDATE_INT",
  "message": "Qty 必须是整数"
}
```

---

## 8. SQLite Upsert

生成 SQL：

```sql
INSERT INTO orders (order_no, customer, amount)
VALUES (?, ?, ?)
ON CONFLICT(order_no) DO UPDATE SET
  customer = excluded.customer,
  amount = excluded.amount;
```

规则：

- conflict 列不参与 update。
- 自增主键列如果未在 Profile 中声明，不写入。
- Profile 中声明的列才参与 INSERT/UPDATE，避免覆盖未映射的历史列。
- 所有值通过 bind 参数传入，禁止拼接用户数据。

---

## 9. 导出流程

### 9.1 单表导出

```
load profile
select mapped columns from table
write header
write rows
save xlsx
```

### 9.2 多表集合导出

MVP 支持两种方式：

1. Profile 显式声明 `export.sql`，由业务方提供 JOIN 查询。
2. Profile 未声明 `export.sql` 时，按 `routes[]` 的 parent/fkInject 生成简单 LEFT JOIN。

示例：

```json
{
  "export": {
    "sql": "SELECT o.order_no AS OrderNo, o.customer AS Customer, i.line_no AS LineNo, i.sku AS Sku FROM orders o LEFT JOIN order_items i ON i.order_no = o.order_no ORDER BY o.order_no, i.line_no"
  }
}
```

导出规则：

- 写出同一个 Sheet。
- 表头使用 Profile 中的 `source` 名称。
- A/B/C 混编导出时，输出鉴别列，例如 `Type=A/B/C`。
- 多表一对多时，MVP 采用 `expandRows`：父表字段在多行中重复，子表字段逐行展开。

---

## 10. 未来未知表支持

MVP 将“未知表”拆成两类：

| 场景 | MVP 支持方式 |
|---|---|
| 未知单表 | 自动自省生成 Profile，表头名等于列名 |
| 未知多表 / A/B/C 混编 | 不改 C++ 代码；通过新增 JSON Profile 描述 |

单表 auto profile 条件：

- SQLite 表已存在。
- 表有 PRIMARY KEY 或 UNIQUE 约束。
- Excel 表头名与表列名一致，或者用户接受自动生成的列名表头。
- 生成列、自增主键列默认不从 Excel 写入。

自省 SQL：

```sql
SELECT name FROM sqlite_master WHERE type='table';
PRAGMA table_info('<table>');
PRAGMA index_list('<table>');
PRAGMA index_info('<index>');
PRAGMA foreign_key_list('<table>');
```

---

## 11. 错误码

| 错误码 | 含义 |
|---|---|
| `E_OPEN_DB` | 打开 SQLite 失败 |
| `E_OPEN_XLSX` | 打开 Excel 失败 |
| `E_PROFILE_PARSE` | Profile JSON 解析失败 |
| `E_PROFILE_TABLE_NOT_FOUND` | Profile 指定表不存在 |
| `E_PROFILE_COLUMN_NOT_FOUND` | Profile 指定列不存在 |
| `E_PROFILE_NO_CONFLICT_KEY` | 未配置可用 Upsert 键 |
| `E_PROFILE_TOPOLOGY_CYCLE` | 多表依赖成环 |
| `E_HEADER_NOT_FOUND` | Excel 表头缺失 |
| `E_ROUTE_UNMATCHED` | 混编行未匹配任何 class |
| `E_VALIDATE_NULL` | 必填为空 |
| `E_VALIDATE_TYPE` | 类型错误 |
| `E_VALIDATE_REGEX` | 正则不匹配 |
| `E_VALIDATE_DUPLICATE` | 批内唯一键重复 |
| `E_VALIDATE_FK` | 外键引用不存在 |
| `E_DB_UPSERT` | SQLite 写入失败 |
| `E_EXPORT_QUERY` | 导出查询失败 |
| `E_WRITE_XLSX` | 写 Excel 失败 |

---

## 12. 建议目录结构

```
dbridge/
├── include/dbridge/
│   ├── DataBridge.h
│   ├── Types.h
│   └── Errors.h
├── src/
│   ├── DataBridge.cpp
│   ├── profile/
│   ├── schema/
│   ├── excel/
│   ├── validation/
│   ├── mapping/
│   ├── sql/
│   └── service/
├── 3rdparty/QXlsx/
├── tests/
│   ├── unit/
│   ├── integration/
│   └── data/
├── examples/
└── CMakeLists.txt
```

---

## 13. 测试策略

MVP 必测：

| 类别 | 用例 |
|---|---|
| Upsert | 新增插入、已存在更新、不覆盖未映射列 |
| 事务 | 导入中任意错误整体回滚 |
| Profile | 缺表、缺列、无唯一键、parent 成环 |
| 校验 | 非空、类型、长度、正则、枚举、批内重复 |
| 单表 | Excel -> SQLite -> Excel |
| 多表 m 集合 | A 行拆入 m1/m2，再导出回 Sheet |
| 混编 A/B/C | 同 Sheet 混杂导入 m/n/o，再导出回 Sheet |
| 未知单表 | 运行期新建表，auto profile 导入导出 |
| 错误定位 | 错误返回 Sheet、行号、列名、原值 |

---

## 14. 开发阶段

### 阶段 1：基础工程与单表 Upsert

- 建立库工程。
- 集成 QtSql 和 QXlsx。
- SQLite open/close。
- 单表 Profile。
- 单表导入 Upsert。
- AllOrNothing 事务。

交付：单表 Excel 导入 SQLite 可用。

### 阶段 2：校验与错误告警

- Profile 级校验。
- 行级校验器。
- 错误列表结构。
- 失败不落库。

交付：导入前发现错误并返回精确错误清单。

### 阶段 3：未知单表自省

- SQLite 表结构读取。
- 自动生成单表 Profile。
- 未知表导入导出。

交付：运行期新增简单表后无需改代码即可导入导出。

### 阶段 4：多表集合

- `routes[]` 支持。
- `parent` 拓扑排序。
- `fkInject` 业务键注入。
- m 集合导入导出。

交付：A 行可拆入多表并导出还原。

### 阶段 5：A/B/C 混编

- `discriminator`。
- `classes[]`。
- A/B/C 分别路由到 m/n/o。
- 混编导出。

交付：必须业务目标全部闭环。

---

## 15. 风险与约束

| 风险 | MVP 处理 |
|---|---|
| Excel 文件过大导致内存高 | 首版不承诺百万行；超过阈值提示用户拆分文件 |
| 多表关系过复杂 | MVP 只支持业务键 FK 注入，不做自增 ID 回填 |
| 未知表没有唯一键 | 无法 Upsert，返回 `E_PROFILE_NO_CONFLICT_KEY` |
| A/B/C 匹配规则冲突 | MVP 使用 `equals` 精确匹配，冲突视为 Profile 错误 |
| 导出 JOIN 复杂 | 允许业务在 Profile 中显式提供 `export.sql` |
| QXlsx 类型推断差异 | 以 Profile validator 为准，错误则拒绝导入 |

---

## 16. 验收标准

MVP 完成的判断标准：

- 能导入单表 Excel，新增行插入，已有行更新。
- 任意校验错误会终止导入，数据库无部分写入。
- 能对运行期新增的简单 SQLite 表生成 Profile 并导入导出。
- 能把 A 行拆分写入 m 集合多表。
- 能把混杂 A/B/C 行分别写入 m/n/o 集合多表。
- 能从 m/n/o 集合导出到同一个 Sheet，并带回 A/B/C 鉴别列。
- 全部核心场景有自动化测试或可重复示例。
