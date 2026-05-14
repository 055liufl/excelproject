# Qt + SQLite + Excel 批量导入导出 实现文档

> 配套设计文档：`specs/MVP-Qt-SQLite-Excel-批量导入导出-实现设计.md`。
> 本文只回答 **How**：具体的工程结构、头文件、内部数据结构、关键算法、SQL 模板、模块时序与阶段化任务。
> 范围严格对齐 MVP 设计第 1 节的五类业务目标和第 4 节的同步 Public API，不引入异步 Job、ChunkCommit、ContinueOnError、流式 xlsx、跨数据库 SPI、代理主键 FK 回填、C ABI 等任何长期演进内容。

---

## 0. 阅读导览

| 想知道什么 | 看哪一节 |
|---|---|
| MVP 必须实现的 5 件事如何分到模块 | §1 实现目标对照矩阵 |
| 怎么搭工程、用哪几个库 | §2 工程基线 |
| 头文件长什么样 | §3 Public 头实现 |
| 内部数据结构 | §4 内部数据结构 |
| 单个模块怎么写 | §5 模块实现详述 |
| 关键 SQL/算法长什么样 | §6 关键算法与模板 |
| 一次 importExcel / exportExcel 内部发生什么 | §7 端到端时序 |
| 出错怎么收拢、事务怎么开 | §8 事务与错误处理 |
| 5 个业务目标分别落在哪几条代码路径 | §9 业务目标实现路径 |
| 按阶段交付怎么排任务 | §10 阶段化任务清单 |
| 测试怎么写、数据怎么准备 | §11 测试实现 |
| 完工以什么为准 | §12 验收 Checklist |

---

## 1. 实现目标对照矩阵

MVP 设计第 1 节列出五个硬目标，实现文档需要把它们映射到模块、关键代码路径、测试用例：

| 业务目标 | 主要实现模块 | 关键代码路径 | 验收测试 |
|---|---|---|---|
| 1. Upsert 导入（INSERT/UPDATE 不用 REPLACE） | `SqlBuilder` + `ImportService` | §6.1 SQLite Upsert 模板 | §11.3 `upsert_new`、`upsert_update`、`upsert_no_unmapped_overwrite` |
| 2. 导入前校验、错误终止 | `ProfileValidator` + `ValidatorChain` + `ErrorCollector` + `ImportService` | §7.1 时序的 "Phase B 校验" 段 | §11.3 `validate_required`、`validate_type`、`validate_regex`、`validate_dup_in_batch` |
| 3. 未来未知单表 + 未来复杂表用 JSON Profile | `SchemaIntrospector` + `AutoProfileBuilder` + `ProfileLoader` | §6.8 自省 + 自动 Profile 生成 | §11.3 `auto_profile_single`、`auto_profile_no_unique_key` |
| 4. 单类行（A 行）拆入多表 m 集合 | `Mapper`（多 route）+ `TopoSorter` + `FkInjector` | §6.5 业务键 FK 注入 + §6.3 拓扑排序 | §11.3 `multi_table_split_and_export` |
| 5. 同 Sheet 混编 A/B/C 分发到 m/n/o 多表集合 | `Router`（鉴别列）+ `Mapper`（按 class 选 routes） | §6.6 Discriminator 匹配 + §6.7 expandRows | §11.3 `mixed_abc_roundtrip` |

只要这五行测试全部绿，MVP 即视为完成。

---

## 2. 工程基线

### 2.1 工具链

| 项 | 值 |
|---|---|
| C++ 标准 | C++17 |
| Qt | 5.12.12（仅 `QtCore` / `QtSql`） |
| 第三方 | QXlsx（vendored 进 `3rdparty/QXlsx/`） |
| 数据库 | 仅 SQLite 3.24+（支持 `ON CONFLICT ... DO UPDATE`） |
| 构建 | CMake ≥ 3.16 |
| 编译器 | MSVC 2017+ / GCC 9+ / Clang 10+（同工程同工具链链接，不承诺跨 ABI） |
| 测试 | Qt Test |
| 静态检查 | clang-format、clang-tidy（已经在 `.pre-commit-config.yaml` 中开启） |

### 2.2 依赖原则

- 不引入除 QXlsx 之外的第三方库；JSON 用 `QJsonDocument`，正则用 `QRegularExpression`，日期用 `QDate`。
- QXlsx 作为源码 vendor，统一在 `3rdparty/QXlsx/CMakeLists.txt` 编译为静态库。
- SQLite 直接使用 `QSQLITE` 驱动（Qt 自带，无需链接 sqlite3 源码）。

### 2.3 目录与命名空间

目录与 MVP 设计第 12 节保持一致：

```
dbridge/
├── CMakeLists.txt
├── include/dbridge/
│   ├── DataBridge.h        // 唯一公开头
│   ├── Types.h             // 公开结构体（ConnectionSpec/Options/RowError/Result）
│   └── Errors.h            // 错误码字符串常量
├── src/
│   ├── DataBridge.cpp
│   ├── DataBridgePrivate.h // PImpl 内部
│   ├── profile/            // ProfileSpec / Loader / Validator / AutoBuilder
│   ├── schema/             // SchemaIntrospector / SchemaCatalog
│   ├── excel/              // ExcelReader / ExcelWriter
│   ├── validation/         // ValidatorChain + 内置 Validator
│   ├── mapping/            // Router / Mapper / TopoSorter / FkInjector
│   ├── sql/                // SqlBuilder
│   └── service/            // ImportService / ExportService
├── 3rdparty/QXlsx/
├── tests/{unit,integration,data}/
├── examples/cli/main.cpp
└── README.md
```

C++ 命名空间统一 `namespace dbridge`，私有实现进 `namespace dbridge::detail`。

### 2.4 CMake 工程骨架

`dbridge/CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.16)
project(dbridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt5 5.12 REQUIRED COMPONENTS Core Sql Test)
add_subdirectory(3rdparty/QXlsx)

add_library(dbridge
  src/DataBridge.cpp
  src/profile/ProfileLoader.cpp
  src/profile/ProfileValidator.cpp
  src/profile/AutoProfileBuilder.cpp
  src/schema/SchemaIntrospector.cpp
  src/excel/ExcelReader.cpp
  src/excel/ExcelWriter.cpp
  src/validation/ValidatorChain.cpp
  src/validation/Validators.cpp
  src/mapping/Router.cpp
  src/mapping/Mapper.cpp
  src/mapping/TopoSorter.cpp
  src/mapping/FkInjector.cpp
  src/sql/SqlBuilder.cpp
  src/service/ImportService.cpp
  src/service/ExportService.cpp
)

target_include_directories(dbridge
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(dbridge PUBLIC Qt5::Core Qt5::Sql PRIVATE QXlsx)

enable_testing()
add_subdirectory(tests)
```

输出形态：静态库或动态库由 `BUILD_SHARED_LIBS` 决定，宿主侧链接 `dbridge`。

