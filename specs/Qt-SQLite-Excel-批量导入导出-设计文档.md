# Qt + SQLite + Excel 批量导入导出动态库设计文档

> 适用范围：基于 Qt 5.12.12 + C++17 的桌面端 / 中间件 ETL 组件。
> 输出形态：跨平台动态库（`.dll` / `.so` / `.dylib`），提供 C++ Public Interface（"稳定"的具体含义见 §3.0 兼容性矩阵）。
> 上游需求来源：`specs/Qt-SQLite-Excel-批量导入导出参考.md`。

---

## 1. 设计目标与约束

### 1.1 功能目标
1. **Upsert 同步**：导入时主键/唯一键存在则原地更新，不存在则插入，绝不触发 `DELETE + INSERT` 级联灾难。
2. **前置数据校验**：在数据落盘前完成结构、词法、关联完整性三级校验；一旦发现错误立即终止当前批次并精确告警（含 Sheet / 行号 / 列号 / 原值 / 失败原因）。
3. **动态自省**：对编译期未知的表结构（未来新增表）能在运行期通过元数据探测自动完成读写映射，无需重新编译。
4. **异构单 Sheet 混编与多维路由**：
   - 同一 Sheet 中混杂 A/B/C 类行；
   - A 行 → m 集合（多表），B 行 → n 集合（多表），C 行 → o 集合（多表），支持主从外键级联注入；
   - 反向：从 m/n/o 集合反规范化聚合后错位组装写回同一 Sheet。
5. **数据库可扩展**：先支持 SQLite，未来通过插件方式扩展 MySQL / Oracle / PostgreSQL 等。
6. **大数据量稳定性**：分片处理、多线程、不阻塞 UI、可观测可审计。

### 1.2 技术约束
| 项 | 选择 | 说明 |
|---|---|---|
| 框架 | Qt 5.12.12（锁定，本期不评估 Qt6） | LTS 版本，覆盖目标平台 |
| 语言 | C++17（统一） | `std::optional` / `std::variant` / `if constexpr` / 结构化绑定；不再混用 11/14 |
| Excel | QXlsx（源码集成）+ 自研 zip+`QXmlStreamReader` 流式读通道 | QXlsx 自身**不提供**真正 SAX 级流式读，详见 §11.3 |
| 数据库 | QtSql 模块 + 自研抽象层 | 默认 SQLite，预留 MySQL/Oracle/PG 适配位 |
| 配置 | JSON（QJsonDocument） | 路由 / 映射 / 校验规则均外置 |
| 日志 | 封装 Qt 日志 + 可选 spdlog | 结构化输出，便于审计 |
| 线程 | `QThread` + 显式 Producer/Consumer 队列 | QtSql 连接 / `QSqlQuery` 有线程亲和性，每线程独立连接，详见 §11.1 |
| 构建 | qmake + CMake 双轨支持 | 便于不同接入方集成 |

### 1.3 非目标 / 范围限定
- 不实现完整 ORM（不做对象 ↔ 表的字段级反射注册，仅做"行 ↔ 表"的运行期映射）。
- 不实现 SQL 解析器，仅以模板拼装 + 预编译执行。
- 不在本期支持跨库分布式事务，单连接 ACID 即可。
- **"未来未知表自省"（auto profile）MVP 范围限定**：仅支持"单表 + 表头列名严格匹配 + 存在唯一/主键"的简单表；其他复杂条件（identity/生成列/触发器/外键拓扑/保留字列名/复合主键等）由 §5.5.1 的检查矩阵逐项判定，**不满足者一律回退为"候选 profile + 人工确认"**而非直接拒绝。详见 §5.5.1。
- **不承诺真正二进制 ABI**：详见 §3.0；本库的"接口稳定"被定义为"同编译器主版本、同 CRT、同 Qt 主+小版本、同 C++ 标准、同 STL 调试/Release 配置"前提下的源码/可重链接兼容，跨这些维度需要重新编译。

---

## 2. 总体架构

### 2.1 分层架构总图

```
┌─────────────────────────────────────────────────────────────────────────┐
│                       Application / Host (GUI / CLI)                    │
└───────────────▲───────────────────────────────────────▲─────────────────┘
                │ Public C++ API                        │ Signals
┌───────────────┴───────────────────────────────────────┴─────────────────┐
│                       ⬛ DataBridge Public Interface                     │
│  IDataBridge | IImportJob | IExportJob | IProgressReporter | IAlertSink │
├─────────────────────────────────────────────────────────────────────────┤
│                          🅢 Service / Orchestration                     │
│   ImportOrchestrator  │  ExportOrchestrator  │  JobScheduler (QThread)  │
├──────────────┬─────────────────────┬──────────────────────┬─────────────┤
│  📥 Excel    │  🧠 Routing &       │  ✅ Validation       │  💾 DB      │
│  I/O Layer   │  Mapping Engine     │  Engine (3-layer)    │  Access     │
│  (QXlsx)     │  - Discriminator    │  - Schema validator  │  Layer      │
│  - Reader    │  - Field Slicer     │  - Regex / Range     │  (Abstract) │
│  - Writer    │  - Topo Sorter      │  - FK Reference      │             │
├──────────────┴──┬──────────────────┴──────────────────┬──┴─────────────┤
│ 🧬 Schema       │ 🛠 SQL Builder & Upsert Executor   │ 🧾 Transaction  │
│ Introspector    │ - ON CONFLICT DO UPDATE             │ Manager (ACID) │
│ (PRAGMA / IS)   │ - Prepared Statement + execBatch    │ Savepoint      │
├─────────────────┴─────────────────────────────────────┴────────────────┤
│            🔌 Database Driver SPI（IDatabaseDriver Plugin）             │
│   ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌────────────┐  │
│   │ SQLiteDriver │  │ MySQLDriver  │  │ OracleDriver │  │ PgDriver   │  │
│   │  (built-in)  │  │ (future)     │  │ (future)     │  │ (future)   │  │
│   └──────────────┘  └──────────────┘  └──────────────┘  └────────────┘  │
├─────────────────────────────────────────────────────────────────────────┤
│        🪵 Cross-cutting: Logger | Config Loader | Error Codes | i18n    │
└─────────────────────────────────────────────────────────────────────────┘
```

### 2.2 模块职责（单一职责约束）

| 模块 | 职责 | 不做的事 |
|---|---|---|
| Public API | 暴露稳定的接入面，PImpl 隔离 ABI | 业务实现 |
| ImportOrchestrator | 编排导入流水线，状态管理 | 直接读 Excel / 写库 |
| ExportOrchestrator | 编排导出流水线 | 直接拼 SQL |
| Excel I/O | 解析 / 写入 xlsx，按行迭代 | 业务校验、路由 |
| Routing Engine | 按 Discriminator 鉴别行类型并触发对应路由策略 | 拼 SQL、落库 |
| Mapping Engine | 把宽行 → 多个 RowPayload；多表行 → 一个宽行 | 决策路由、落库 |
| Validation Engine | 三级校验（结构 / 词法 / 关联） | 修改原始数据 |
| Schema Introspector | 元数据发现 + 缓存数据字典 | 业务字段含义 |
| SQL Builder | 基于元数据动态生成 Upsert / Select | 执行 |
| Upsert Executor | 预编译 + 批量执行 | 事务边界 |
| Transaction Manager | 事务 / Savepoint / 回滚 | SQL 生成 |
| DB Driver SPI | 厂商方言适配（Upsert 语法、占位符、回填主键） | 解析 Excel |
| Logger / Alert | 结构化告警 + 审计日志 | 业务流转 |

---

## 3. Public Interface 设计

> 全部对外类型放置于命名空间 `dbridge`。库名暂定 `libdbridge`。
> 接口使用纯抽象类（虚函数）+ 工厂函数，内部用 PImpl 屏蔽实现。

### 3.0 ABI 边界与兼容性矩阵（关键澄清）

公开接口中暴露了 `QString` / `QVariant` / `QVariantMap` / `QList` / `QHash` / `std::shared_ptr` / `std::function` / 纯虚类指针，这些**都不是真正的二进制稳定 ABI**。PImpl 只能隐藏实现细节，无法跨编译器或 STL/Qt 版本边界保证 ABI。本库采取**双轨策略**：

| 通道 | 形态 | 适用 | 兼容矩阵 |
|---|---|---|---|
| **主通道**：C++/Qt API | 见 §3.1–§3.3 | 与本库一起编译/链接的宿主 | 必须满足：**Qt 5.12.12 严格一致**、同编译器主版本、同 CRT/标准库版本、同 C++ 标准（C++17）、Release/Debug 一致、相同 `_GLIBCXX_USE_CXX11_ABI` 与异常/RTTI 配置 |
| **辅通道**：C ABI Handle（可选） | `dbridge_open` / `dbridge_import` 等不透明句柄 + POD 结构 | 跨编译器、跨语言绑定（Python/C#/Go） | 仅依赖 C ABI；为简单场景设计，不暴露多表 FK 注入与流式 SQLite 元数据查询 |

约束：
- 公开符号统一通过 `DBRIDGE_API`（GCC `visibility("default")` / MSVC `__declspec(dllexport)`）控制，默认隐藏。
- 公共头不引入 `<unordered_map>` / `<filesystem>` / `<chrono>` 等容易踩 ABI 雷的 STL 容器作为参数/返回值；如必需，封装为本库自身的 POD 视图。
- 库整体走语义化版本：API 冻结后 MINOR 不破坏接口；驱动 SPI（§3.3）保留 `capabilities()` 扩展位以允许向后兼容地新增能力位。
- ABI 兼容性测试在 §13 中作为验收门槛单列。

### 3.1 头文件布局