### 2.5 编译选项

```cmake
target_compile_options(dbridge PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>)
```

`#include` 顺序：自身头 → 项目头 → Qt 头 → 标准库；用 clang-format 强制。

---

## 3. Public 头实现

### 3.1 `include/dbridge/Types.h`

```cpp
#pragma once
#include <QList>
#include <QString>

namespace dbridge {

struct ConnectionSpec {
    QString sqlitePath;
    int     busyTimeoutMs = 5000;
    bool    enableWal     = true;
};

struct ImportOptions {
    QString profileName;
    QString sheetName;       // 空则用 Profile 内 sheet
    bool    abortOnError = true;   // MVP 必须为 true
};

struct ExportOptions {
    QString profileName;
    QString sheetName;       // 空则用 Profile 内 sheet
};

struct RowError {
    QString sheet;
    int     row = 0;         // Excel 1-based 行号；表级错误为 0
    QString column;          // 表头名；表级错误为空
    QString rawValue;
    QString code;            // 见 Errors.h
    QString message;
};

struct ImportResult {
    bool          ok           = false;
    int           readRows     = 0;
    int           writtenRows  = 0;
    QList<RowError> errors;
};

struct ExportResult {
    bool          ok          = false;
    int           writtenRows = 0;
    QList<RowError> errors;
};

} // namespace dbridge
```

### 3.2 `include/dbridge/Errors.h`

将 MVP 设计第 11 节的错误码作为字符串常量集中暴露，避免散写 magic string：

```cpp
#pragma once
namespace dbridge::err {
inline constexpr const char* E_OPEN_DB                  = "E_OPEN_DB";
inline constexpr const char* E_OPEN_XLSX                = "E_OPEN_XLSX";
inline constexpr const char* E_PROFILE_PARSE            = "E_PROFILE_PARSE";
inline constexpr const char* E_PROFILE_TABLE_NOT_FOUND  = "E_PROFILE_TABLE_NOT_FOUND";
inline constexpr const char* E_PROFILE_COLUMN_NOT_FOUND = "E_PROFILE_COLUMN_NOT_FOUND";
inline constexpr const char* E_PROFILE_NO_CONFLICT_KEY  = "E_PROFILE_NO_CONFLICT_KEY";
inline constexpr const char* E_PROFILE_TOPOLOGY_CYCLE   = "E_PROFILE_TOPOLOGY_CYCLE";
inline constexpr const char* E_HEADER_NOT_FOUND         = "E_HEADER_NOT_FOUND";
inline constexpr const char* E_ROUTE_UNMATCHED          = "E_ROUTE_UNMATCHED";
inline constexpr const char* E_VALIDATE_NULL            = "E_VALIDATE_NULL";
inline constexpr const char* E_VALIDATE_TYPE            = "E_VALIDATE_TYPE";
inline constexpr const char* E_VALIDATE_REGEX           = "E_VALIDATE_REGEX";
inline constexpr const char* E_VALIDATE_DUPLICATE       = "E_VALIDATE_DUPLICATE";
inline constexpr const char* E_VALIDATE_FK              = "E_VALIDATE_FK";
inline constexpr const char* E_DB_UPSERT                = "E_DB_UPSERT";
inline constexpr const char* E_EXPORT_QUERY             = "E_EXPORT_QUERY";
inline constexpr const char* E_WRITE_XLSX               = "E_WRITE_XLSX";
} // namespace dbridge::err
```

### 3.3 `include/dbridge/DataBridge.h`（PImpl）

```cpp
#pragma once
#include "Types.h"
#include <memory>

namespace dbridge {

class DataBridgePrivate;

class DataBridge {
public:
    DataBridge();
    ~DataBridge();
    DataBridge(const DataBridge&)            = delete;
    DataBridge& operator=(const DataBridge&) = delete;

    bool open(const ConnectionSpec& spec, QString* err = nullptr);
    void close();

    bool loadProfile(const QString& jsonPath, QString* err = nullptr);
    bool loadProfileFromString(const QString& json, QString* err = nullptr);

    QString generateAutoProfileJson(const QString& table, QString* err = nullptr);

    ImportResult importExcel(const QString& xlsxPath, const ImportOptions& options);
    ExportResult exportExcel(const QString& xlsxPath, const ExportOptions& options);

private:
    std::unique_ptr<DataBridgePrivate> d_;
};

} // namespace dbridge
```

PImpl 把 `QSqlDatabase`、`SchemaCatalog`、`QHash<QString, ProfileSpec>` 等私有依赖完全藏在 `.cpp`，公开头零 Qt-Sql/QXlsx 依赖。宿主只需要链接 `dbridge` 并引一个头。

---

## 4. 内部数据结构

> 文件位置：`src/profile/ProfileSpec.h` / `src/schema/SchemaCatalog.h` / `src/mapping/RowPayload.h`。下面给出最小可工作字段集合，命名与 Profile JSON 对齐，便于调试。

### 4.1 ProfileSpec

```cpp
enum class ProfileMode { SingleTable, MultiTable, Mixed };

struct ColumnSpec {
    QString     dbColumn;          // SQLite 列名
    QString     source;            // Excel 表头名（MVP 唯一支持）
    QStringList validatorTokens;   // 原始 token，如 "len<=32"、"regex:^...$"
};

struct ConflictSpec {
    QStringList columns;           // 对应 PRIMARY KEY 或 UNIQUE
};

struct FkInjectSpec {
    QString fromTable;             // "orders"
    QString fromColumn;            // "order_no"
    QString toTable;               // "order_items"
    QString toColumn;              // "order_no"
};

struct RouteSpec {
    QString                       table;
    QString                       parent;          // 空=根
    ConflictSpec                  conflict;
    std::optional<FkInjectSpec>   fkInject;
    QVector<ColumnSpec>           columns;
};

struct ClassSpec {
    QString             id;             // "A" / "B" / "C"
    QString             matchEquals;    // MVP 仅 equals
    QVector<RouteSpec>  routes;
};

struct ExportSpec {
    QStringList orderBy;
    QString     explicitSql;     // 非空 => 直接执行
    QString     classColumn;     // 混编导出时写入哪个表头
};

struct ProfileSpec {
    QString             name;
    QString             sheet;
    int                 headerRow = 1;
    ProfileMode         mode      = ProfileMode::SingleTable;
    QVector<RouteSpec>  routes;            // SingleTable/MultiTable
    QString             discriminatorSource;
    QVector<ClassSpec>  classes;           // Mixed
    ExportSpec          exportSpec;
};
```

### 4.2 SchemaCatalog（SchemaIntrospector 输出）