```
include/dbridge/
├── DataBridge.h            // 顶层入口 & 工厂
├── IDataBridge.h           // 主门面接口
├── IImportJob.h            // 异步导入任务句柄
├── IExportJob.h            // 异步导出任务句柄
├── IAlertSink.h            // 告警回调
├── IProgressReporter.h     // 进度回调
├── IDatabaseDriver.h       // 数据库驱动 SPI（厂商扩展点）
├── Types.h                 // 通用类型：ColumnInfo / TableSchema / RowError
├── Config.h                // 路由 / 映射配置结构
└── Errors.h                // 错误码 / 异常类
```

### 3.2 核心接口（伪签名）

```cpp
namespace dbridge {

enum class DbKind { SQLite, MySQL, Oracle, PostgreSQL, Custom };

struct ConnectionSpec {
    DbKind        kind = DbKind::SQLite;
    QString       dsn;          // SQLite 即文件路径；其余按方言
    QString       user;
    QString       password;
    QVariantMap   extra;        // 厂商私参（如 WAL / charset）
};

// 事务/失败语义模式（详见 §9）
enum class TxMode {
    AllOrNothing,   // 整 Job 一个事务；任何不可恢复错误 → 整体 ROLLBACK
    ChunkCommit,    // 每 chunk 一个事务；前面成功的 chunk 保留落盘
    ContinueOnError // 行级错误不阻断，仅记录到 AlertSink；最终 chunk 提交策略由二级开关决定
};

struct ImportOptions {
    TxMode      txMode             = TxMode::AllOrNothing;
    int         chunkRows          = 1000;
    bool        abortOnPreValidate = true;   // L1/L2 校验出错即终止（与 §1.1 硬约束对齐）
    int         readerThreads      = 1;      // QtSql 写入连接必须单线程，详见 §11.1
    QVariantMap extra;                       // 厂商/驱动私参（如 Oracle 唯一冲突重试次数、退避策略；MySQL ODKU 别名写法开关等，键名以驱动 kind 为前缀，如 "oracle.retry.maxAttempts"）
};

class IDataBridge {
public:
    virtual ~IDataBridge() = default;

    // 连接 / 资源
    virtual bool open(const ConnectionSpec&, QString* err = nullptr) = 0;
    virtual void close() = 0;

    // 配置加载（路由 / 映射 / 校验规则）
    virtual bool loadProfile(const QString& jsonPath, QString* err = nullptr) = 0;
    // 内嵌 JSON 加载（供草稿确认后无需落盘即可使用；与 loadProfile 共用同一 schema 校验）
    virtual bool loadProfileFromString(const QString& profileJson,
                                       QString* err = nullptr) = 0;

    // 主动元数据刷新（用于"未来未知表"）
    virtual bool refreshSchema(QString* err = nullptr) = 0;

    // auto profile：尝试为单表生成可直接执行的 profile；当目标表不满足 §5.5.1 全部硬条件时，
    // 返回的 ProfileDraft.executable = false，并通过 issues 列出每条不满足项，由 Host 决定
    // 是修订草稿（保存为命名 profile 后再 loadProfile）还是放弃。
    virtual ProfileDraft generateAutoProfileDraft(const QString& table,
                                                  const QString& xlsxHeaderHint = QString()) = 0;

    // 异步导入：返回 Job 句柄，调用方可订阅进度 / 告警 / 完成
    // 回调生命周期约定（详见 §3.2.1）：progress/alert 必须在 Job 终态触发之前保持有效；
    // Host 销毁前应先 Job::cancel() + wait()。如需更安全的解耦请用 registerProgressSink /
    // registerAlertSink 注册版本（返回 RAII handle，析构时安全解绑）。
    virtual std::shared_ptr<IImportJob>
        importExcel(const QString& xlsxPath,
                    const QString& profileName,
                    const ImportOptions& opts = {},
                    IProgressReporter* progress = nullptr,
                    IAlertSink*        alert    = nullptr) = 0;

    // RAII 注册：返回的 handle 析构时安全解绑；可用于 Host 生命周期短于 Job 的场景
    virtual std::unique_ptr<class ISinkHandle>
        registerProgressSink(IProgressReporter*) = 0;
    virtual std::unique_ptr<class ISinkHandle>
        registerAlertSink(IAlertSink*) = 0;

    // 异步导出
    virtual std::shared_ptr<IExportJob>
        exportExcel(const QString& xlsxPath,
                    const QString& profileName,
                    IProgressReporter* progress = nullptr,
                    IAlertSink*        alert    = nullptr) = 0;
};

// 工厂
DBRIDGE_API std::unique_ptr<IDataBridge> createDataBridge();

} // namespace dbridge
```

### 3.2.1 Job 句柄与生命周期

```cpp
enum class JobState {
    Pending,        // 已创建未启动
    Running,        // 正在执行
    Cancelling,     // 收到取消请求，等待 Worker 协作退出
    // —— 四个终态 ——
    Succeeded,      // 全部成功
    PartialSuccess, // ChunkCommit / ContinueOnError 下有失败但已提交部分成功
    Failed,         // AllOrNothing 整体回滚 / 致命错误终止
    Cancelled       // 因 cancel() 而停止（已落盘部分由模式决定保留与否）
};

class IImportJob {
public:
    virtual ~IImportJob() = default;

    virtual JobState   state()  const = 0;
    virtual void       cancel()       = 0;             // 协作式取消，非立即
    virtual bool       wait(int timeoutMs = -1) = 0;   // 阻塞直到终态
    virtual int        okRows()     const = 0;
    virtual int        failedRows() const = 0;
    virtual ErrorInfo  lastError()  const = 0;         // 见 §10.1
};
```

调用契约（强约束）：
1. 回调线程：所有 `IProgressReporter` / `IAlertSink` 回调均在 Worker 线程触发；Host 若需更新 UI 必须自行 marshal（QMetaObject::invokeMethod / signal-slot QueuedConnection）。
2. 生命周期：Host 必须保证 `progress` / `alert` 在 Job 终态之前不被销毁；若无法保证，使用 `IDataBridge::registerProgressSink` / `registerAlertSink` 注册版本（返回 RAII `ISinkHandle`，析构时安全解绑）。
3. 关库：`IDataBridge::close()` 之前必须先对所有未结束 Job 触发 `cancel()` 并 `wait()`，否则行为未定义。
4. 终态语义：`AllOrNothing` 模式只会出现 `Succeeded` / `Failed` / `Cancelled`；`ChunkCommit` / `ContinueOnError` 才会出现 `PartialSuccess`。`onCompleted` 回调签名见 §10.2，使用 `JobState` 区分而非仅靠 ok/failed 行数。

### 3.2.2 配套回调与导出 Job 契约

```cpp
// 进度回调（最小接口）
class IProgressReporter {
public:
    virtual ~IProgressReporter() = default;
    // 0~100 整数；rowsDone/rowsTotal=-1 表示未知（如流式未读完）
    virtual void onProgress(int percent, qint64 rowsDone, qint64 rowsTotal) = 0;
};

// 导出 Job 与导入 Job 对称
class IExportJob {
public:
    virtual ~IExportJob() = default;
    virtual JobState  state()  const = 0;
    virtual void      cancel()       = 0;
    virtual bool      wait(int timeoutMs = -1) = 0;
    virtual qint64    rowsWritten() const = 0;
    virtual ErrorInfo lastError()   const = 0;
};

// 注册 Sink 的 RAII 句柄
class ISinkHandle {
public:
    virtual ~ISinkHandle() = default;   // 析构 = 安全解绑（线程安全、幂等）
    virtual void detach() = 0;          // 显式提前解绑；多次调用幂等
    virtual bool isAttached() const = 0;
};
```

ISinkHandle 契约：
- **析构 = 解绑**：析构时把本 Sink 从内部订阅表移除，并等待"已派发但未返回"的回调结束（最多内部超时 1s，可由 `ConnectionSpec.extra["sinkDetachTimeoutMs"]` 调整）。
- **线程安全**：可在任意线程析构；与回调派发线程的竞态由库内部锁保证安全。
- **重复析构幂等**：多次 detach() / 析构等价于一次。
- **生命周期重叠**：handle 析构后，该 sink 不再接收新回调；handle 必须在所属 `IDataBridge::close()` 之前析构（与 §3.2.1 "关库前 cancel+wait" 共同约束）。

### 3.3 数据库驱动 SPI（扩展点）

驱动能力之间存在大量真实差异（execBatch 是否真批量、是否支持 ON CONFLICT、是否能在 upsert 后稳定回填主键、是否能给出 per-row 错误、命名 vs 位置占位符、Savepoint 语法），因此 SPI 必须显式声明**能力矩阵**，由上层 Orchestrator 据此挑选执行路径，避免"用统一接口隐藏方言差异"造成的隐式失败。