```cpp
struct ColumnInfo {
    QString  name;
    QString  declaredType;
    bool     notNull       = false;
    bool     primaryKey    = false;
    int      pkOrder       = 0;      // 复合主键顺序
    QString  defaultValue;
    bool     autoIncrement = false;  // sqlite_sequence 或 INTEGER PRIMARY KEY AUTOINCREMENT
    bool     generated     = false;  // PRAGMA table_xinfo hidden
};

struct IndexInfo {
    QString     name;
    bool        unique = false;
    QStringList columns;
};

struct FkInfo {
    QString refTable;
    QString fromColumn;
    QString toColumn;
};

struct TableInfo {
    QString             name;
    QVector<ColumnInfo> columns;
    QVector<IndexInfo>  indexes;
    QVector<FkInfo>     foreignKeys;
};

class SchemaCatalog {
public:
    bool                hasTable(const QString& name) const;
    const TableInfo*    table(const QString& name) const;
    QStringList         allTables() const;
private:
    QHash<QString, TableInfo> tables_;
};
```

### 4.3 RowPayload（Mapper 输出）

一行 Excel 经过 Mapper 拆分后变成一组 `RoutePayload`：

```cpp
struct RoutePayload {
    QString                       table;        // 与 RouteSpec.table 对应
    QString                       routeKey;     // route 在 Profile 中的唯一键（如 "orders" 或 "A:orders"），用于 FK 注入查找
    QVector<QString>              dbColumns;    // 与 binds 顺序一一对应
    QVector<QVariant>             binds;
    QStringList                   conflictKey;  // 实际 conflict 列名
    QVector<QVariant>             conflictVals; // 用于批内重复检测、FK 注入
};

struct RowContext {
    int                  excelRow = 0;
    QString              classId;       // 非 mixed 为空
    QVector<RoutePayload> payloads;     // 已按 topo 顺序排列
};
```

### 4.4 ErrorCollector

```cpp
class ErrorCollector {
public:
    void add(const QString& sheet, int row, const QString& column,
             const QString& raw, const QString& code, const QString& msg);
    void addTable(const QString& sheet, const QString& code, const QString& msg);
    bool empty() const { return errors_.isEmpty(); }
    QList<RowError>& list() { return errors_; }
private:
    QList<RowError> errors_;
};
```

整个导入过程只用一个 ErrorCollector；任意阶段失败即返回 `errors_` 给宿主。

---

## 5. 模块实现详述

### 5.1 ProfileLoader

文件：`src/profile/ProfileLoader.{h,cpp}`。

职责：把 JSON（文件或字符串）解析成 `ProfileSpec`，**仅做结构性解析**，不查 DB、不查 Excel。

骨架：

```cpp
class ProfileLoader {
public:
    bool load(const QJsonDocument& doc, ProfileSpec* out, QString* err);
private:
    bool readSingleTable(const QJsonObject& o, ProfileSpec* out, QString* err);
    bool readMultiTable (const QJsonObject& o, ProfileSpec* out, QString* err);
    bool readMixed      (const QJsonObject& o, ProfileSpec* out, QString* err);
    bool readRoute      (const QJsonObject& o, RouteSpec* out, QString* err);
    bool readColumn     (const QString& dbCol, const QJsonObject& o, ColumnSpec* out, QString* err);
};
```

规则：

- 未知字段直接忽略（MVP 暂不做 schema 严格化，避免与 Profile 草稿写法冲突）。
- `mode` 必填，三选一；未知 mode → `E_PROFILE_PARSE`。
- `routes` / `classes` 至少一项；`columns` 可以为空（表示 fkInject 全自动）。
- `validators` 数组按顺序保留为字符串 token，不做语义解析；语义解析在 `ValidatorChain` 内做（5.7）。

### 5.2 SchemaIntrospector

文件：`src/schema/SchemaIntrospector.{h,cpp}`。

职责：对一个已经 `QSqlDatabase::open()` 的连接，遍历所有表填 `SchemaCatalog`。在 `DataBridge::open` 成功后立即扫描一次；后续 Profile 与导入导出都查这份内存目录，**不再每次 PRAGMA**。

骨架：

```cpp
class SchemaIntrospector {
public:
    bool load(QSqlDatabase& db, SchemaCatalog* out, QString* err);
private:
    bool readTables (QSqlDatabase&, QStringList* tables, QString*);
    bool readColumns(QSqlDatabase&, TableInfo*, QString*);
    bool readIndexes(QSqlDatabase&, TableInfo*, QString*);
    bool readForeignKeys(QSqlDatabase&, TableInfo*, QString*);
};
```

使用的 SQL（同 MVP 设计第 10 节）：

```sql
SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%';
PRAGMA table_xinfo('<table>');     -- 比 table_info 多 hidden 列
PRAGMA index_list('<table>');
PRAGMA index_info('<index>');
PRAGMA foreign_key_list('<table>');
```

`autoIncrement` 判断规则：

1. 列类型大小写不敏感等于 `INTEGER`。
2. 是主键且复合主键阶数为 1。
3. `sqlite_master.sql` 包含 `AUTOINCREMENT`（对应表名出现于 `sqlite_sequence`）。
   三条同时满足则置 `autoIncrement = true`。否则只是 ROWID alias，仍然可以不写入但不强制视为 auto。

### 5.3 AutoProfileBuilder

文件：`src/profile/AutoProfileBuilder.{h,cpp}`。

职责：实现 MVP 设计 §10 的"未知单表" 自动 Profile 草稿生成（`DataBridge::generateAutoProfileJson`）。

算法：

```text
input  : tableName, SchemaCatalog
output : ProfileSpec(JSON string)

1. lookup TableInfo; 不存在 -> err: E_PROFILE_TABLE_NOT_FOUND
2. 选 conflict columns（按优先级）：
     a. autoIncrement = false 的复合主键 -> 全部主键列
     b. autoIncrement = true 的 ROWID 主键 -> 跳过该列，转 (c)
     c. 最小列数的 UNIQUE 索引
   全失败 -> err: E_PROFILE_NO_CONFLICT_KEY
3. 选可写列：
     - 跳过 autoIncrement 列
     - 跳过 generated 列
     - 其余列全部纳入
4. 生成 ColumnSpec：
     - dbColumn = 列名
     - source   = 列名 (业务可在 Excel 表头使用同名)
     - validators：
         * notNull 列追加 "notNull"
         * declaredType 含 INT  -> "int"
         * declaredType 含 REAL/NUMERIC/DECIMAL -> "decimal"
         * declaredType 含 DATE -> "date:yyyy-MM-dd"
5. mode=singleTable, sheet=表名, headerRow=1
```

返回的是 JSON 字符串，宿主可以打印、保存、手工编辑后再 `loadProfileFromString`。

### 5.4 ProfileValidator

文件：`src/profile/ProfileValidator.{h,cpp}`。

职责：把 `ProfileSpec` 与 `SchemaCatalog`、Excel 表头三方对账，覆盖 MVP §7.1 全部条目。

校验项（顺序执行，失败立刻 collect 不立刻 return，保证一次反馈尽可能多错误）：

1. `name` 非空、`sheet` 非空、`headerRow ≥ 1`。
2. 对每个 RouteSpec：
   - `table` 在 catalog 中存在 → 否则 `E_PROFILE_TABLE_NOT_FOUND`。
   - `conflict.columns` 非空 → 否则 `E_PROFILE_NO_CONFLICT_KEY`。
   - 每个 conflict 列存在于 `TableInfo.columns` 且匹配 PRIMARY KEY 或 某个 UNIQUE 索引集合 → 否则 `E_PROFILE_NO_CONFLICT_KEY`。
   - 每个 ColumnSpec.dbColumn 存在 → 否则 `E_PROFILE_COLUMN_NOT_FOUND`。
   - 若 `fkInject` 存在：`from/to` 表与列都存在；`fromTable == parent`。
3. 拓扑：`parent` 指向的 route 必须存在；运行 §6.3 Kahn 算法检测环 → `E_PROFILE_TOPOLOGY_CYCLE`。
4. 表头校验（需 ExcelReader 已经把表头读出）：每个 ColumnSpec.source 在表头出现 → 否则 `E_HEADER_NOT_FOUND`。Mixed 模式 `discriminatorSource` 同样校验。

### 5.5 ExcelReader

文件：`src/excel/ExcelReader.{h,cpp}`，包装 QXlsx。

```cpp
class ExcelReader {
public:
    bool open(const QString& xlsxPath, QString* err);
    bool selectSheet(const QString& name, QString* err);

    // 读取 headerRow 那一行，trim 后建立 header->column 索引
    bool readHeader(int headerRow, QString* err);

    int  firstDataRow() const { return headerRow_ + 1; }
    int  lastRow() const;

    // 按 source 表头取值；缺列返回 QVariant()
    QVariant cellBySource(int row, const QString& source) const;

    QStringList headers() const { return headers_; }
    int columnOfSource(const QString& source) const;

private:
    QXlsx::Document        doc_;
    QXlsx::AbstractSheet*  sheet_ = nullptr;
    int                    headerRow_ = 1;
    QStringList            headers_;
    QHash<QString,int>     sourceToCol_;
};
```

读取约定：

- `cellBySource` 返回的 `QVariant` 保留 QXlsx 原始类型（Double / QString / QDate / QDateTime / Bool），后续 `ValidatorChain` 负责类型解释。
- 空单元格统一返回 `QVariant()`（isNull==true）。
- 表头读取前 `QString::trimmed()`，避免 Excel 末尾空格。

### 5.6 ExcelWriter

```cpp
class ExcelWriter {
public:
    bool open(const QString& xlsxPath, const QString& sheetName, QString* err);
    void writeHeader(const QStringList& headers);
    void writeRow   (const QVector<QVariant>& row);
    bool save(QString* err);
private:
    QXlsx::Document doc_;
    QString         sheetName_;
    int             rowCursor_ = 1;
};
```

写入约定：

- header 写在 row 1，从 row 2 开始写数据。
- `QVariant()` 写为空单元格。
- `save()` 完成后自动关文件，宿主可重复构造写多个文件。

### 5.7 ValidatorChain

文件：`src/validation/ValidatorChain.{h,cpp}` + `Validators.{h,cpp}`。

每个 validator 是一个 `std::function<bool(const QVariant&, QString* errCode, QString* msg)>`。`ValidatorChain` 把一个 token 数组在 Profile 加载完成后**一次编译成函数列表**，后续按行执行不再 parse 字符串。

支持 token：

| Token | 实现 |
|---|---|
| `notNull` | `v.isNull() || v.toString().isEmpty()` → `E_VALIDATE_NULL` |
| `len<=N` | 字符串长度 ≤ N，否则 `E_VALIDATE_TYPE` |
| `len>=N` | 同上反向 |
| `int`    | `v.toLongLong(ok)`, `ok==false` → `E_VALIDATE_TYPE` |
| `int>=N` | 同 `int` 再加下限 |
| `decimal`| `v.toDouble(ok)` 或 `QString::toDouble(ok)` |
| `date:yyyy-MM-dd` | `QDate::fromString(v.toString(), fmt).isValid()`；若原本是 `QDate/QDateTime` 直接通过 |
| `regex:<pattern>` | 预编译 `QRegularExpression`，对 `v.toString()` 全匹配 → `E_VALIDATE_REGEX` |
| `enum:a,b,c` | 预编译为 `QSet<QString>`，成员判定 |

`notNull` 不在 token 列表里时：null 值跳过其他校验（与 SQL 语义一致：NULL 不参与类型校验）。

`ValidatorChain` 同时负责类型规范化：
- `int` / `int>=N` 成功 → 返回 `qlonglong`
- `decimal` 成功 → `double`
- `date:fmt` 成功 → `QDate`
- 其余 → 原值

规范化结果会成为最终 bind 给 `QSqlQuery` 的值。

### 5.8 Router

文件：`src/mapping/Router.{h,cpp}`。

非混编模式：直接返回 `ProfileSpec.routes`。

混编模式：

```cpp
class Router {
public:
    bool init(const ProfileSpec& p, QString* err);            // 校验各 class.matchEquals 唯一
    const ClassSpec* match(const QVariant& discriminator) const;
    QString discriminatorSource() const;
};
```

匹配规则严格 `equals`（字符串比较），MVP 不引入正则、不引入多列。`init` 阶段把 `matchEquals` 集中检查唯一，重复 → `E_PROFILE_PARSE`。运行时未匹配 → `E_ROUTE_UNMATCHED`。

### 5.9 Mapper

文件：`src/mapping/Mapper.{h,cpp}`。

输入：`RouteSpec` 列表 + 当前 Excel 行已读取的源值 → 输出 `QVector<RoutePayload>`。

伪代码：

```text
for each route in routes_in_topo_order:
    payload = RoutePayload{table=route.table, routeKey=route_key(route)}
    for each ColumnSpec col in route.columns:
        v_raw = excelReader.cellBySource(row, col.source)
        (ok, v_normalized) = validatorChain[route, col].run(v_raw)
        if !ok: errorCollector.add(...); continue
        payload.dbColumns.append(col.dbColumn)
        payload.binds.append(v_normalized)
    payload.conflictKey  = route.conflict.columns
    payload.conflictVals = pick from payload by name (校验完整性见 §6.4)
    out.append(payload)
```

`route_key` 构造规则：
- 非混编：`route.table`
- 混编：`<class.id>:<route.table>`
- 同一 class 内多表禁止重名 → ProfileValidator 校验。

### 5.10 TopoSorter

文件：`src/mapping/TopoSorter.{h,cpp}`。

Kahn 算法见 §6.3。输入是 `QVector<RouteSpec>`，输出是排序后的拷贝；环路检测交由 ProfileValidator 触发，TopoSorter 自身只在 Profile 加载阶段调用一次，缓存结果。

### 5.11 SqlBuilder

文件：`src/sql/SqlBuilder.{h,cpp}`。

接口：