```cpp
enum class Placeholder { Positional, Named };       // ? vs :name
enum class UpsertSyntax { OnConflict, OnDupKey, Merge, ReplaceIntoBanned };

struct DriverCapabilities {
    UpsertSyntax upsert         = UpsertSyntax::OnConflict;
    bool batchInsertNative      = false;  // 真批量(multi-values / OCI batch) vs 模拟循环
    bool batchUpsertNative      = false;  // batch + upsert 能在单次往返完成
    bool perRowErrorInBatch     = false;  // batch 失败能定位到具体行
    bool returningSupported     = false;  // INSERT ... RETURNING / OUTPUT 子句
    bool generatedKeyOnUpdate   = false;  // upsert 命中 update 分支也能拿到主键
    bool conflictTargetExplicit = true;   // 是否支持显式 conflict columns（MySQL ODKU 不支持）
    bool reportsHitConstraint   = false;  // 多 UK 模糊 upsert 时能稳定报告实际命中的约束名（MySQL ODKU 默认无法）
    bool savepointNative        = true;
    QString savepointSyntax     = "SAVEPOINT %1";  // Oracle/MySQL/SQLite 细节差异
};

class IDatabaseDriver {
public:
    virtual ~IDatabaseDriver() = default;

    virtual DbKind                kind()         const = 0;
    virtual DriverCapabilities    capabilities() const = 0;
    virtual Placeholder           placeholderStyle() const = 0;

    virtual bool    connect(const ConnectionSpec&, QString* err) = 0;
    virtual void    disconnect() = 0;

    // 元数据
    virtual QStringList listTables() = 0;
    virtual TableSchema describe(const QString& table) = 0;

    // 方言：生成 Upsert SQL；conflict 用 UniqueConstraint 表达（见 §7）
    virtual QString    buildUpsertSql(const TableSchema& schema,
                                      const UniqueConstraint& conflict,
                                      const QStringList& updateColumns) = 0;

    // 主键回填能力矩阵（重要：不再假设有"统一 lastInsertedKey"）：
    //   - SQLite：仅 INSERT 分支可用 sqlite_rowid；UPDATE 分支必须 SELECT-back。
    //   - MySQL：LAST_INSERT_ID() 仅对 AUTO_INCREMENT，且 ODKU 走 UPDATE 时需特殊技巧。
    //   - Oracle：MERGE + RETURNING per-row，QtSql 驱动不一定支持，必要时降级 SELECT-back。
    // 上层先选 FK 注入路径（见 §5.3.1）：
    //   ① 业务键 FK → 不调用此函数；
    //   ② 代理键 FK + 父表有业务唯一键 → 调用此函数（驱动可内部走 RETURNING / SELECT-back）；
    //   ③ 代理键 FK 且父表无业务唯一键 且 capabilities.returningSupported=false 且 generatedKeyOnUpdate=false
    //     → Profile 加载阶段拒绝（E_DRIVER_CAPABILITY_MISMATCH），不进入执行。
    virtual bool returnGeneratedKeys(QSqlQuery& q,
                                     const TableSchema& schema,
                                     QVector<QVariant>* outRowKeys,
                                     QString* err) = 0;

    // 事务（详见 §9 状态机）
    virtual bool begin()    = 0;
    virtual bool commit()   = 0;
    virtual bool rollback() = 0;
    virtual bool savepoint(const QString& name)       = 0;
    virtual bool releaseSavepoint(const QString& name)= 0;
    virtual bool rollbackToSavepoint(const QString& name) = 0;

    // 原生错误码分类，便于上层决定是否可重试
    virtual ErrorCategory classifyError(const QSqlError& e) = 0;
};

enum class ErrorCategory {
    UniqueViolation, FkViolation, NotNullViolation,
    Deadlock, Timeout, Serialization,
    ConnectionLost, ConstraintOther, Unknown
};

// 驱动注册：宿主可自行注册自定义驱动
DBRIDGE_API void registerDriver(DbKind kind,
                                std::function<std::unique_ptr<IDatabaseDriver>()> factory);
```

> SQLite 驱动随库内置；MySQL / Oracle 驱动可后续以独立子库或宿主侧实现并通过 `registerDriver` 注入。Orchestrator 在 Profile 加载阶段做"路径可达性检查"——对每个需要 generated-key 的子表 FK，按 §5.3.1 的三档路径选择能落地的一条；若三档都不可达（业务键无 / RETURNING 无 / update 分支回填无 / SELECT-back 也无父表唯一键支撑），返回 `E_DRIVER_CAPABILITY_MISMATCH`。仅在这种"路径全断"时才拒绝执行。

---

## 4. 配置文件（Profile）模型

业务变化最频繁的路由 / 映射 / 校验规则放在外置 JSON，避免硬编码。

### 4.1 Profile 结构（示意）

```jsonc
{
  "profileName": "order_intake_v1",
  "schemaVersion": "1.0",            // Profile JSON Schema 版本，向后兼容
  "sheet": "Sheet1",
  "headerRow": 1,
  "skipBlankRows": true,
  "mergedCellPolicy": "propagate",   // propagate | first-only | error
  "discriminator": {
    "type": "column",                 // column | regex | composite
    "column": "Type"                  // 鉴别列（按表头名定位，而非绝对单元格）
  },
  "routing": {
    "strategy": "firstMatch",         // firstMatch | uniqueMatch（多重匹配→错误）
    "onNoMatch": "reject",            // reject | default:<classId>
    "ambiguityCode": "E_ROUTE_AMBIGUOUS"
  },
  "classes": [
    {
      "id": "A",
      "match": { "equals": "Alpha" },
      "routes": [
        {
          "table": "m1",
          "conflict": {
            "constraintName": "uk_m1_order",
            "columns": ["order_no"],
            "onMultipleUniqueIndex": "strictOrFail"   // strictOrFail | bestEffortWarn
          },
          "columns": {
            "order_no":  {
              "source": { "kind": "header", "name": "OrderNo" },
              "convert": ["trim"],
              "validators": ["notNull", "len<=32"]
            },
            "customer":  { "source": { "kind": "header", "name": "Customer" } },
            "amount":    {
              "source": { "kind": "header", "name": "Amount" },
              "convert": ["decimal:12,2"],
              "validators": ["decimal(12,2)"]
            }
          },
          "exportKey": true
        },
        {
          "table": "m2",
          "parent": "m1",
          "cardinality": "oneToMany",                 // 见 §5.4
          "fkInject": [{ "from": "m1", "pairs": [["order_no","order_no"]] }],
          "conflict": { "constraintName": "pk_m2", "columns": ["order_no","line_no"] },
          "columns": {
            "line_no":   { "source": { "kind": "header", "name": "LineNo" } },
            "sku":       { "source": { "kind": "header", "name": "Sku" },
                           "validators": ["regex:^[A-Z0-9-]{3,16}$"] },
            "qty":       { "source": { "kind": "header", "name": "Qty" },
                           "validators": ["int>=1"] }
          }
        }
      ]
    },
    { "id": "B", "match": { "equals": "Beta" },  "routes": [ /* n 集合 */ ] },
    { "id": "C", "match": { "regex": "^\\d+$", "column": "Code" }, "routes": [ /* o 集合 */ ] }
  ],
  "export": {
    "ordering": [{ "key": "created_at", "dir": "asc" }],
    "cardinalityOnExport": "expandRows", // expandRows | concatCells | nestedJson
    "styles":   { "A": "fillBlue", "B": "fillGreen", "C": "fillOrange" }
  }
}
```

#### 4.1.1 列定位语义

列引用统一通过 `source` 对象表达，避免与"绝对单元格 A1/B3"混淆：
| `source.kind` | 含义 |
|---|---|
| `header` | 按表头名查找当前行对应列；列不存在时按 `missingPolicy` 处理 |
| `colIndex` | 按 0/1-based 列号定位（明确指明 base） |
| `literal` | 字面常量（用于补默认值） |
| `expr` | 简单表达式（`${OrderNo} + "-" + ${LineNo}`，受限语法） |

每列还可声明：`missingPolicy: required|optional|default:<v>`、`convert:[trim, upper, decimal:p,s, date:fmt]`、`nullable: bool`。

#### 4.1.2 路由冲突与歧义处理

- `routing.strategy=firstMatch`：按 `classes` 顺序首个命中；
- `routing.strategy=uniqueMatch`：多个 class 同时命中视为错误 `E_ROUTE_AMBIGUOUS`；
- `routing.onNoMatch=reject`：未匹配行抛 `E_ROUTE_UNMATCHED`；
- `routing.onNoMatch=default:<classId>`：归入默认 class；
- 隐藏行/合并单元格按全局 `mergedCellPolicy` 与每列 `missingPolicy` 共同决定。

#### 4.1.3 Profile JSON Schema

完整 JSON Schema 与示例放在 `tests/data/profile-schema.json`，Loader 加载阶段强校验；任何未声明字段视为错误，避免笔误静默放行。

### 4.2 校验器目录（内置可扩展）
- 结构层：`notNull` / `unique` / `len<=N` / `len>=N`
- 词法层：`int` / `decimal(p,s)` / `date(fmt)` / `regex:pattern` / `enum[..]`
- 关联层：`fkExists:table.column` / `refEqualsOnUpdate`

> Validation Engine 通过策略模式（Strategy）+ 注册表加载，宿主可注册自定义校验器（开闭原则）。

---

## 5. 关键流程

### 5.1 异步导入主流程

```
 Host                ImportOrchestrator       Excel I/O       Validation         Routing       SQL/Upsert        DB
  │   importExcel()       │                      │                │                │              │              │
  │──────────────────────▶│                      │                │                │              │              │
  │     Job handle        │                      │                │                │              │              │
  │◀──────────────────────│                      │                │                │              │              │
  │                       │  refreshSchema()     │                │                │              │              │
  │                       │─────────────────────────────────────────────────────────────────────▶│ PRAGMA/IS    │
  │                       │◀─────────────────────────────────────────────────────────────────────│ TableSchema  │
  │                       │  openSheet()         │                │                │              │              │
  │                       │─────────────────────▶│                │                │              │              │
  │                       │                      │ rows (chunk N) │                │              │              │
  │                       │◀─────────────────────│                │                │              │              │
  │   loop per chunk      │                      │                │                │              │              │
  │                       │  validate(rows)      │                │                │              │              │
  │                       │─────────────────────────────────────▶│                │              │              │
  │                       │   errors / pass      │                │                │              │              │
  │                       │◀─────────────────────────────────────│                │              │              │
  │  ─── if row errors: IAlertSink::onRowError(逐条) + onBatchAborted(致命) → ABORT ─── │    │      │              │
  │                       │  classify + slice    │                │                │              │              │
  │                       │────────────────────────────────────────────────────▶  │              │              │
  │                       │     RowPayloads      │                │                │              │              │
  │                       │◀────────────────────────────────────────────────────  │              │              │
  │                       │  buildUpsert + execBatch (per table, topo order)      │              │              │
  │                       │─────────────────────────────────────────────────────────────────────▶│ SAVEPOINT s1 │
  │                       │                                                                       │ Upsert m1    │
  │                       │                                                                       │ FK inject    │
  │                       │                                                                       │ Upsert m2..  │
  │                       │◀─────────────────────────────────────────────────────────────────────│ OK / ERR     │
  │                       │  commit chunk / progress(p%)                                          │              │
  │  progress(p%)         │                                                                       │              │
  │◀──────────────────────│                                                                       │              │
  │   end of file → COMMIT job, emit done                                                         │              │
```

要点（与 §9 事务状态机严格对应）：
- **三种事务模式**（`TxMode`）：
  - `AllOrNothing`（默认强一致）：整 Job 一个 BEGIN；chunk 失败 → `ROLLBACK TO SAVEPOINT` → 整 Job 终止并 `ROLLBACK`。
  - `ChunkCommit`：每 chunk `BEGIN ... COMMIT`，失败 chunk 单独 `ROLLBACK`；之前成功 chunk 已落盘。
  - `ContinueOnError`：行级错误进入 AlertSink，不阻断；chunk 边界仍按 `ChunkCommit` 落盘。注意：与 §1.1 的"导入前校验出错即终止"硬约束**只对 L1/L2 校验失败成立**（`abortOnPreValidate=true` 强制）；L3 DB 约束失败才是 `ContinueOnError` 真正放行的对象。
- **Savepoint ≠ Commit**：Savepoint 仅是"批内回退点"，不会让数据对其他事务可见；事务可见性由模式决定。
- **流水线**：Excel Reader → 校验 → 路由切片是并行的；但 **DB 写入连接强制单线程**（QtSql/QSqlQuery 有线程亲和性，SQLite 也只能单写者，详见 §11.1）。

### 5.2 校验拦截流程（三层串联）

```
 Excel Row(QVariantMap)
        │
        ▼
 ┌─────────────────┐    fail   ┌─────────────────────┐
 │ L1 Schema/Lex   │──────────▶│  AlertSink          │
 │ notNull/regex/  │           │  (sheet,row,col,    │
 │ range/type      │           │   value,reason)     │
 └────────┬────────┘           └─────────────────────┘
          │ pass                          ▲
          ▼                               │ aggregated
 ┌─────────────────┐    fail              │
 │ L2 Relation     │──────────────────────┤
 │ FK / uniqueness │                       │
 │ (mem-hashed)    │                       │
 └────────┬────────┘                       │
          │ pass                           │
          ▼                                │
 ┌─────────────────┐    fail (rare:        │
 │ L3 DB Constraint│   collation/trigger)  │
 │ caught after    │──────────────────────┘
 │ execBatch       │
 └────────┬────────┘
          │ ok
          ▼
       Commit chunk
```

要点：
- **L2 拆三段**：
  1. **批内精确校验**：当前 chunk 内对 PK/UK/FK 做精确去重与引用校验（`QHash<QByteArray, RowAddress>`）；
  2. **DB 快照校验**：批前从 DB 按需 SELECT 当前 chunk 涉及的键集合到内存（"按需"而非"全量预热"，避免百万级 PK 全量加载 OOM）；
  3. **DB 约束兜底（L3）**：execBatch 后的 `QSqlError` 翻译为业务错误码并按 `TxMode` 处理。
- **Bloom Filter 仅作快速提示**：用于"明显未冲突"的快速放行；命中"可能冲突"时仍走精确 SELECT。**Bloom 假阳性绝不直接拒写**。
- **L3 处理**：捕获 `QSqlError` → `IDatabaseDriver::classifyError` → 按 `ErrorCategory` 决定 retry/abort/skip。
- **并发安全**：L2 内存集合在并发写场景（同一 DB 多写者）下并非可靠去重，最终去重靠 DB 约束。

### 5.3 异构单 Sheet 正向路由

```
   Excel Sheet1
   ┌────────────────────────────────────┐
   │ Type=Alpha  ... A 行                │──▶ Classifier (Discriminator)
   │ Type=Beta   ... B 行                │        │
   │ Code=12345  ... C 行                │        ▼
   │ Type=Alpha  ... A 行                │   ┌──────────────────┐
   │ ...                                 │   │ RouteRule[A]: m  │──▶ Slicer ──▶ [m1 payload, m2 payload, m3 payload]
   └────────────────────────────────────┘   │ RouteRule[B]: n  │──▶ Slicer ──▶ [n1, n2]
                                            │ RouteRule[C]: o  │──▶ Slicer ──▶ [o1, o2, o3, o4]
                                            └──────────────────┘
                                                     │
                                                     ▼
                                            ┌──────────────────┐
                                            │ Topo Sorter      │  按 parent / fkInject 拓扑排序
                                            │ m1 → m2 → m3     │
                                            │ n1 → n2          │
                                            │ o1 → o2,o3 → o4  │
                                            └──────┬───────────┘
                                                   ▼
                                            Upsert Executor (per table, batched)
                                                   │
                                                   ▼
                                            DB (m / n / o tables, transactional)
```

#### 5.3.1 FK 注入策略（主键回填能力差异化）

跨方言的 generated-key 回填能力不一致（详见 §3.3 capabilities），FK 注入按优先级落地：

1. **业务键优先**（默认推荐）：父子表都暴露相同业务键（如 `order_no`），FK 直接使用业务键，**无需依赖任何 generated key**。Profile 中 `fkInject.from.column` 指向父表的业务键列。
2. **代理键回填路径**（仅在驱动能力满足时启用）：
   - SQLite + INSERT 分支 → `sqlite_rowid`；
   - 驱动 `capabilities().returningSupported=true` → `INSERT ... RETURNING id`；
   - 驱动 `generatedKeyOnUpdate=true` → upsert 命中 update 时也能拿到键；
   - 都不满足 → **SELECT-back**：upsert 后按业务键 SELECT 取回代理主键到内存映射表。
3. **不可达组合直接拒绝**：Profile 要求代理键 FK，但驱动既不支持 RETURNING 也不支持 update 分支回填，且业务键无 UNIQUE 约束 → 加载 Profile 时报 `E_DRIVER_CAPABILITY_MISMATCH`，不进入执行。

实施约束：父子表写入按拓扑顺序串行（同一 chunk 内），父表 chunk 完成 → 构建 `(business_key → surrogate_key)` 映射 → 子表使用该映射注入 FK；映射作用域限定于当前 Job，跨 Job 不复用。

### 5.4 异构反向导出聚合

```
        ┌─────────────┐   LEFT JOIN per class
   m1───▶│  Aggregator │◀─── m2,m3        ─┐
        └─────────────┘                    │
        ┌─────────────┐                    │   ┌──────────────────┐
   n1───▶│  Aggregator │◀─── n2            │──▶│  Row Interleaver │──▶ Sheet1
        └─────────────┘                    │   │  by ordering key │
        ┌─────────────┐                    │   └──────────────────┘
   o1───▶│  Aggregator │◀─── o2,o3,o4      ─┘
        └─────────────┘
```

- 每类生成一个反规范化 SELECT（多 LEFT JOIN）；
- `Row Interleaver` 按 Profile 中 `export.ordering` 对所有类记录归并；
- 写出阶段按行类型施加不同单元格样式（A 蓝 / B 绿 / C 橙）。

#### 5.4.1 多对一/一对多的 cardinality 处理

cardinality 在两处出现，含义不同但必须配套：

| 出现位置 | 角色 | 取值 |
|---|---|---|
| `routes[].cardinality`（Profile 路由侧，§4.1） | **声明**父子表关系基数 | `oneToOne` / `oneToMany` |
| `export.cardinalityOnExport`（Profile 导出侧，§4.1） | **执行**导出时如何把多行展平回单行 | `expandRows` / `concatCells` / `nestedJson` |

Orchestrator 在加载 Profile 时校验：`oneToMany` 子路由的导出策略不允许是 `expandRows` 之外的设置**与父表唯一性冲突**（如 `concatCells` 要求父表业务键唯一）。
内部 `RowPayload` 不承载 cardinality（属于 RouteRule 元数据，不进入行级数据），但写入阶段会读取 `RouteRule.cardinality` 决定 FK 注入是否要按"父行 → 多子行"展开。

LEFT JOIN 在子表存在多行时会**行数放大**（父行重复），导出阶段按 `cardinalityOnExport` 处理：

| `cardinalityOnExport` | 行为 | 适用 |
|---|---|---|
| `expandRows` | 父行按子行数复制，子表空缺时仍输出 1 行（NULL 占位）→ 与正向"宽行"语义对称 | 默认；正向导入是宽行展开多表的逆操作 |
| `concatCells` | 同父行的多条子记录在一列内用分隔符拼接 | 子表是属性集合而非独立实体 |
| `nestedJson` | 子表打包为 JSON 写入单格 | 高级场景，需对端能解析 |

排序键 `export.ordering` 要求"对每个 class 都解析得到"，否则按 `class.id` + 物理顺序兜底，确保结果**确定性**。空子表行为：`expandRows` 输出 1 行 NULL；`concatCells/nestedJson` 输出空字符串。

#### 5.4.2 Round-trip 一致性

导入 → 导出 → 再导入应得到等价 DB 状态（在排序与样式上允许差异）。为此：
- 导入时记录每行的 `routeSignature`（class id + route hash），导出时按相同 signature 回放；
- Profile 提供 `roundTripStrict` 开关，开启后导出阶段强校验所有列都有逆向映射，否则报 `E_PROFILE_NOT_ROUND_TRIPPABLE`。

### 5.5 面向未来未知表的自省导入