```cpp
struct UpsertSql {
    QString sql;
    QVector<QString> bindOrder;   // 与占位符一一对应（用于 named bind 备用，当前仍走 positional）
};

class SqlBuilder {
public:
    UpsertSql buildUpsert(const RoutePayload& p);
    QString   buildAutoJoinSelect(const QVector<RouteSpec>& routes,
                                  const ExportSpec& exp);
};
```

`buildUpsert`：

```text
INSERT INTO <table> (col1, col2, ..., colN) VALUES (?, ?, ..., ?)
ON CONFLICT(<conflict.columns>) DO UPDATE SET
  <updCol1> = excluded.<updCol1>,
  <updCol2> = excluded.<updCol2>;
```

其中 `updColX = dbColumns \ conflict.columns`；若集合为空 → 把 `DO UPDATE SET ...` 替换为 `DO NOTHING`（满足"已存在则不动" 的极端情形，避免空 SET 语法错误）。

`buildAutoJoinSelect`：见 §6.2。

所有值通过 `QSqlQuery::addBindValue` 传入，禁止字符串拼接用户数据。

### 5.12 ImportService

文件：`src/service/ImportService.{h,cpp}`。`DataBridge::importExcel` 直接转调它。

`ImportService::run(profile, xlsxPath, options) -> ImportResult`：见 §7.1 时序。

关键守门：

- `abortOnError == false` → 直接返回 `E_PROFILE_PARSE` 类错误（MVP 不接受该值），避免误用。
- 所有 `QSqlQuery` 创建和执行都在同一线程（即调用 importExcel 的线程）。
- 写阶段使用一个 `QSqlQuery` 反复 `prepare`（同 SQL 只 prepare 一次）+ `bind` + `exec`。

### 5.13 ExportService

文件：`src/service/ExportService.{h,cpp}`。`DataBridge::exportExcel` 直接转调。

行为见 §7.2 时序。注意：

- SingleTable / MultiTable：执行单条 SQL（`export.explicitSql` 优先；否则自动生成）→ 按返回列顺序写表头与数据。
- Mixed：对每个 `ClassSpec` 单独执行查询；为每行追加 `classColumn = class.id`；最后按 `orderBy` 在内存排序（MVP 数据规模可控）。

---

## 6. 关键算法与模板

### 6.1 SQLite Upsert 模板

生成示例（对应 MVP §8）：

输入：
```
table = "orders"
columns = [order_no, customer, amount]
conflict = [order_no]
```

输出：
```sql
INSERT INTO orders (order_no, customer, amount)
VALUES (?, ?, ?)
ON CONFLICT(order_no) DO UPDATE SET
  customer = excluded.customer,
  amount   = excluded.amount;
```

实现要点：

- 所有标识符在 Profile 校验阶段就已经核对过存在性，这里直接拼接（不做 `[]` 引号转义，假定列名合法标识符；非法标识符在 Profile 加载阶段拒绝）。
- 列顺序以 ColumnSpec 在 Profile 中出现的顺序为准，保证可重现 SQL。
- update_cols 为空时使用 `ON CONFLICT(...) DO NOTHING`。

### 6.2 多表自动 LEFT JOIN 导出 SQL

输入：拓扑排序后的 routes，第一项为根。

输出（以两表为例）：

```sql
SELECT
  orders.order_no   AS OrderNo,
  orders.customer   AS Customer,
  orders.amount     AS Amount,
  order_items.line_no AS LineNo,
  order_items.sku     AS Sku,
  order_items.qty     AS Qty
FROM orders
LEFT JOIN order_items
  ON order_items.order_no = orders.order_no
ORDER BY orders.order_no, order_items.line_no;
```

构造步骤：

1. SELECT 列：遍历每个 route 的 ColumnSpec，输出 `<route.table>.<dbColumn> AS <source>`。
2. FROM：根 route 的 table。
3. 每个非根 route：`LEFT JOIN <child.table> ON <fkInject.to>=<fkInject.from>`，用 fkInject 的限定名 `<table>.<column>`。
4. ORDER BY：来自 `export.orderBy`，每项支持 `table.column` 或 `column`（无表前缀 → 默认根表）。

`export.explicitSql` 存在时直接执行该 SQL，列别名（AS）必须与 Profile 中 ColumnSpec.source 一致，否则导出表头不可知。

### 6.3 拓扑排序（Kahn）

```text
inDeg = {r.table: 0 for r in routes}
adj   = {r.table: [] for r in routes}
for r in routes:
    if r.parent != "":
        adj[r.parent].push(r.table)
        inDeg[r.table] += 1
queue = [t for t in inDeg if inDeg[t] == 0]
out = []
while queue:
    t = queue.pop_front()
    out.push(t)
    for c in adj[t]:
        inDeg[c] -= 1
        if inDeg[c] == 0: queue.push(c)
if len(out) != len(routes):
    err E_PROFILE_TOPOLOGY_CYCLE
return [route by t for t in out]
```

混编模式：每个 `ClassSpec.routes` 独立排序，class 之间不存在父子。

### 6.4 批内唯一性检测

实现：`QHash<QString, QHash<QString,int>>`，外层 key 是 `route.table`（或 `classId:table`），内层 key 是 conflict 值的 join 串（`|` 分隔，前置长度避免歧义），value 是首次出现的 Excel 行号。

伪码：

```text
for payload in row.payloads:
    key = encodeConflict(payload.conflictVals)   // 例如 "3|001|7|01" 表示 order_no=001, line_no=01
    if seen[payload.routeKey].contains(key):
        errors.add(row=excelRow, col=conflictColsJoin,
                   raw=key, code=E_VALIDATE_DUPLICATE,
                   msg=fmt("conflict key 与第 %1 行重复").arg(seen[..][key]))
    else:
        seen[payload.routeKey][key] = excelRow
```

`encodeConflict` 把值拼成 `<len>|<v1>|<len>|<v2>|...` 形式以避免 `"ab|c"` 与 `"ab","c"` 撞键。

### 6.5 业务键 FK 注入

当 `RouteSpec.fkInject` 存在：

1. 查找同一行同一 class 内 `route.table == fkInject.fromTable` 的父 payload。
2. 从父 payload 的 binds 中取出 `fkInject.fromColumn` 对应值（来源：父 payload 的 dbColumns/binds 数组）。父 payload 必须包含该列，否则 ProfileValidator 阶段拒绝。
3. 将该值写入子 payload：
   - 如果子 payload 已有 `fkInject.toColumn`：覆盖（Profile 中应避免显式映射 FK 列；ProfileValidator 在发现 Excel source 又映射 FK 列时上报 `E_PROFILE_COLUMN_NOT_FOUND`/或专门告警。MVP 选择直接覆盖并不报错，保持简单）。
   - 否则在 dbColumns/binds 末尾追加；同时把它纳入 conflict 值（若它属于 conflict.columns）。