```
 Host: bridge->generateAutoProfileDraft("future_tableX")
        │
        ▼
 SchemaIntrospector.refresh()
        │   SELECT name FROM <catalog>            (SQLite: sqlite_master / MySQL: INFORMATION_SCHEMA)
        │   PRAGMA table_info(t)                  (or DESCRIBE / ALL_TAB_COLUMNS)
        ▼
 DataDictionary[QHash<QString, TableSchema>]
        │
        ▼
 自动构造 ProfileDraft（header 名 == 列名）：
   - RouteRule：table=headerSheet，conflictTarget = PK 或首个 UniqueConstraint
   - ColumnInfo → LogicalType 映射（见 §7）
   - ON CONFLICT DO UPDATE SQL（仅由 SqlBuilder + Driver SPI 完成）
        │
        ▼  draft.executable == true（§5.5.1 全部硬条件满足）
 Host 选其一：
   ① 落盘草稿 JSON → bridge->loadProfile("draft.json")
   ② 直接内存加载 → bridge->loadProfileFromString(draft.profileJson)
        │
        ▼
 bridge->importExcel("future_tableX_dump.xlsx", profileName) → 走通用 Import Pipeline，无需重编译
```

> draft.executable == false 时：Host 必须先按 `draft.issues` 修订草稿，再走 ①/② 任一加载路径；不存在"importExcel 直接吃 'auto' 这个 magic profile"的隐式行为。

#### 5.5.1 auto profile 适用范围（硬限制）

auto profile **不是万能**，必须满足以下所有条件才会生效，否则回退为"候选 profile + 人工确认"模式（生成可编辑的 JSON 草稿，由用户审阅后保存为命名 profile）：

| 条件 | 不满足时的行为 |
|---|---|
| 单表导入（一个 Sheet → 一个 Table） | 多表/多 class 必须显式 Profile |
| 表头列名与 DB 列名（去空白后）一一对应 | 任何不匹配 → 生成草稿并标红冲突 |
| 表存在至少一个 PRIMARY KEY 或 UNIQUE 约束 | 草稿仍可生成（`executable=false`），但 `issues` 标记 `E_AUTO_NO_UNIQUE`（Fatal）；后续即使 Host 手动 `loadProfile` 执行 upsert，也会在 SqlBuilder 阶段拒绝（与 §1.3 "不直接拒绝草稿"协同：拒绝的是 upsert 执行，不是草稿生成） |
| 表无 identity/generated/expression 列 或 这些列允许由 DB 自动填充 | 生成草稿并要求人工确认默认值策略 |
| 表无外键 或 外键引用对象已经存在 | 含 FK 拓扑 → 生成草稿 |
| 列名不与方言保留字冲突 | 生成草稿并提示加引号策略 |

实现层面：`SchemaIntrospector` 提供 `autoProfileCandidate(table)` 返回 `ProfileDraft`（见 §7.1），由 `IDataBridge::generateAutoProfileDraft` 暴露给 Host；Host 决定是修订后保存 + `loadProfile` 走通用路径，还是放弃。`loadProfile` 自身**只接受已确认的命名 Profile**，不会"内部决定是否执行草稿"，避免接口职责模糊。

---

## 6. 关键设计模式映射

| 设计点 | 模式 | 用途 |
|---|---|---|
| `IDatabaseDriver` 厂商扩展 | **Strategy + Abstract Factory** | 不同 DB 方言可插拔 |
| 校验器目录 | **Strategy + Registry** | 内置 + 宿主自定义 |
| 行分类 | **Chain of Responsibility** | Discriminator 链路 |
| 多表拓扑写入 | **Pipeline + Topological Sort** | 主从外键有序注入 |
| Public API | **Facade + PImpl** | 隐藏实现（**注意：不等于二进制 ABI 稳定**，详见 §3.0） |
| Job 句柄 | **Command + Observer** | 异步任务 + 信号通知；生命周期见 §3.2.1 |
| 反向聚合 | **Builder** | 动态拼 JOIN / SELECT |
| 告警通道 | **Observer** | `IAlertSink` 解耦 UI |
| 元数据 | **Lazy Initialization + Cache** | 首次访问探测 + 失效刷新 |

---

## 7. 数据结构（核心类型）

```cpp
// 本库自有的稳定 logical type，避免 QVariant::Type（Qt6 已转 QMetaType）成为 ABI 包袱
enum class LogicalType {
    Null, Bool, Int32, Int64, Double, Decimal, String,
    Date, Time, DateTime, Blob, Json
};

struct ColumnInfo {
    QString      name;
    QString      declaredType;       // 原始声明（如 "DECIMAL(12,2)"）
    LogicalType  logicalType = LogicalType::String;
    int          precision = 0;       // 仅 Decimal 有效
    int          scale     = 0;
    bool         notNull   = false;
    bool         isPk      = false;
    bool         isGenerated = false; // 生成列/计算列
    QVariant     defaultValue;
};

// 唯一约束模型（PK 也用同一结构表达，name 形如 "<PRIMARY>"）
struct UniqueConstraint {
    QString      name;                // 约束名 / 索引名
    QStringList  columns;             // 列序敏感
    bool         isPrimary  = false;
    bool         isPartial  = false;  // 部分索引（SQLite/PG 支持）
    QString      partialWhere;        // 部分索引的 WHERE 表达式（原样保留）
    QString      expression;          // 表达式索引（如 lower(col)）
    QString      collation;           // 整索引或列级 collation
    bool         usableAsUpsertTarget = true; // false：方言不允许作为 ON CONFLICT 目标
};

struct ForeignKey {
    QStringList localCols;
    QString     refTable;
    QStringList refCols;
    QString     onUpdate;   // CASCADE | RESTRICT | SET NULL | NO ACTION
    QString     onDelete;
};

struct TableSchema {
    QString                  table;
    QList<ColumnInfo>        columns;
    UniqueConstraint         primaryKey;        // 复合主键也走这里
    QList<UniqueConstraint>  uniqueConstraints; // 表达力 > 单一 QStringList
    QList<ForeignKey>        foreignKeys;
};

struct RowAddress { int sheetIndex; int row; int col; };

struct RowError {
    RowAddress where;
    QString    column;
    QString    rawValueRepr;  // 字符串化的原值，避免 QVariant 跨 ABI
    QString    code;          // E_VALIDATE_REGEX / E_FK_MISSING / E_DB_UNIQUE ...
    QString    message;       // 已国际化
};

struct RowPayload {
    QString                  table;
    QVariantMap              values;       // 列名 → 值（内部使用）
    UniqueConstraint         conflictTarget; // Upsert 冲突约束（替代旧 conflictKeys 列名串）
    QString                  parentTable;  // 拓扑依赖（可空）
    QHash<QString,QString>   fkInject;     // parent.col → this.col
};
```

> 设计动机：`LogicalType` 与字符串化原值并非为了 Qt6 迁移（本期 Qt 锁定 5.12.12），而是为了避免 `QVariant::Type` 这种带历史包袱的类型出现在公共 ABI 上，简化错误码 / 行错误的跨编译器互通。

#### 7.1 ProfileDraft（auto profile 草稿）

```cpp
struct ProfileIssue {
    QString    code;       // E_AUTO_NO_UNIQUE / W_AUTO_RESERVED_WORD 等
    Severity   severity;
    QString    location;   // "table.col" / "schema"
    QString    message;    // 人工修订指引
};

struct ProfileDraft {
    QString               profileJson;   // 序列化后的候选 Profile（可由人工编辑后保存）
    bool                  executable = false;   // §5.5.1 所有硬条件满足时为 true
    QList<ProfileIssue>   issues;        // 不满足项 / 警告，severity 决定阻塞性
};
```

执行约束：`executable=false` 时 `IDataBridge` 不会自动加载该草稿；Host 必须人工修订 `profileJson` 后通过 `loadProfile`（落盘的 JSON 路径）或 `loadProfileFromString`（直接传 JSON 字符串）显式加载。`executable=true` 时 Host 可选择落盘并 `loadProfile`，或直接 `loadProfileFromString(draft.profileJson)`。两条路径走完全相同的 Schema 校验与执行流水线。

---

## 8. SQL 生成与 Upsert 语义（方言隔离）

### 8.1 SQLite 方言（默认）
```sql
INSERT INTO m1 (order_no, customer, amount)
VALUES (?, ?, ?)
ON CONFLICT(order_no) DO UPDATE SET
  customer = excluded.customer,
  amount   = excluded.amount;
```
- `ON CONFLICT(...)` 列必须**精确匹配**一个 PRIMARY KEY 或 UNIQUE 约束的列集合（不是任意 UNIQUE 索引），否则 SQLite 报错。
- 缺约束 / 不匹配时 SqlBuilder 抛 `E_PROFILE_NO_CONFLICT_KEY`，编排器停止任务并告警。

### 8.2 MySQL 方言（未来）
```sql
INSERT INTO m1 (...) VALUES (...)
ON DUPLICATE KEY UPDATE customer=VALUES(customer), amount=VALUES(amount);
-- MySQL 8.0.20+ 推荐 alias 写法，避免 VALUES() 的弃用警告：
-- INSERT INTO m1 (...) VALUES (...) AS new
--   ON DUPLICATE KEY UPDATE customer = new.customer, amount = new.amount;
```
**重要差异**：
- ODKU **不接受 conflict target**，触发的是表中**任意** UNIQUE/PRIMARY 冲突；当表上有多个唯一索引时，任意一个冲突都会走 UPDATE 分支，与 Profile 的 `conflict.columns` 语义**不一致**。
- 因此 MySQL 驱动的 `capabilities().conflictTargetExplicit = false`；Orchestrator 在 Profile 加载阶段检测表上 UNIQUE 约束数：
  - 若仅 1 个 UNIQUE/PK：无歧义，正常执行。
  - 若 >1 个，且 Profile 的 `conflict.onMultipleUniqueIndex = "strictOrFail"`（默认）：抛 `E_MYSQL_AMBIGUOUS_UPSERT_TARGET`（5005），**拒绝执行**。
  - 若 >1 个，且设置为 `"bestEffortWarn"`：发出警告 `W_AMBIGUOUS_UPSERT_TARGET` 并继续；审计信息记录"Profile 期望的 conflict 约束 + 表上所有候选唯一约束清单 + ODKU 执行后 `ROW_COUNT()` 返回的 1（INSERT）/ 2（UPDATE）/ 0（无变化）分支区分"，**不承诺**能给出 MySQL 实际命中的具体约束名（驱动层面无稳定方式获得）；未来某驱动若声明 `capabilities().reportsHitConstraint=true`，再升级为精确命中。
- 测试矩阵必须覆盖多唯一索引场景的两种策略。
- 目标 MySQL 版本通过 `ConnectionSpec.extra["mysqlVersion"]` 显式指定，驱动据此切换 `VALUES()` 与 alias 写法。

### 8.3 Oracle 方言（未来）
```sql
MERGE INTO m1 USING (SELECT ? order_no, ? customer, ? amount FROM dual) s
ON (m1.order_no = s.order_no)
WHEN MATCHED THEN UPDATE SET m1.customer = s.customer, m1.amount = s.amount
WHEN NOT MATCHED THEN INSERT (order_no,customer,amount) VALUES (s.order_no,s.customer,s.amount);
```
**并发安全**：MERGE **不免疫**并发竞态。并发会话同时 INSERT 同一唯一键时仍可能触发 ORA-00001（唯一约束冲突），高并发下也可能出现 ORA-08177（serializable）或死锁。驱动必须：
- 显式声明事务隔离级别（默认 `READ COMMITTED`）；
- 提供"唯一冲突 → 重试 N 次（带指数退避） → 仍失败则按 `TxMode` 落入失败处理"的策略，重试次数与退避通过 `ImportOptions.extra` 暴露；
- `classifyError` 将 ORA-00001 / ORA-00060 / ORA-08177 分别归类，避免非幂等操作被错误重试。

> 全部由 `IDatabaseDriver::buildUpsertSql()` 内部完成，上层与方言**语义差异**通过 `capabilities()` 显式暴露，避免"用统一接口隐藏方言陷阱"。

---

## 9. 事务与回滚策略

### 9.1 模式语义

| `TxMode` | BEGIN 边界 | chunk 失败 | Job 终止行为 | 适用 |
|---|---|---|---|---|
| `AllOrNothing` | Job 头 BEGIN，Job 尾 COMMIT | `ROLLBACK TO SAVEPOINT` → 取消整 Job 的剩余 chunk → 最终 `ROLLBACK` | 一处失败全部回滚 | 强一致导入（默认） |
| `ChunkCommit` | 每 chunk BEGIN / COMMIT | 当前 chunk `ROLLBACK`；前序已 COMMIT 保留 | 部分成功 + 失败清单 | 大文件容错导入 |
| `ContinueOnError` | 每 chunk BEGIN / COMMIT | L3 行级失败按 capability 决定（perRowErrorInBatch）skip 或拆批 | 部分成功 + 错误清单 | 数据清洗类场景 |

约束：`abortOnPreValidate=true`（默认）下，L1/L2 任何一条失败都**直接终止 Job**，不进入执行阶段，与上述任一模式都正交。

### 9.2 状态机

> 终态严格对齐 §3.2.1 `JobState`：仅 `Succeeded` / `PartialSuccess` / `Failed` / `Cancelled` 四种。预校验失败属于 `Failed`（不是单独的"Aborted"）。

```
            ┌─────────┐
            │  Init   │
            └────┬────┘
        loadProfile / refreshSchema
                 ▼
            ┌─────────┐    abortOnPreValidate fail
            │ PreRun  │───────────────────────────────► Failed (E_VALIDATE_*)
            └────┬────┘
       BEGIN (AllOrNothing)  /  no-op
                 ▼
            ┌─────────────┐  for each chunk：
            │ ChunkLoop   │   ① BEGIN (ChunkCommit/ContinueOnError)
            └────┬────────┘   ② SAVEPOINT s_n
                 │            ③ execBatch
                 │            ④ 成功 → RELEASE / COMMIT
                 │            ⑤ 失败：见下方分支
                 │            ⑥ 任意时刻收到 cancel() → 跳出循环走 Finalize
       ┌─────────┼───────────────────────────────┐
       ▼         ▼                               ▼
ROLLBACK TO    (AllOrNothing) → ROLLBACK Job   (ChunkCommit) → ROLLBACK chunk → 继续
SAVEPOINT      (ContinueOnError) → 按 capability 处理：
                  - perRowErrorInBatch=true：跳过失败行 RELEASE
                  - 否则：拆批二分定位失败行，其余行重做
                 ▼
            ┌─────────┐
            │ Finalize│ → COMMIT (AllOrNothing) / 已逐 chunk 提交（其他）
            └────┬────┘
                 ▼
   ┌────────────────────────────────────────────────────────┐
   │  终态选择规则                                          │
   │  - 收到 cancel() 且已开始执行：Cancelled               │
   │  - 失败 0 / 成功 >0：Succeeded                         │
   │  - 失败 >0 且仍有成功（ChunkCommit/ContinueOnError）： │
   │    PartialSuccess                                      │
   │  - 失败 >0 且零成功 或 AllOrNothing 整体回滚：Failed   │
   └────────────────────────────────────────────────────────┘
```

#### 9.2.1 cancel() 的收尾规则（按 TxMode）

| 状态阶段 | `AllOrNothing` | `ChunkCommit` | `ContinueOnError` |
|---|---|---|---|
| `Pending`（尚未 BEGIN） | 直接进入 `Cancelled`，无 DB 动作 | 同左 | 同左 |
| `PreRun`（已 BEGIN，未跑 chunk） | `ROLLBACK` → `Cancelled` | （未 BEGIN，无动作） → `Cancelled` | 同 ChunkCommit |
| `ChunkLoop` 内 chunk 进行中 | 当前 chunk `ROLLBACK TO SAVEPOINT` → 整 Job `ROLLBACK` → `Cancelled`（**不 COMMIT**） | 当前 chunk `ROLLBACK`；已 COMMIT 的 chunk 保留 → `Cancelled` | 同 ChunkCommit |
| chunk 之间（前 chunk 已完成，下一个未启动） | 整 Job `ROLLBACK` → `Cancelled` | 已 COMMIT 保留 → `Cancelled` | 同 ChunkCommit |
| 已进入 `Finalize` 且 `COMMIT` 已发出 | cancel 视为 no-op（不可撤销） | 同左 | 同左 |

关键约束：
- **`AllOrNothing` 取消 = 全量回滚**，绝不 COMMIT 部分数据；这是该模式的强一致承诺。
- **`ChunkCommit` / `ContinueOnError` 取消 = 保留已 COMMIT、回滚当前活动 chunk**；终态附带 `okRows`、`failedRows` 给 Host 评估。
- cancel 收尾期间的状态为 `Cancelling`，回滚完成后翻转到终态 `Cancelled`；这期间 `cancel()` 重复调用幂等。

### 9.3 QtSql 与 Savepoint 边界

- `QSqlDatabase::transaction()` **不支持嵌套**，本库**不使用**它；BEGIN/COMMIT/ROLLBACK 全部由驱动 SPI 通过 `QSqlQuery` 直接执行 SQL，便于精确控制 Savepoint。
- Savepoint 命名：`dbridge_sp_<jobId>_<chunkIdx>`，避免与宿主已有 savepoint 冲突。
- Savepoint 语法差异由 `IDatabaseDriver::savepoint` 屏蔽：
  - SQLite：`SAVEPOINT n; RELEASE SAVEPOINT n; ROLLBACK TO SAVEPOINT n;`
  - MySQL：基本相同；
  - Oracle：`SAVEPOINT n;` + `ROLLBACK TO SAVEPOINT n;`，但**没有 RELEASE**（commit 时随事务统一释放），驱动 `releaseSavepoint` 在 Oracle 上为 no-op。
- `QSqlDatabase` 与 `QSqlQuery` 有线程亲和性：写连接绑定到唯一 Worker 线程，全部 Savepoint/Upsert 在该线程串行执行。

### 9.4 SQLite 专项

- `PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;`：让"读不阻塞写、写不阻塞读"，但仍是**单写者**（详见 §11.1）。
- `PRAGMA busy_timeout=<ms>`：避免短暂锁等返回 SQLITE_BUSY。
- WAL 文件可能因长事务膨胀：`AllOrNothing` 模式下大文件需评估磁盘；必要时切换 `ChunkCommit` 让 checkpoint 推进。
- 崩溃恢复：依赖 SQLite 自身 journal；本库不维护外部 redo log。

---

## 10. 错误码与告警

### 10.1 错误码 Registry

错误码作为公共 ABI 的一部分，**冻结后不得改名或改值**；新错误码必须分配到保留区间，按下表分类：

| 区间 | 类别 | 严重性默认 | 可重试 |
|---|---|---|---|
| 1000-1999 | 资源/IO（DB/XLSX 打开、网络） | Fatal | 部分 |
| 2000-2999 | Profile 解析 / 配置 | Fatal | 否 |
| 3000-3999 | 校验 L1/L2（结构/词法/关联） | Error | 否 |
| 4000-4999 | DB 执行 / L3（含原生错误透传） | Error | 由 `classifyError` 决定 |
| 5000-5999 | 拓扑 / 路由 / 能力不匹配 | Fatal | 否 |
| 6000-6999 | 内部 Bug / 不变量违反 | Fatal | 否 |
| 7000-7999 | 警告（W_）：不阻断但需记录 | Warning | n/a |

错误对象结构：