执行顺序：批内唯一性检测在 FK 注入**之后**进行，因为 FK 注入会改写 conflict 值。

### 6.6 Discriminator 匹配

`Router::match(value)`：

1. `value.isNull()` → null；遍历 classes 找 `matchEquals == ""`（MVP 不支持），直接返回 nullptr。
2. 比较 `value.toString() == class.matchEquals`，首个匹配返回。
3. 任何 class 都不匹配 → `nullptr`。

ImportService 拿到 nullptr → 写一条 `E_ROUTE_UNMATCHED` 进 ErrorCollector，继续校验下一行（让宿主一次看到所有未匹配行）；最终因为存在错误而终止导入。

### 6.7 expandRows 组装（导出）

由 §6.2 自动 LEFT JOIN 隐式实现：父行字段在多行重复，子行字段逐行展开。不需要单独"组装" 步骤。

混编导出：

```text
out_rows = []
for class in profile.classes:
    sql = explicit or autoJoin(class.routes, export)
    rs  = exec(sql)
    while rs.next():
        row = QHash<header,QVariant> from rs
        row[classColumn] = class.id   // 注入鉴别列
        out_rows.append(row)
sort out_rows by export.orderBy   // 内存排序
write to xlsx
```

混编导出表头集合：所有 class 用过的 source 名的并集 + `classColumn`。在写入前固定列顺序，宿主可后续手动调整。

### 6.8 自动 Profile 生成

对应 §5.3 算法，输出 JSON 字符串结构：

```json
{
  "profileName": "auto_<table>",
  "sheet": "<table>",
  "headerRow": 1,
  "mode": "singleTable",
  "table": "<table>",
  "conflict": { "columns": ["..."] },
  "columns": { "<col>": { "source": "<col>", "validators": [...] }, ... },
  "export": { "orderBy": ["<conflict.columns[0]>"] }
}
```

字段顺序固定（写 `QJsonObject` 时手动构建键序列）以便人读和 diff。

---

## 7. 端到端时序

### 7.1 importExcel

```
DataBridge::importExcel
  └── ImportService::run(profile, xlsxPath, opts)
       Phase A: 准备
         1. ExcelReader.open(xlsx)                    // 失败 -> E_OPEN_XLSX
         2. ExcelReader.selectSheet(profile.sheet)
         3. ExcelReader.readHeader(profile.headerRow) // 失败 -> E_HEADER_NOT_FOUND
       Phase B: 校验
         4. ProfileValidator.run(profile, catalog, excelHeaders)
            -> 失败立即返回 ImportResult{ok=false, errors=...}, 不开事务
         5. Compile ValidatorChain per (route, column)
       Phase C: 全量行映射 + 行级校验
         for r = firstDataRow .. lastRow:
            readRows++
            if mixed:
                cls = router.match(excel.cellBySource(r, discriminatorSource))
                if !cls: errors.add(E_ROUTE_UNMATCHED); continue
                routes = cls.routes_topo
            else:
                routes = profile.routes_topo
            payloads = Mapper.map(routes, r)            // 含行级校验
            FkInjector.inject(payloads)
            BatchUniquenessChecker.check(payloads, r)
            context.append({row=r, classId, payloads})
         6. if errors.notEmpty(): return ok=false
       Phase D: 落库（单事务）
         7. db.transaction()
         8. for ctx in contexts:
              for payload in ctx.payloads (topo order):
                sql = sqlBuilder.buildUpsert(payload)
                if newSql: query.prepare(sql)         // 同 SQL 复用 prepare
                bind & exec
                if !ok: errors.add(E_DB_UPSERT, row=ctx.excelRow); break outer
              writtenRows++
         9. if errors.empty(): db.commit()
            else            : db.rollback()
        10. return ImportResult
```

约束：

- Phase A/B/C 不写任何 DB。
- Phase D 一旦失败立刻 ROLLBACK，全部数据无效。
- `writtenRows` 只在 commit 成功后才作为最终值；rollback 时返回 0。

### 7.2 exportExcel

```
DataBridge::exportExcel
  └── ExportService::run(profile, xlsxPath, opts)
       1. ExcelWriter.open(xlsxPath, sheetName)
       2. branch by mode:
          a. singleTable / multiTable:
             sql = profile.exportSpec.explicitSql.isEmpty() ?
                   SqlBuilder.buildAutoJoinSelect(routes_topo, exportSpec)
                   : exportSpec.explicitSql
             rs  = exec(sql)
             headers = SELECT 列 AS 别名顺序
             writer.writeHeader(headers)
             while rs.next(): writer.writeRow(...)
          b. mixed:
             allRows = []
             headers = union(source per class) + classColumn (固定列序)
             for cls in profile.classes:
                sql = explicitSql.isEmpty() ? autoJoin(cls.routes, exportSpec) : explicitSql
                rs  = exec(sql)
                while rs.next():
                  row = {h: rs.value(h) for h in headers}
                  row[classColumn] = cls.id
                  allRows.append(row)
             sort allRows by exportSpec.orderBy
             writer.writeHeader(headers)
             writer.writeRow(...)
       3. writer.save()
       4. return ExportResult
```

任何 SQL 执行错误 → `E_EXPORT_QUERY`；任何写盘错误 → `E_WRITE_XLSX`。

### 7.3 generateAutoProfileJson

```
DataBridge::generateAutoProfileJson(table)
  1. catalog.hasTable(table) ? else E_PROFILE_TABLE_NOT_FOUND
  2. AutoProfileBuilder.build(catalog.table(table)) -> ProfileSpec
  3. ProfileSpec -> JSON string (固定字段顺序)
  4. 不写入内部 profile 缓存（由宿主决定是否再 loadProfileFromString）
```

---

## 8. 事务与错误处理

### 8.1 事务边界

- 一次 `importExcel` 调用 = 一个事务。
- 事务通过 `db.transaction()` / `db.commit()` / `db.rollback()`，不手写 `BEGIN`。
- 在 `DataBridge::open` 中设置：

```sql
PRAGMA busy_timeout = <busyTimeoutMs>;
PRAGMA journal_mode = WAL;                 -- 当 enableWal 为 true
PRAGMA foreign_keys = ON;
```

- 不设置 `PRAGMA synchronous = OFF`，保留默认 `NORMAL`/`FULL`，避免崩溃数据风险。

### 8.2 错误聚合策略

| 阶段 | 错误处理 |
|---|---|
| 打开 DB / Excel | 单一错误立刻返回 |
| Profile 加载 / Profile 校验 / 表头校验 | 尽量收集后一次返回 |
| 行级校验 | 收集全部错误后才决定是否进入写阶段；不会因为某一行错误中止后续行的校验，方便宿主一次性反馈所有问题 |
| 写阶段 | 任意一行写失败立刻 rollback 并返回错误，不再尝试后续行 |

### 8.3 SQLite 配置