```cpp
enum class Severity {
    Info,      // 信息
    Warning,   // 不阻断
    Error,     // 当前操作失败但可被上层模式覆盖（如 ContinueOnError 跳过）
    Fatal      // 不可恢复，必然终止 Job
};

struct ErrorInfo {
    int        numericCode = 0; // 稳定数值；0 表示无错误
    QString    symbolicCode;    // E_PROFILE_NO_CONFLICT_KEY 等
    Severity   severity   = Severity::Error;
    bool       retryable  = false;
    QString    category;        // 见 §3.3 ErrorCategory 字符串化
    int        nativeCode = 0;  // 驱动原生错误号
    QString    nativeMessage;   // 驱动原生消息
    QString    message;         // i18n 后的业务级提示
};
```

错误码常量定义在 `Errors.h`，分为 **public stable**（暴露给宿主）与 **internal**（仅日志使用）两区；public 区改动须走 MINOR 版本。

错误码示例：
- 1000 `E_OPEN_DB` / 1001 `E_OPEN_XLSX`
- 2000 `E_PROFILE_PARSE` / 2001 `E_PROFILE_NO_CONFLICT_KEY` / 2002 `E_PROFILE_UNKNOWN_VALIDATOR` / 2003 `E_PROFILE_NOT_ROUND_TRIPPABLE`
- 3000 `E_VALIDATE_NULL` / 3001 `E_VALIDATE_REGEX` / 3002 `E_VALIDATE_RANGE` / 3003 `E_VALIDATE_TYPE` / 3010 `E_FK_MISSING`
- 4000 `E_DB_NATIVE` / 4001 `E_UNIQUE_CONFLICT` / 4002 `E_DEADLOCK`（可重试）
- 5000 `E_TOPOLOGY_CYCLE` / 5001 `E_ROUTE_AMBIGUOUS` / 5002 `E_ROUTE_UNMATCHED` / 5003 `E_DRIVER_CAPABILITY_MISMATCH` / 5004 `E_AUTO_NO_UNIQUE` / 5005 `E_MYSQL_AMBIGUOUS_UPSERT_TARGET`
- 7000 `W_AMBIGUOUS_UPSERT_TARGET` / 7001 `W_MISSING_INDEX`

### 10.2 告警通道
```cpp
class IAlertSink {
public:
    virtual ~IAlertSink() = default;
    virtual void onRowError(const RowError&)                   = 0;
    virtual void onWarning(const ErrorInfo&)                   = 0;
    virtual void onBatchAborted(const ErrorInfo&)              = 0;
    virtual void onCompleted(JobState finalState,
                             int okRows,
                             int failedRows,
                             const ErrorInfo& lastError /*可空*/) = 0;
};
```
- `finalState` ∈ { `Succeeded`, `PartialSuccess`, `Failed`, `Cancelled` }，与 `IImportJob::state()` 终态严格一致。
- `lastError` 仅在 `Failed` / `PartialSuccess` 时有意义；`Succeeded` 时其 `numericCode == 0`。
宿主可：
- 直接将 `RowError` 渲染到 `QTableView` 的高亮单元格；
- 弹出无边框 `QDialog` 风格的告警；
- 写到 CSV 异常清单供二次修订后重导。

生命周期约束见 §3.2.1。

---

## 11. 并发与性能策略

### 11.1 线程与连接模型（重要修正）

| 角色 | 线程 | 连接 |
|---|---|---|
| Excel Reader | 1 个 | 无 DB 连接 |
| Validator / Router | N 个（CPU 可调） | L2 需要 DB 读连接，每线程**独立** `QSqlDatabase`（按 connectionName 区分） |
| DB Writer | **强制 1 个**（SQLite 单写者；其他方言为简化也单写） | 1 个独立写连接 |

QtSql 约束：
- `QSqlDatabase` / `QSqlQuery` **不可跨线程共享**；每线程通过 `QSqlDatabase::addDatabase(driver, name)` 拿到独立连接句柄。
- SQLite 即使 WAL 模式也**只允许一个写者**，多个写者会 `SQLITE_BUSY`；故写入串行化在 DB Writer 线程。
- WAL 模式下读连接不被写阻塞，因此校验阶段的 SELECT 可并行。

### 11.2 批量执行策略（按 capability 选路）

| 驱动能力 | 选择 |
|---|---|
| `batchUpsertNative=true` | 单次 `execBatch` 完成 upsert（如 MySQL 多 VALUES + ODKU） |
| `batchInsertNative=true` 但 upsert 不可批 | 拆为 batch 探测 + 个别冲突行降级单条 upsert |
| 都不支持 | Prepared statement + 循环单行执行（SQLite 默认走此路径，配合 WAL 仍可接受） |
| `perRowErrorInBatch=false` 但需要行级错误 | 失败时二分拆批定位失败行（chunk → 半 → 单行） |

### 11.3 Excel I/O 性能与内存

**QXlsx 实际能力澄清**：QXlsx 默认走 DOM 模型（载入全 workbook），对大文件并不友好。本库的处理：

- **读路径**：实现 `XlsxStreamReader`，直接基于 zip + `QXmlStreamReader` 解析 `xl/worksheets/sheetN.xml` 与 `xl/sharedStrings.xml`，行级流式输出；样式信息按需懒加载。本路径在 §14 阶段 0/2 作为**预研验证项**，未通过前文档关于"100w 行可行"的承诺不成立。
- **写路径**：使用 QXlsx 的写出 API，但对超出阈值（默认 50w 行）的导出走"分 Sheet"或"分文件"，并对 sharedStrings 池设上限避免内存爆炸。
- **样式**：导出阶段样式按 class 预创建一次复用，不每行新建 Format 对象。

### 11.4 其他

| 策略 | 说明 |
|---|---|
| L2 键集"按需"装载 | 仅 SELECT 当前 chunk 涉及的键，不做全量预热（避免百万级 PK OOM） |
| Bloom Filter（可选） | 仅作"明显未冲突"的快速放行提示，**不作拒写依据**（假阳性） |
| 进度合并 | 按 chunk 边界回报，避免高频信号槽抖动 |
| 临时索引 | 大批导入前对外键 / 唯一键缺索引的列发出 `W_MISSING_INDEX` 告警（不自动建） |

---

## 12. 目录结构（建议）

```
dbridge/
├── include/dbridge/            # Public headers (安装到系统)
│   ├── DataBridge.h
│   ├── IDataBridge.h
│   ├── IDatabaseDriver.h
│   ├── IImportJob.h
│   ├── IExportJob.h
│   ├── IAlertSink.h
│   ├── IProgressReporter.h
│   ├── Types.h
│   ├── Config.h
│   └── Errors.h
├── src/
│   ├── api/                    # Facade + PImpl
│   ├── orchestrator/           # Import/Export Orchestrator
│   ├── excel/                  # QXlsx 适配（Reader/Writer）
│   ├── routing/                # Discriminator / Slicer / Topo
│   ├── validation/             # Validator Registry + 内置实现
│   ├── schema/                 # Introspector + DataDictionary
│   ├── sql/                    # SqlBuilder + Upsert Executor
│   ├── tx/                     # TransactionManager
│   ├── driver/
│   │   ├── sqlite/             # 内置 SQLite 驱动
│   │   ├── mysql/              # 预留
│   │   └── oracle/             # 预留
│   ├── config/                 # JSON Profile Loader
│   ├── log/                    # Logger
│   └── util/                   # 通用工具
├── 3rdparty/
│   └── QXlsx/                  # 源码集成
├── tests/
│   ├── unit/                   # 各模块单测
│   ├── integration/            # 端到端
│   └── data/                   # 样例 xlsx + profile
├── examples/
│   ├── basic_import/
│   ├── auto_schema/
│   └── heterogeneous_routing/
├── docs/
├── CMakeLists.txt
└── dbridge.pro
```

---

## 13. 测试策略

| 层级 | 用例样本 |
|---|---|
| 单元 | SqlBuilder 各方言 Upsert 生成、Validator 边界、Topo Sort 环检测、Profile JSON Schema 校验 |
| 模块 | Introspector 对 SQLite 元数据解析正确性（含复合 PK、partial index、表达式索引）；MySQL/Oracle 在对应驱动**启用后必测**（v1 默认只测 SQLite + SPI mock） |
| 驱动能力矩阵 | 每个驱动声明的 `capabilities()` 与实际执行行为**一致性测试**（execBatch、upsert、returning、per-row error） |
| 集成 | 1000+ 行混编 A/B/C → m/n/o 多表注入，含成功 / 失败 / 部分成功三路 |
| 路由 | 多重命中（uniqueMatch）、无匹配、隐藏行 / 合并单元格、缺列、保留字列名 |
| 反向一致性 | 导入→导出→再导入 round-trip 等价；cardinality `expandRows/concatCells/nestedJson` 各一组 |
| 性能 | 10w / 50w / 100w 行导入吞吐与**内存峰值**；分别测 QXlsx DOM 路径与自研 streaming 路径 |
| QXlsx 预研基线（阶段 0/2 验收前置） | 单 Sheet 100w 行读、含 sharedStrings 占比 30% / 60% / 90%；内存阈值 ≤ 1.5GB |
| ABI 兼容（正向） | Qt 5.12.12 + 兼容矩阵内（同编译器主版本 / 同 CRT / 同 STL ABI 标志 / 同 C++ 标准 / 同 Debug/Release）：宿主链接同一 DLL 的运行测试，至少 GCC 与 MSVC 各一组 |
| ABI 兼容（负向） | 故意偏离兼容矩阵的组合（不同 CRT、调试/发布混搭、不同 `_GLIBCXX_USE_CXX11_ABI`）必须**失败或拒绝加载**而不是静默崩溃；至少 2 组负向用例 |
| 事务 | `AllOrNothing` / `ChunkCommit` / `ContinueOnError` 三模式各一组；Savepoint 嵌套；崩溃恢复（kill -9 后重启） |
| 并发 | SQLite WAL 下读写混合；MySQL 多唯一索引下的 ODKU 行为（MySQL 驱动启用后必测） |
| 故障注入 | 主键冲突 / 外键缺失 / 非法日期 / 文件半截 / 写盘满 / 连接中断 / 死锁 / 唯一冲突重试 |
| 兼容 | 未来未知表（运行时新建）auto profile 适用与回退草稿两路 |