- `QSQLITE` 驱动；连接名带随机后缀避免与宿主已存在的同名连接冲突（`QSqlDatabase::addDatabase("QSQLITE", "dbridge_<uuid>")`）。
- 关闭时 `db.close()` 后 `QSqlDatabase::removeDatabase(name)`，避免连接泄漏导致后续 open 警告。

---

## 9. 五类业务目标实现路径（核对）

| 目标 | 主路径函数 | 核心代码点 |
|---|---|---|
| Upsert 导入 | `ImportService::executeWrite` | `SqlBuilder::buildUpsert` 生成 INSERT/ON CONFLICT/DO UPDATE；只 update 非 conflict、Profile 已映射的列；从不删除现有行；不使用 `INSERT OR REPLACE` |
| 校验告警终止 | `ImportService::validateAll` | `ProfileValidator::run` + `ValidatorChain::run` + 批内唯一性 + 父子同批 conflict 检查；任意错误 → 不进入 `executeWrite` |
| 未来未知表 | `DataBridge::generateAutoProfileJson` → 用户 → `DataBridge::loadProfileFromString` | `SchemaIntrospector::load` + `AutoProfileBuilder::build` |
| 单类多表（m 集合） | `Mapper::map` + `TopoSorter` + `FkInjector::inject` | 一个 RowContext 含多个 RoutePayload；按 parent 拓扑顺序 upsert |
| 混编 A/B/C 多集合 | `Router::match` + per-class `Mapper::map` | 每行进入唯一 class；同 Sheet 内 m/n/o payload 独立维护；导出按 class 分别查询再合并 |

---

## 10. 阶段化任务清单

> 每个阶段都对应 MVP 设计 §14 的一阶段；任务粒度按 "可独立提交+可独立测试" 拆分。

### 10.1 阶段 1：基础工程 + 单表 Upsert

| # | 任务 | 关联文件 | 验收 |
|---|---|---|---|
| 1.1 | 建立 CMake、Qt5/QtSql、QXlsx vendor、`pre-commit` 钩子接入 | `CMakeLists.txt`, `3rdparty/QXlsx/` | `cmake -S . -B build && cmake --build build` 通过 |
| 1.2 | 实现 `DataBridge` PImpl + open/close + 连接命名 | `include/dbridge/DataBridge.h`, `src/DataBridge.cpp` | 单元测试：能 open 不存在路径返回 `E_OPEN_DB` |
| 1.3 | 实现 `SchemaIntrospector` 读 table_xinfo / index_list / index_info / foreign_key_list | `src/schema/*` | 单元测试 fixture：自建 SQLite，读取列/主键/唯一索引正确 |
| 1.4 | 实现 `ProfileLoader` 解析 SingleTable | `src/profile/ProfileLoader.*` | 单元测试：合法 JSON 解析、缺字段报错 |
| 1.5 | 实现 `ExcelReader` 读表头 + cellBySource | `src/excel/ExcelReader.*` | 单元测试：能从 fixture xlsx 读出 header 和指定单元格 |
| 1.6 | 实现 `SqlBuilder::buildUpsert`（无 update 列时 DO NOTHING） | `src/sql/SqlBuilder.*` | 单元测试：给定 payload 生成期望 SQL 字符串 |
| 1.7 | 实现 `ImportService` 单表路径 + AllOrNothing 事务 | `src/service/ImportService.*` | 集成测试：单表导入新增+更新均成功；中间错误回滚 |

**阶段 1 出口**：能通过 `customer_basic` Profile（MVP §5.1）把 `Customers.xlsx` 导入 SQLite，已存在的 customer_no 走 UPDATE，不存在的走 INSERT，事务原子。

### 10.2 阶段 2：校验与错误告警

| # | 任务 | 关联文件 | 验收 |
|---|---|---|---|
| 2.1 | `ValidatorChain` + 全部内置 validator | `src/validation/*` | 单元测试覆盖 9 个 token；token 解析错误在 Profile 加载阶段报 `E_PROFILE_PARSE` |
| 2.2 | `ProfileValidator` 表/列/conflict/header 全量对账 | `src/profile/ProfileValidator.*` | 单元测试：每条规则各有正反用例 |
| 2.3 | `ErrorCollector` + RowError 字段填充规范 | `src/service/ImportService.*` | 单元测试：sheet/row/column/raw 信息齐全 |
| 2.4 | 批内唯一性检测（§6.4） | `src/mapping/BatchUniqueness.*`（可放在 Mapper 同目录） | 单元测试：3 行重复时报第 1 次出现行号 |
| 2.5 | 行级校验失败时严格不开事务 | `src/service/ImportService.*` | 集成测试：故意造一行错误，DB 中应无任何写入 |

**阶段 2 出口**：导入前发现错误返回 `ImportResult{ok=false}`，`errors` 列表能完整定位 Sheet/行/列/原值/错误码，DB 零写入。

### 10.3 阶段 3：未知单表自省

| # | 任务 | 关联文件 | 验收 |
|---|---|---|---|
| 3.1 | `AutoProfileBuilder` 实现 §5.3 算法 | `src/profile/AutoProfileBuilder.*` | 单元测试：3 类表（INTEGER PK AUTOINC、复合 PK、UNIQUE 索引）产出可执行 Profile |
| 3.2 | `DataBridge::generateAutoProfileJson` 接口 + JSON 字段顺序固定 | `src/DataBridge.cpp` | 单元测试：输出与黄金 JSON 字节级一致 |
| 3.3 | 自动 Profile 经 `loadProfileFromString` 后可直接 import / export | 端到端 | 集成测试：运行期 `CREATE TABLE`，自动 profile，导入导出回环 |

**阶段 3 出口**：宿主新建一张满足约束的简单表，调用 `generateAutoProfileJson` → `loadProfileFromString` → `importExcel/exportExcel` 即可工作，无需重编译。

### 10.4 阶段 4：多表集合

| # | 任务 | 关联文件 | 验收 |
|---|---|---|---|
| 4.1 | `ProfileLoader` 支持 MultiTable + `parent` + `fkInject` | `src/profile/ProfileLoader.*` | 单元测试：MVP §5.2 JSON 完整解析 |
| 4.2 | `TopoSorter` Kahn 实现 + 环检测 | `src/mapping/TopoSorter.*` | 单元测试：1 父 2 子 / 链式 3 表 / 环路均符合预期 |
| 4.3 | `FkInjector` 业务键注入 | `src/mapping/FkInjector.*` | 单元测试：父业务键能注入到子 payload，且参与 conflict 值 |
| 4.4 | `Mapper` 多 route 模式 | `src/mapping/Mapper.*` | 单元测试：一行 Excel 拆成多 payload，顺序为 topo |
| 4.5 | `SqlBuilder::buildAutoJoinSelect` | `src/sql/SqlBuilder.*` | 单元测试：与黄金 SQL 字节级一致（去除多余空白） |
| 4.6 | `ExportService` MultiTable 路径 | `src/service/ExportService.*` | 集成测试：从 orders+order_items 导出，父行随子行展开 |

**阶段 4 出口**：MVP §5.2 `order_m_set` Profile 端到端通过：A 行（OrderNo+多 LineNo）能正确写入 `orders` 和 `order_items`，导出回 Sheet 字段顺序正确。

### 10.5 阶段 5：A/B/C 混编

| # | 任务 | 关联文件 | 验收 |
|---|---|---|---|
| 5.1 | `ProfileLoader` 支持 Mixed mode + `discriminator` + `classes` | `src/profile/ProfileLoader.*` | 单元测试：MVP §5.3 JSON 完整解析 |
| 5.2 | `Router` equals 匹配 + 未匹配告警 | `src/mapping/Router.*` | 单元测试：A/B/C 命中正确；未知值上报 `E_ROUTE_UNMATCHED` |
| 5.3 | `Mapper` 按 class 选 route 集 | `src/mapping/Mapper.*` | 集成测试：A→m1+m2、B→n1、C→o1 |
| 5.4 | `ExportService` Mixed 路径 + classColumn 注入 + 内存排序 | `src/service/ExportService.*` | 集成测试：m/n/o 数据导出回同一个 Sheet，Type 列正确 |

**阶段 5 出口**：MVP §5.3 `mixed_abc` Profile 端到端通过；与导入互为逆操作（导入→导出→再导入应得到一致结果集，业务键稳定）。

---

## 11. 测试实现

### 11.1 测试目录

```
tests/
├── CMakeLists.txt
├── data/
│   ├── sql/
│   │   ├── 01_customer.sql
│   │   ├── 02_orders.sql
│   │   └── 03_mixed.sql
│   ├── xlsx/
│   │   ├── customer_basic.xlsx
│   │   ├── order_m_set.xlsx
│   │   └── mixed_abc.xlsx
│   └── profiles/
│       ├── customer_basic.json
│       ├── order_m_set.json
│       └── mixed_abc.json
├── unit/
│   ├── tst_profile_loader.cpp
│   ├── tst_schema_introspector.cpp
│   ├── tst_validator_chain.cpp
│   ├── tst_sql_builder.cpp
│   ├── tst_topo_sorter.cpp
│   ├── tst_router.cpp
│   └── tst_auto_profile_builder.cpp
└── integration/
    ├── tst_import_single.cpp
    ├── tst_import_multitable.cpp
    ├── tst_import_mixed.cpp
    ├── tst_export_single.cpp
    ├── tst_export_multitable.cpp
    └── tst_export_mixed.cpp
```

`tests/CMakeLists.txt`：每个 `tst_*.cpp` 注册为 `add_test`，统一链接 `Qt5::Test` 与 `dbridge`。fixture 用 `:memory:` SQLite + `tests/data/sql/*.sql` 初始化，xlsx fixture 直接由测试代码用 QXlsx 写出（避免提交二进制）。

### 11.2 单元测试要点

| 文件 | 用例 |
|---|---|
| `tst_profile_loader` | 3 种 mode 合法 JSON / 缺 mode / 缺 sheet / mode 未知 / `routes` 空 / `match.equals` 重复 |
| `tst_schema_introspector` | 单表读取列+主键、复合 PK、UNIQUE 索引、外键、autoIncrement 判定 |
| `tst_validator_chain` | 9 类 token 正反用例；null 跳过非 notNull；token 解析错误 |
| `tst_sql_builder` | upsert 含 update / DO NOTHING / 多列 conflict；auto join 单表/双表 |
| `tst_topo_sorter` | 单根、链式、环路、孤立节点 |
| `tst_router` | A/B/C 命中、未知值返回 nullptr、空 discriminator |
| `tst_auto_profile_builder` | AUTOINCREMENT 主键跳过、复合 PK、UNIQUE 选择、无可用键报错 |

### 11.3 集成测试用例（对照 §1 矩阵）

| 用例 | 步骤 |
|---|---|
| `upsert_new` | 空表 → 导入 3 行 → 表内 3 行 |
| `upsert_update` | 表内已有 1 行 → 导入同 conflict key 修改 → UPDATE 生效，主键/未映射列不变 |
| `upsert_no_unmapped_overwrite` | 表内 `extra_col` 已有值且 Profile 未映射 → 导入后 `extra_col` 保持原值 |
| `validate_required` | 必填空 → ok=false，errors 至少 1 条 |
| `validate_type` | 字段写字符串到 int 列 → `E_VALIDATE_TYPE` |
| `validate_regex` | 不符合正则 → `E_VALIDATE_REGEX` |
| `validate_dup_in_batch` | 同 conflict key 在 2 行出现 → `E_VALIDATE_DUPLICATE`，DB 零写入 |
| `auto_profile_single` | 运行期 CREATE TABLE → generateAutoProfileJson → loadProfileFromString → importExcel → 数据正确 |
| `auto_profile_no_unique_key` | 表无 PK 无 UNIQUE → `E_PROFILE_NO_CONFLICT_KEY` |
| `multi_table_split_and_export` | OrderNo+多 LineNo Excel → orders + order_items 行数符合预期 → 导出回 Sheet 字段顺序正确 |
| `mixed_abc_roundtrip` | A/B/C 混杂 Excel 导入 m/n/o → 导出 → Type 列正确、数据顺序符合 orderBy |

### 11.4 测试运行

```bash
cd build && ctest --output-on-failure
```

CI 钩子可直接跑这条命令；pre-commit 已强制 clang-format/clang-tidy。

---

## 12. 验收 Checklist（对照 MVP §16）

完工前逐条核对：

- [ ] 单表 Excel 导入：新增插入、已有更新均符合预期，未映射列不被覆盖。
- [ ] 任意校验错误终止导入，`writtenRows == 0`，数据库零写入。
- [ ] 运行期新增的简单 SQLite 表能通过 `generateAutoProfileJson` 生成 Profile，并完成导入导出回环。
- [ ] A 行能拆分写入 m 集合多表，父子顺序正确，FK 业务键由 fkInject 注入。
- [ ] 混杂 A/B/C 行能分别写入 m/n/o 集合，class 内多表也能正确 FK 注入。
- [ ] 从 m/n/o 导出回同一个 Sheet，Type 列正确，列顺序固定，可被同一 Profile 再次导入。
- [ ] 所有上述场景在 `tests/integration` 中有自动化用例并稳定通过。
- [ ] `dbridge` 公开头只暴露 `DataBridge.h` / `Types.h` / `Errors.h`，不泄漏 Qt-Sql/QXlsx。
- [ ] Profile 加载、行校验、批内唯一性、FK 注入、SQL 生成、事务、错误聚合全部按本文档约定实现，无超出 MVP 范围的附加能力。

满足以上 9 条即视为 MVP 实现完成；任何超出条目（异步、ChunkCommit、流式 xlsx、多数据库、ABI 等）一律不属于本文档实现范围，进入长期演进文档处理。