---

## 14. 开发计划（分阶段，建议双周迭代）

> 估时仅供排期参考，可按团队节奏伸缩。

### 阶段 0 · 立项与基线 + 关键预研（约 1.5 周）
- 仓库初始化、CMake / qmake 双轨脚手架。
- 集成 QXlsx、QtSql、CI 流水线（Linux + Windows 编译矩阵）。
- 公共错误码 / 日志 / i18n 骨架。
- **预研 1（QXlsx 流式可行性）**：测 QXlsx DOM 路径 100w 行内存峰值；若超阈值，启动 zip+QXmlStreamReader 自研流式 reader 的 spike。结论纳入设计文档 §11.3。
- **预研 2（QtSql execBatch 真实行为）**：对 SQLite/MySQL 实测 execBatch 与单条循环的吞吐差；确认 `perRowErrorInBatch` 实际能力。结论写回 §3.3 capability。
- **预研 3（ABI 边界 spike）**：用 GCC/MSVC 不同小版本各自编译同一头文件，验证 PImpl + `DBRIDGE_API` 在跨编译器/STL 配置下的实际兼容范围，输出兼容矩阵。
- 交付物：可编译的空动态库 + `examples/basic_import` 占位 + 三份预研报告（决定下游阶段实现走向）。

### 阶段 1 · 数据库抽象与 SQLite 驱动（约 2 周）
- `IDatabaseDriver` SPI 定义、`registerDriver` 注册中心。
- SQLite 驱动：连接、`PRAGMA` 元数据、Upsert SQL、Savepoint。
- Schema Introspector + DataDictionary 缓存与刷新策略。
- 单测：覆盖 PRAGMA 解析、各类约束识别。

### 阶段 2 · Excel I/O 与基础导入流水线（约 2 周）
- Excel I/O 封装：按阶段 0 预研 1 的结论二选一——QXlsx DOM 包装 + 行迭代器，或自研 `XlsxStreamReader`（zip + `QXmlStreamReader`）；统一对外 API 为按行流式迭代，空行 / 表头识别。
- ImportOrchestrator 单类（无路由）骨架：Reader → 校验骨架 → Executor。
- Prepared + execBatch 写入；事务 / Savepoint / WAL 接入。
- 端到端：单表 Upsert 跑通，含失败回滚。

### 阶段 3 · 三级校验引擎（约 1.5 周）
- Validator Registry + 内置（结构 / 词法 / 范围 / 正则）。
- L2 关联校验：按 §5.2 的三段式（批内精确去重 / DB 快照按需 SELECT / L3 兜底），**不做全量 PK 预热**。
- AlertSink 接入：精确行列定位告警结构。
- 单测：错误码全覆盖。

### 阶段 4 · 路由 / 映射 / 拓扑写入（约 2 周）
- Profile JSON Schema + Loader + 校验。
- Discriminator（column / regex / composite）。
- Field Slicer：宽行 → RowPayload[]。
- Topo Sorter：主从依赖排序 + 环检测。
- FK 注入：按 §5.3.1 的三档路径选择（业务键优先 / `returnGeneratedKeys` / SELECT-back），不走旧的 `lastInsertedKey` 单点抽象。
- 集成测试：A→m、B→n、C→o 混编场景。

### 阶段 5 · 反向导出与错位组装（约 1.5 周）
- ExportOrchestrator：每类生成反规范化 SELECT（LEFT JOIN）。
- Row Interleaver：按全局排序键归并。
- 写出阶段的样式区隔（fill / 字体）。
- 集成测试：多表 → 单 Sheet 还原一致性。

### 阶段 6 · 未知表自省与"auto" Profile（约 1 周）
- `auto` 模式：表头 == 列名时自动构造 RouteRule。
- 类型自动推断（QVariant 装箱）。
- 兼容性测试：运行期新建表后立即可用。

### 阶段 7 · 并发与性能优化（约 1.5 周）
- 流水线线程化（Producer/Consumer Queue）。
- 主键集 Bloom Filter（可选）。
- 进度合并 / 日志异步化。
- 压测：10w / 50w 行场景吞吐 + 内存。

### 阶段 8 · 公共 API 冻结与 ABI 稳定（约 1 周）
- PImpl 复核、`DBRIDGE_API` 导出宏。
- 头文件最终化、示例代码完善。
- API 文档（Doxygen） + 使用手册。

### 阶段 9 · MySQL 驱动样板（可选，约 1.5 周）
- MySQL 方言 Upsert（`ON DUPLICATE KEY UPDATE`）。
- 元数据通过 `INFORMATION_SCHEMA`。
- 端到端跑通同样的混编路由用例，验证 SPI 抽象正确。

### 阶段 10 · 验收与发布（约 1 周）
- 故障注入回归。
- 用户验收测试（UAT）+ 文档定稿。
- v1.0.0 标签、release notes、变更日志。

> 总周期：约 14–16 周（含 MySQL 驱动样板）。
> 关键里程碑：M1（阶段 2 结束，端到端单表通） / M2（阶段 4 结束，异构混编通） / M3（阶段 8 结束，API 冻结）。

---

## 15. 风险与对策

### 15.1 关键架构风险（必须在 MVP 前验证）

| 风险 | 影响 | 对策 |
|---|---|---|
| 二进制 ABI 不稳定 | 跨编译器/Qt 版本崩溃 | §3.0 双轨策略 + 兼容矩阵 + 阶段 0 预研 3 + §13 ABI 测试 |
| QXlsx 不具备真正流式读 | 大文件 OOM、性能不达标 | §11.3 自研 streaming reader；阶段 0 预研 1 |
| 跨方言 generated-key 回填不可达 | 多表 FK 注入静默失败 | §3.3 capability + §5.3.1 业务键优先 + 显式 capability mismatch 拒绝 |
| 事务/Savepoint 状态机不清 | 部分写入 / 数据丢失 | §9 完整状态机 + 三模式 + QtSql transaction 不嵌套约束 |

### 15.2 一般风险

| 风险 | 影响 | 对策 |
|---|---|---|
| 用户表无唯一约束却要求 Upsert | 无法走 ON CONFLICT | 已确认 Profile：加载阶段做前置检查并报 `E_PROFILE_NO_CONFLICT_KEY`；auto profile 草稿：仍生成 `executable=false`，由 SqlBuilder 在实际执行 upsert 时再次拒绝（二次防线）。两条路径都给出补建索引的指引。 |
| Excel 单元格类型推断错乱 | 数据失真 | QXlsx + Profile 显式声明类型；推断结果记入日志 |
| 多表拓扑环依赖 | 死循环 | TopoSorter 检测到环立即 `E_TOPOLOGY_CYCLE` |
| 大文件 OOM | 进程崩溃 | 流式迭代 + chunk Savepoint，禁止一次性 `read all`；写出按行数分文件 |
| MySQL 多唯一索引 ODKU 语义偏差 | upsert 命中非预期约束 | `capabilities().conflictTargetExplicit=false` 强警告 + 集成测试覆盖多 UK 场景 |
| Oracle MERGE 并发竞态 | ORA-00001/00060/08177 | §8.3 重试策略 + `classifyError` |
| 误用 `REPLACE INTO` | 历史数据丢失 | 内部强制使用 `ON CONFLICT DO UPDATE`，对外不暴露 REPLACE 选项 |
| 回调悬空指针 | 崩溃 | §3.2.1 注册 token + cancel/wait 强约束 |
| SQLite WAL 长事务膨胀 | 磁盘占用、checkpoint 滞后 | §9.4 + 建议大文件切 `ChunkCommit` |
| 错误码改名破坏宿主 | ABI 破坏 | §10.1 registry 冻结 + 数值化稳定值 + MINOR 版本控制 |

---

## 16. 与上游需求映射核对

| 需求 | 落点 |
|---|---|
| 存在则更新 / 不存在则插入 | §8 Upsert 方言 + §5.1 Executor |
| 导入前校验，出错告警终止 | §5.2 三层校验 + §10 AlertSink |
| 未来未知表 | §5.5 Introspector + auto Profile |
| 单 Sheet A 行 → m 多表 | §4 Profile + §5.3 路由 |
| 单 Sheet A/B/C 混编 → m/n/o | §4 多 class + §5.3 拓扑写入 |
| 反向多表 → 单 Sheet 混编 | §5.4 聚合 + Row Interleaver |
| 动态库 + Public Interface | §3 IDataBridge + PImpl |
| 可扩展到 MySQL / Oracle | §3.3 IDatabaseDriver SPI |
| SOLID / 单一职责 | §2.2 / §6 设计模式表 |

---

## 17. 后续工作建议
- 完成本设计文档评审后，**先做阶段 0 的三项预研**（QXlsx 流式可行性、QtSql execBatch 真实能力、ABI 兼容矩阵），结论回写本文档的 §11.3、§3.3、§3.0。任何与预研结论冲突的设计承诺都需要在 MVP 前修订。
- 之后在 `阶段 0/1/2` 范围内构建最小可运行版本（MVP），优先打通"SQLite + 单表 Upsert + 三级校验 + AlertSink + AllOrNothing 事务模式"端到端。
- MVP 通过后再投入路由、反向导出、多方言驱动的工作量，避免架构假设在真实数据上未验证就过度展开。
- **本期 Qt 锁定 5.12.12**，不评估 Qt6 迁移，相关讨论排除在本设计范围之外。

> 本文为设计文档，不含代码实现；代码开发将依据本文档分阶段推进。
