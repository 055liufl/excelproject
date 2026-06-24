# SQLite 同步工具设计文档

> 版本：v0.2（草案）
> 日期：2026-06-24
> 对应需求：`specs/SQLite-同步工具-需求文档.md` v0.4（FR-1~FR-17、共识 C1~C17、Codex 整改 F-01~F-20）
> 本版整改：纳入 **Codex（gpt-5.5）设计评审 D-01~D-28**（5 Critical / 9 High / 11 Medium / 3 Low），把 v0.1 的"概念分解稿"收敛为**可编码契约**——补齐线程/连接模型、同事务收割契约、载荷二分（changeset 原生 apply vs 选择性推送 UPSERT）、完整数据模型 DDL、双状态机、阶段 0 硬验收，并删除违反 FR-1 的"CDC 兜底"回退。
> 定位：需求文档的实现侧设计，在**现有 dbridge 库**之上增量集成，不重写既有 ETL 通道。

---

## 0. 文档目的与范围

| 项 | 说明 |
|---|---|
| 目的 | 把需求 FR/共识落成**可编码的线程模型、模块、接口、数据模型 DDL 与流程**。 |
| 范围 | 同步引擎（星型增量 + 上行选择性推送 + 下行自动广播）、场景 2 比对/合并、精简批量导入导出门面。 |
| 非范围 | DDL/Schema 传播、CRDT、分布式共识、传输工具本身、宿主 GUI（需求 §1.3）。**触发器 CDC 不是本设计的运行时降级路径**（见 §13.1）。 |
| 读者 | 实现工程师、评审者；前置阅读需求文档 v0.4。 |

### 0.1 现状基线（代码库探查）与对设计的硬约束

| 现状事实 | 设计影响 |
|---|---|
| `DataBridge` 为 PImpl；`DataBridgePrivate` 持 `QSqlDatabase db_`（主线程、UUID 连接名）、`SchemaCatalog catalog_`、`QHash<QString,ProfileSpec> profiles_`、无状态 `ImportService/ExportService` | **db_ 属创建它的线程，禁止交给后台 worker**（见 §2.4）；catalog/profiles 为连接无关元数据，可只读共享 |
| 真正的 UPSERT 落库循环埋在 `ImportService.cpp:683-731` | 提取 `UpsertExecutor`；但**仅用于 UPSERT 路径**（import / 场景2 save / 上行选择性推送），下行 changeset 走原生 `apply_v2`，二者不共享行写实现（D-04） |
| `SchemaIntrospector` 已用 `PRAGMA foreign_key_list`；`TopoSorter` 为 Kahn | 外键闭包/拓扑复用；行级闭包需面向表图的拓扑变体 |
| `dbridge::err` 为 `inline constexpr const char*` | `E_SYNC_*` 沿用 |
| **全库无 `sqlite3*` 句柄穿透；系统 Qt QSQLITE 几乎肯定未启用 `SQLITE_ENABLE_SESSION`** | **阶段 0 可行性闸门**（§13.1 硬验收）；不通过则停止实施，无运行时降级 |
| Qt 5.12 / C++17 / 静态库 `libdbridge`，3rdparty 仅 QXlsx | 同步模块同标准编入同库 |

---

## 1. 设计目标与约束

| 约束 | 落实 |
|---|---|
| **KISS** | 公开面纯抽象 + 工厂；纯轮询无回调；写串行到单写线程。 |
| **DRY** | UPSERT 三路收敛 `UpsertExecutor`；外键/拓扑/schema 复用既有件；上行/下行/场景2 复用同一传输底座与 `WriteTxn` 事务包装。 |
| **YAGNI** | 多域、CRDT、加密、回调、原始 WHERE 直通均不实现，留扩展位。 |
| **SOLID** | SRP=§3.2 每模块边界；DIP/OCP=纯抽象 + 工厂 + 策略点（ConflictPolicy/TransportAdapter）；ISP=8+8 接口不臃肿。 |
| **函数 ≤150 行** | 复杂流程预拆（见下"函数拆分约束"，D-27）。 |
| **参数 ≤7** | 多参装配走 Builder；接口方法参数恒 ≤4。 |
| **Builder 模式** | `SyncConfig::Builder` / `SyncSelection::Builder`，`build()` 完整性校验、产出不可变值对象。 |
| **单一职责 / 无重复** | §3.2 "做/不做"；两条写通道不重复实现。 |
| **循序最小可落地** | §11 阶段 0→5；**持久化基础（表/epoch/quarantine/ack）下沉到阶段 1**（D-28）。 |

**函数拆分约束（D-27）**：天然超长的流程在实现时按下表拆为小函数（每个 ≤150 行、单一职责）：

| 流程 | 拆分函数（建议） |
|---|---|
| `syncSelected` | `validateSelection` → `resolveSelectionPK` → `buildFkClosure` → `pruneConsistent` → `freezeManifest` → `streamChunks` |
| changeset 应用 | `verifySchema` → `beginWriteTxn` → `applyChangesetV2`(+冲突回调) → `advanceAppliedVector` → `appendForwardChangelog` → `commitOrRollback` |
| 上行分片应用（中心） | `decodePayload` → `checkEpoch` → `beginWriteTxn` → `applyChunkUpsert`(逐行 DO UPDATE/DO NOTHING) → `recordChunkProgress` → `emitAck` |
| 场景2 save | `validateStage` → `beginImmediate` → `upsertStaged` → `releaseInboundGate` → `commit` |

---

## 2. 设计总览

### 2.1 增量定位

保留并复用现有 ETL（Profile→映射→校验→Upsert），在其旁新增"同步编排层 + SQLite 接入层 + 传输适配层"与三个公开门面；**所有写经单写线程串行**，UPSERT 路径收敛回 `UpsertExecutor`，下行 changeset 走原生 `apply_v2`。

### 2.2 总体分层架构

```mermaid
graph TD
    subgraph Host["宿主应用（dbridge 之外）"]
        APP["业务调用方"]
        UI["场景2 双屏 UI（宿主自建）"]
    end

    subgraph Facade["dbridge 公开面（纯抽象 + 工厂；只入队/查询，不直接写库）"]
        DB["DataBridge（现有 importExcel/exportExcel）"]
        BT["IBatchTransfer（非阻塞批量）"]
        SE["sync::ISyncEngine（8+1）"]
        CS["sync::IComparisonSession（场景2）"]
    end

    subgraph Orch["同步编排层（后台单写线程驱动）"]
        SM["ForegroundOperationState 机"]
        BG["BackgroundPipeline + PeerState 机"]
        WK["SyncWorker（唯一写连接所有者）"]
        SEL["SelectionResolver / FkClosureBuilder"]
        CODEC["PayloadCodec（Changeset / SelectionPush 二分）"]
        ARB["ConflictArbiter / RebaseEngine"]
        APP1["ChangesetApplier（native apply_v2）"]
        APP2["SelectionPushApplier（UPSERT）"]
        WT["WriteTxn（事务包装，两路共用）"]
        ST["持久化：Changelog/AppliedVector/OutboundAck/TableState/Quarantine/PushProgress"]
        GATE["InboundTableGate（场景2 暂停闸）"]
    end

    subgraph Reuse["现有 ETL（复用）"]
        UX["UpsertExecutor（提取自 ImportService）"]
        TS["TopoSorter"]
        FK["FkInjector"]
        SI["SchemaIntrospector / SchemaCatalog"]
        SB["SqlBuilder"]
        XL["ExcelReader / Writer"]
    end

    subgraph Access["SQLite 接入层（仅本写线程）"]
        H["SqliteHandle（driver()->handle() → sqlite3*）"]
        REC["SessionRecorder"]
    end

    subgraph Trans["Transport Adapter（dbridge 边界内）"]
        OUT["OutboxWriter"]
        IN["InboxWatcher"]
        ACK["AckChannel"]
    end

    Third["第三方传输工具（黑盒）"]

    APP --> DB
    APP --> BT
    APP --> SE
    UI --> CS

    SE --> SM
    SE --> SEL
    BT --> WK
    CS --> GATE
    SM --> WK
    BG --> WK

    WK --> REC --> H
    WK --> APP1 --> WT
    WK --> APP2 --> WT
    APP2 --> UX --> SB
    WK --> ARB --> RB2["（rebase buffer 来自 apply_v2）"]
    WK --> ST
    WK --> CODEC
    SEL --> FK
    SEL --> TS
    SEL --> SI
    BT -. 复用 .-> XL
    CS --> SI
    CS --> UX

    CODEC --> OUT
    IN --> CODEC
    ACK -. ack 制品 .- OUT
    OUT -. 文件制品 .-> Third
    Third -. 文件制品 .-> IN
```

> 关键修正（D-04/D-05/D-18）：**`ChangesetApplier` 与 `SelectionPushApplier` 是两条独立写通道**，只共享 `WriteTxn`/SchemaGuard/错误归集，不共享行写实现；上行选择性推送**必须经 Outbox→第三方→Inbox**，禁止跨节点组件直接调用。

### 2.3 关键设计决策

| 决策 | 选型 | 理由 |
|---|---|---|
| 公开面 | 纯抽象 + `createXxx()` | DIP、可测、可替换 |
| 写并发 | **单写线程 + 写连接独占**（§2.4） | C15；规避 QSqlDatabase 跨线程与写写冲突 |
| 变更捕获 | 短命 session + **同事务**写 changelog | F-01 崩溃零窗口 |
| 载荷二分 | ChangesetPayload→native apply；SelectionPushPayload→UPSERT | F-02/D-04 |
| 冲突 | 自动路径 origin 规范序；上行人工"直选 DO UPDATE / 依赖 DO NOTHING"（逐行） | C7/C12/D-21 |
| 句柄穿透 | `QSqlDriver::handle()`，封进 `SqliteHandle`，仅写线程内取用 | 唯一 Qt 耦合点 |

### 2.4 线程与连接模型（D-01，本设计根基）

Qt 的 `QSqlDatabase`/`QSqlQuery` **不可跨线程**，底层 `sqlite3*` 亦绑定其连接所属线程。故定义：

```mermaid
graph LR
    subgraph MainT["主/调用线程"]
        FB["DataBridge.db_（既有，主线程专属）"]
        GET["getter 快照（加锁值拷贝，不读库）"]
    end
    subgraph WriteT["SyncWorker 线程（唯一写者）"]
        WC["写连接 wconn（本线程 addDatabase + open 同一 .db）"]
        WH["sqlite3* = SqliteHandle::of(wconn)"]
        Q["写任务队列（串行）"]
    end
    subgraph ReadT["读线程池（场景2/闭包解析）"]
        RC["只读连接（WAL 读，不阻塞写者）"]
    end
    FILE[("同一 SQLite 文件（WAL）")]
    WC --> FILE
    RC --> FILE
    FB --> FILE
    Q --> WC
```

规则：

1. **唯一写连接**：`SyncWorker` 在自身线程 `addDatabase`（独立 connName）打开**指向同一 `.db`** 的写连接 `wconn`，`SqliteHandle::of(wconn)` 取 `sqlite3*` 仅在本线程使用。**所有写**（changeset apply、上行 UPSERT、import/export、场景2 save、广播打包前的本地捕获）作为任务入**写队列**，在该线程串行执行 → 天然无写写冲突、无死锁。
2. **禁止移交 `db_`**：现有 `DataBridge::db_`（主线程）不交给后台线程；`IBatchTransfer` 后台导入复用的是 `wconn`（`ImportService::run` 无状态、连接参数化，传入 `wconn` 即可），元数据（`catalog_`/`profiles_`）为连接无关只读数据可共享。
3. **读用独立连接**：WAL 下读者不阻塞写者；场景2 物化、闭包解析各自用只读连接（本线程创建）；getter 不读库，只返回加锁快照。
4. **跨线程只传值**：线程间仅传递 `QByteArray` 载荷、值类型快照、任务描述；绝不传 `QSqlQuery`/`sqlite3*`。

---

## 3. 模块分解与职责

### 3.1 组件依赖图（与 §2.2 同向，D-18 已统一）

```mermaid
graph LR
    subgraph Public["公开面"]
        SE["ISyncEngine"]
        BT["IBatchTransfer"]
        CS["IComparisonSession"]
        FG["ForegroundGate（按 DataBridge 共享）"]
    end
    subgraph Worker["SyncWorker（写线程）"]
        WK["任务调度"]
        WT["WriteTxn"]
        A1["ChangesetApplier"]
        A2["SelectionPushApplier"]
        REC["SessionRecorder"]
        CLOG["ChangelogStore"]
        AV["AppliedVectorStore"]
        OA["OutboundAckStore"]
        TSt["TableStateStore"]
        QS["QuarantineStore"]
        PP["PushProgressStore"]
        SG["SchemaGuard"]
        ARB["ConflictArbiter"]
        RB["RebaseEngine"]
        RT["RoutingTable"]
        EV["DeadPeerEvictor"]
        BL["BaselineManager"]
        PC["PayloadCodec"]
    end
    subgraph Sel["上行选择（读线程 + 写线程协作）"]
        SR["SelectionResolver"]
        FCB["FkClosureBuilder"]
        CCA["ConsistencyCache"]
        FM["FrozenManifest"]
        CHS["ChunkStreamer"]
    end
    subgraph Trans["传输适配"]
        OUT["OutboxWriter"]
        INB["InboxWatcher"]
        ACK["AckChannel"]
    end
    subgraph Diff["场景2"]
        GATE["InboundTableGate"]
        DE["DiffEngine"]
        STG["StagingBuffer"]
    end
    subgraph Reuse["复用 ETL"]
        UX["UpsertExecutor"]
        TS["TopoSorter"]
        FK["FkInjector"]
        SI["SchemaIntrospector"]
        SB["SqlBuilder"]
    end

    SE --> FG
    BT --> FG
    SE --> WK
    BT --> WK
    SE --> SR
    WK --> WT
    A1 --> WT
    A2 --> WT
    A2 --> UX --> SB
    WK --> A1
    WK --> A2
    WK --> REC
    WK --> CLOG
    WK --> AV
    WK --> OA
    WK --> TSt
    WK --> QS
    WK --> PP
    WK --> SG
    WK --> ARB --> RB
    WK --> RT
    WK --> EV
    WK --> BL
    WK --> PC
    SR --> FCB
    FCB --> SI
    FCB --> TS
    FCB --> CCA
    FCB --> FM --> CHS --> PC
    PC --> OUT
    INB --> PC
    OA --> ACK
    CS --> GATE --> WK
    CS --> DE --> SI
    CS --> STG --> UX
```

### 3.2 模块职责表（SRP：做 / 不做）

| 模块 | 做 | 不做 |
|---|---|---|
| **SyncEngine（ISyncEngine 实现）** | 装配组件、暴露 8+1、维护前台门控与可观测快照、入队任务 | 不直接操作 `sqlite3*`、不在调用线程写库 |
| **ForegroundGate** | 按 `DataBridge` 实例共享的前台单活动闸门（D-22）；重入 `E_BUSY` | 不拦截后台 inbox/apply/广播 |
| **SyncWorker** | 唯一写线程：串行执行写任务、周期扫 inbox、攒批广播、推进位点 | 不暴露公开接口、不渲染 |
| **WriteTxn** | 写事务包装（`BEGIN IMMEDIATE`/`COMMIT`/`ROLLBACK`），两路写通道共用 | 不实现行写、不解析载荷 |
| **ChangesetApplier** | `sqlite3changeset_apply_v2` + 冲突回调（FR-5），仅 ChangesetPayload | 不做 UPSERT、不传输 |
| **SelectionPushApplier** | 上行分片逐行 UPSERT（直选 DO UPDATE / 依赖 DO NOTHING），调 `UpsertExecutor` | 不做 changeset apply、不解析闭包 |
| **UpsertExecutor**（提取） | 统一 UPSERT 落库 + prepared 缓存 | 不解析 Profile、不读 Excel、不持事务（由 WriteTxn 持） |
| **SqliteHandle** | 写线程内取 `sqlite3*`、探测 Session 可用 | 不录制、不应用 |
| **SessionRecorder** | 写线程内绑定 `sqlite3*` 录制短命 session；提交前 `sealInto` | 不拥有事务（D-02）、不读历史区间 |
| **ChangelogStore** | 持久化/读取 `sync_changelog` 区间 | 不仲裁、不传输 |
| **PayloadCodec** | 两类载荷编解码（**类型化** DecodeResult）、压缩、校验、版本 | 不决定发给谁、不落库 |
| **ConflictArbiter / RebaseEngine** | 规范序仲裁（C7）；从 apply_v2 收集 rebase buffer 经 `sqlite3rebaser_*` 生成权威下行（D-13） | 不传输 |
| **Anchor(=OutboundAckStore)** | **仅发送端** outbound ACK 水位（D-19）；裁剪/重发/截断依据 | 不表示接收端状态 |
| **AppliedVectorStore** | **仅接收端** `(origin, stream_epoch)` 幂等去重 | 不发 ACK |
| **TableStateStore** | 每表 schema 指纹/高水位/校验和增量维护（FR-12/F-17，D-11） | 不在比对时全表扫描 |
| **SchemaGuard / QuarantineStore** | 版本比较、指纹兜底、隔离 + 重放 | 不传播 DDL |
| **BaselineManager / DeadPeerEvictor / RoutingTable** | 基线构建；三维阈值逐出 + outbox 坍缩；防回声路由 | 不删对端业务数据 |
| **SelectionResolver / FkClosureBuilder / ConsistencyCache / FrozenManifest / ChunkStreamer** | 解析选择集、外键闭包（读快照）、一致性剪枝、冻结清单、分片续传 | 不落库（交 UpsertExecutor）、不渲染 |
| **InboundTableGate（D-16）** | 场景2 会话期暂停"被比对表"的 inbox 应用并排队，save/discard 后放行 | 不渲染、不仲裁 |
| **DiffEngine / StagingBuffer** | 比对（依赖 TableStateStore，零全量拉取）、内存暂存合并 | 不落库（交 UpsertExecutor） |
| **TransportAdapter（Outbox/Inbox/Ack）** | 写出/监听文件制品、收发 ACK | 不搬运文件、不解码业务语义 |
| **BatchTransfer（IBatchTransfer 实现）** | 在 `DataBridge` 上包非阻塞调度 + 轮询，导入跑在写线程的 `wconn` | 不重写 ETL、不用 `db_` 后台写 |

---

## 4. 接口设计（精简可用 · 头文件级契约）

### 4.1 命名空间、导出宏、文件布局

- `dbridge::sync`（同步类型与门面）；`IBatchTransfer` 在 `dbridge`。导出宏 `DBRIDGE_EXPORT`。错误码 `dbridge::err`。

### 4.2 八个同步接口 —— `ISyncEngine`（`dbridge::sync`）

```cpp
class DBRIDGE_EXPORT ISyncEngine {
public:
    virtual ~ISyncEngine() = default;
    virtual bool initialize(const SyncConfig& config, QString* err = nullptr) = 0;  // ① 初始化
    virtual bool sync(QString* err = nullptr) = 0;                                   // ② 同步（手动 drain，见下）
    virtual bool stop(QString* err = nullptr) = 0;                                   // ③ 停止（仅前台 operation）
    virtual SyncState state() const = 0;                                             // ④ 状态（前台 operation 态）
    virtual SyncProgress progress() const = 0;                                       // ⑤ 进度
    virtual QList<SyncLogEntry> logs() const = 0;                                    // ⑥ 日志
    virtual QList<SyncError> errors() const = 0;                                     // ⑦ 错误
    virtual SyncResult result() const = 0;                                           // ⑧ 结果
    virtual bool syncSelected(const SyncSelection& selection, QString* err = nullptr) = 0; // ⑨ 上行选择性推送
};
DBRIDGE_EXPORT std::unique_ptr<ISyncEngine> createSyncEngine();
```

**`sync()` 语义澄清（D-23）**：`sync()` 触发一次**手动 drain**——扫描 inbox、应用待处理载荷、按 `outbound_ack` 打包待发增量并写 outbox；**非阻塞、不等待第三方搬运**。后台 `SyncWorker` 即使不调用 `sync()` 也按 `broadcastIntervalMs` 周期扫 inbox/广播。`stop()` 仅中止当前**前台 operation**，不停后台收取（如需整体下线另有 `shutdown`，本期不暴露，YAGNI）。

### 4.3 八个批量导入导出接口 —— `IBatchTransfer`（`dbridge`）

```cpp
struct TransferProgress { int percent = 0; qint64 rowsDone = 0; qint64 rowsTotal = -1; };
enum class TransferState { Idle, Running, Stopping, Completed, Stopped, Failed };

class DBRIDGE_EXPORT IBatchTransfer {
public:
    virtual ~IBatchTransfer() = default;
    virtual bool startImport(const ImportOptions& options, QString* err = nullptr) = 0; // ① 导入
    virtual bool startExport(const ExportOptions& options, QString* err = nullptr) = 0; // ② 导出
    virtual TransferProgress importProgress() const = 0;   // ③
    virtual QList<RowError>  importErrors()   const = 0;   // ④
    virtual ImportResult     importResult()   const = 0;   // ⑤
    virtual TransferProgress exportProgress() const = 0;   // ⑥
    virtual QList<RowError>  exportErrors()   const = 0;   // ⑦
    virtual ExportResult     exportResult()   const = 0;   // ⑧
    virtual bool stop(QString* err = nullptr) = 0;         // C9 增补
    virtual TransferState importState() const = 0;
    virtual TransferState exportState() const = 0;
};
DBRIDGE_EXPORT std::unique_ptr<IBatchTransfer> createBatchTransfer(DataBridge& bridge);
```

**门控作用域（D-22）**：`createBatchTransfer(bridge)` 与 `createSyncEngine()`（绑定同一 `bridge`/同一 `.db`）**共享同一 `ForegroundGate`**——保证"导入/导出/sync/syncSelected"在同一库上单活动互斥；不同 `.db` 的实例互不影响。导入在**写线程 `wconn`** 上跑 `ImportService::run`（复用元数据，不用 `db_`）。进度由复用 `onPrefetch` 同型计数钩子填充。

### 4.4 配置装配（Builder · 字段以需求 §5.2/§5.6 为准，D-14）

本设计实现**必须逐项包含**需求 §5.2 `SyncConfig::Builder` 全部字段（`nodeId/role/centerNodeId/addPeerNode/database/syncTables/outboxDir/inboxDir/quarantineDir/conflictPolicy/originPriority/peerLag{Soft,Hard}{Limit,Bytes,Ms}/outboxMax{Bytes,Artifacts}PerPeer/ackMaxDelayMs/baselineSizeWarnBytes/schemaVersion/changelogRetention/verifySchemaFingerprint/autoSyncAfterImport/broadcastIntervalMs/broadcastThreshold/maxSelectionSize/pushChunkBudgetBytes/consistencyCacheDurable`）与 §5.6 `SyncSelection::Builder`（`addRecord/addRecords/addWhere/includeFkDependencies/pruneConsistentDependencies`）。`build(QString*)` 做完整性校验，产出**不可变值对象**（线程安全跨线程传值）。

**`addWhere` 安全（D-25，对齐需求 §13 开放项）**：MVP **仅实现"表 + 主键集合"**；`addWhere` 暂不接受原始 SQL 直通——开放项关闭前，只允许经参数绑定编译的**受限 DSL**（禁多语句/子查询/函数副作用），否则 `build` 拒绝。

### 4.5 公共类型与场景2 接口

`SyncState/SyncProgress/PeerSyncState/SyncLogEntry/SyncError/SyncResult` 采纳需求 §5.1。`IComparisonSession`（含 `acceptLocal/acceptRemote/stageCell`、`fetchRemoteRows`）采纳需求 §5.4，并由 `InboundTableGate` 支撑会话期表级暂停（§5.8）。

### 4.6 错误码（完整 · 对齐需求 §7，D-12）

```cpp
namespace dbridge::err {
    // Error / Fatal
    inline constexpr const char* E_SYNC_INIT                 = "E_SYNC_INIT";
    inline constexpr const char* E_SYNC_SESSION_UNAVAILABLE  = "E_SYNC_SESSION_UNAVAILABLE";
    inline constexpr const char* E_SYNC_SCHEMA_MISMATCH      = "E_SYNC_SCHEMA_MISMATCH";
    inline constexpr const char* E_SYNC_PAYLOAD_CORRUPT      = "E_SYNC_PAYLOAD_CORRUPT";
    inline constexpr const char* E_SYNC_TRANSPORT            = "E_SYNC_TRANSPORT";
    inline constexpr const char* E_SYNC_APPLY_FK             = "E_SYNC_APPLY_FK";
    inline constexpr const char* E_SYNC_APPLY_CONSTRAINT     = "E_SYNC_APPLY_CONSTRAINT";
    inline constexpr const char* E_SYNC_NODE_UNKNOWN         = "E_SYNC_NODE_UNKNOWN";
    inline constexpr const char* E_SYNC_GAP                  = "E_SYNC_GAP";
    inline constexpr const char* E_SYNC_STAGE_STALE          = "E_SYNC_STAGE_STALE";
    inline constexpr const char* E_SYNC_STAGE_CONFLICT       = "E_SYNC_STAGE_CONFLICT";
    inline constexpr const char* E_SYNC_PEER_DEAD            = "E_SYNC_PEER_DEAD";
    inline constexpr const char* E_SYNC_SELECTION_EMPTY      = "E_SYNC_SELECTION_EMPTY";
    inline constexpr const char* E_SYNC_FK_CLOSURE_MISSING   = "E_SYNC_FK_CLOSURE_MISSING";
    inline constexpr const char* E_SYNC_FK_CYCLE_UNSUPPORTED = "E_SYNC_FK_CYCLE_UNSUPPORTED";
    inline constexpr const char* E_SYNC_SELECTION_TOO_LARGE  = "E_SYNC_SELECTION_TOO_LARGE";
    inline constexpr const char* E_SYNC_PUSH_SCHEMA_MOVED    = "E_SYNC_PUSH_SCHEMA_MOVED";
    inline constexpr const char* E_BUSY                      = "E_BUSY";
    // Warning
    inline constexpr const char* W_SYNC_CONFLICT_REPLACED      = "W_SYNC_CONFLICT_REPLACED";
    inline constexpr const char* W_SYNC_BASELINE_LARGE         = "W_SYNC_BASELINE_LARGE";
    inline constexpr const char* W_SYNC_PAYLOAD_LARGE          = "W_SYNC_PAYLOAD_LARGE";
    inline constexpr const char* W_SYNC_UNTRACKED_CHANGE       = "W_SYNC_UNTRACKED_CHANGE";
    inline constexpr const char* W_SYNC_PEER_LAGGING           = "W_SYNC_PEER_LAGGING";
    inline constexpr const char* W_SYNC_PUSH_ROW_DRIFTED       = "W_SYNC_PUSH_ROW_DRIFTED";
    inline constexpr const char* W_SYNC_CONCURRENT_MANUAL_PUSH = "W_SYNC_CONCURRENT_MANUAL_PUSH";
}
```

每个码的触发点见 §7 各流程与 §14 追溯表（不再笼统写"分散落"）。

---

## 5. 关键内部组件设计

### 5.1 SqliteHandle（句柄穿透，仅写线程）

```cpp
class SqliteHandle {
public:
    static sqlite3* of(QSqlDatabase& db);     // 在 db 所属线程调用；driver()->handle()
    static bool sessionAvailable(sqlite3* h); // PRAGMA compile_options / 运行期探测
};
```
`initialize` 在写线程探测；不可用 → `E_SYNC_SESSION_UNAVAILABLE`，拒绝同步，**不降级**（D-17）。

### 5.2 事务所有权 + 同事务收割（D-02 / F-01）

**契约（文档级，必须遵守）**：收割不由 `SessionRecorder` 拥有事务，由 `WriteTxn`（调用方）拥有。一次本地写的标准时序（全部在写线程、同一 `wconn`）：

```
WriteTxn.begin()  // BEGIN IMMEDIATE
SessionRecorder.begin(h, syncTables)
... 业务写（经 UpsertExecutor / 普通 SQL）...
SessionRecorder.sealInto(h, ChangelogStore&, txn, &seq)  // changeset() + 同连接写 sync_changelog
WriteTxn.commit()  // 单一 COMMIT：业务写 + changelog 原子提交
```

```cpp
class SessionRecorder {
public:
    bool begin(sqlite3* h, const QStringList& syncTables, QString* err);
    // 必须传入正在进行的写事务句柄与同一连接，证明同事务原子性。
    bool sealInto(sqlite3* h, ChangelogStore& store, const WriteTxn& txn,
                  qint64* outSeq, QString* err);
    void abort();
};
```
崩溃边界：因收割与 changelog 落库在 `COMMIT` 前同事务，"已提交"与"已落 changelog"是同一原子提交，零窗口。

### 5.3 PayloadCodec（类型化解码，D-07）

```cpp
struct DecodeResult {
    PayloadHeader header;            // origin/seq/parentSeq/schemaFingerprint/schemaVer/streamEpoch/routeTag
    PayloadKind   kind;              // Changeset | SelectionPush
    QByteArray    changeset;         // kind==Changeset 时有效（压缩后已解压）
    SelectionPushBody selection;     // kind==SelectionPush 时有效（含 frozenEntries/rows/recordKind/pushId/chunkSeq/totalChunks）
};
class PayloadCodec {
public:
    QByteArray encodeChangeset(const PayloadHeader&, const QByteArray& changeset);
    QByteArray encodeSelectionPush(const PayloadHeader&, const SelectionPushBody&);
    bool decode(const QByteArray&, DecodeResult* out, QString* err); // 头/body/版本/幂等键校验失败 → E_SYNC_PAYLOAD_CORRUPT
};
```

### 5.4 两条写通道（D-04 / D-21）

```cpp
// 通道 A：下行/自动增量 —— 原生 changeset
class ChangesetApplier {
public:
    bool apply(sqlite3* h, const QByteArray& changeset, const ApplyOptions&,
               ApplyOutcome* out, QString* err);     // apply_v2 + 冲突回调（FR-5/6）
};
// 通道 B：上行选择性推送 —— UPSERT（逐行模式）
enum class RecordKind { Selected, Dependency };
struct UpsertRecord { RoutePayload payload; RecordKind kind; };  // 逐行携带模式（D-21）
class SelectionPushApplier {
public:
    bool applyChunk(QSqlDatabase& wconn, const QVector<UpsertRecord>& rows,
                    ErrorCollector* errors, QString* err);   // 直选→DO UPDATE；依赖→DO NOTHING
};
class UpsertExecutor {  // 提取自 ImportService；import/场景2 save/上行三路共用
public:
    bool apply(QSqlDatabase& db, const QVector<UpsertRecord>& rows, ErrorCollector* errors);
};
```
两通道仅共享 `WriteTxn` + `SchemaGuard` + `ErrorCollector`，**不共享行写实现**。`SqlBuilder::buildUpsert` 小幅扩展支持强制 `DO NOTHING`。

### 5.5 上行选择链路（D-15）

```cpp
class SelectionResolver {  // 只读快照内解析 PK（含受限 addWhere）
public:
    bool resolvePk(ReadSnapshot& snap, const SyncSelection&,
                   QVector<RecordRef>* selected, QString* err);
};
class FkClosureBuilder {
public:
    // 读快照取行值 + Fk 图(SchemaCatalog) + Kahn 拓扑；一致性剪枝；FK 环→E_SYNC_FK_CYCLE_UNSUPPORTED
    bool build(ReadSnapshot& snap, const QVector<RecordRef>& selected,
               const SchemaCatalog&, ConsistencyCache&,
               QVector<FrozenEntry>* manifest, QString* err);
};
struct FrozenEntry { RecordRef ref; RecordKind kind; int topoIndex; QByteArray fingerprint; };
```
`FrozenManifest` 持久化（释放读快照，护 WAL，C16）；`ChunkStreamer` 按 `pushChunkBudgetBytes` 拓扑序分片（父片不晚于子片），`(pushId,chunkSeq)` 幂等续传（C13）。`ConsistencyCache` 仅由下行/基线喂养（C10）。

### 5.6 冲突仲裁与 rebase（D-13）

- `ConflictArbiter`：自动路径按 `(origin rank, seq)` 规范序应用（C7）。
- `RebaseEngine`：中心对入站 changeset 调 `apply_v2` 时**收集 rebase buffer**；对需广播的 changeset 用 `sqlite3rebaser_create/configure/rebase/delete` 生成**权威下行 changeset**，下游 `AuthoritativeApply`（强制 REPLACE，不受 ConflictPolicy，F-04）。**阶段 0 必须验证**两路冲突输入、反序到达、重放收敛（§13.1）。

### 5.7 位点分层（D-19 / F-05）

- `OutboundAckStore`（即"锚点"，**仅发送端**）：裁剪/重发/截断依据。
- `AppliedVectorStore`（**仅接收端**）：`(origin, stream_epoch)` 幂等。
- 二者不混用；不再保留含义含糊的 `AnchorStore`。

### 5.8 场景2 表级暂停闸（D-16）

```cpp
class InboundTableGate {
public:
    void open(const QStringList& watchedTables);  // 会话开启：登记被比对表
    bool shouldDefer(const QString& table) const; // 后台 apply 命中 → 入 pending 队列
    void releaseAll();                            // save/discard 后放行
};
```
`ComparisonSession` 开启时钉 `data_version` 读快照并 `open(watched)`；后台对被比对表的载荷进入 pending；`save/discard` 后 `releaseAll`；若 `data_version` 变动 → `E_SYNC_STAGE_STALE` 作废丢弃 stage。

### 5.9 并发 worker

`ForegroundGate`（按 `DataBridge` 共享）拦前台重入 → `E_BUSY`；`SyncWorker` 单线程串行处理写队列 + 周期扫 inbox + 攒批广播 + 位点推进 + 缓存盖章，不受 `E_BUSY` 约束。长推送"等 ACK"为非排他等待，不挡后台。

---

## 6. 数据模型与持久化（完整 · D-08~D-11/D-24）

> 同步元数据置于本地库 `__sync_*` 表（不参与业务同步）。

```mermaid
erDiagram
    SYNC_CHANGELOG {
        INTEGER seq PK
        TEXT kind
        TEXT origin
        TEXT source_peer
        INTEGER parent_seq
        INTEGER stream_epoch
        INTEGER schema_ver
        TEXT schema_fingerprint
        BLOB changeset
        TEXT payload_checksum
        INTEGER byte_size
        INTEGER authoritative
        INTEGER created_ms
    }
    APPLIED_VECTOR {
        TEXT origin PK
        INTEGER stream_epoch PK
        INTEGER applied_seq
        INTEGER baseline_generation
        INTEGER updated_ms
    }
    OUTBOUND_ACK {
        TEXT peer PK
        TEXT origin PK
        INTEGER stream_epoch PK
        INTEGER acked_seq
        INTEGER last_sent_seq
        INTEGER last_ack_ms
        INTEGER pending_baseline
        TEXT last_push_id
        INTEGER last_chunk_seq
    }
    SYNC_TABLE_STATE {
        TEXT table_name PK
        INTEGER stream_epoch PK
        TEXT schema_fingerprint
        INTEGER high_water_seq
        TEXT content_checksum
        INTEGER row_count
        INTEGER updated_ms
    }
    CONSISTENCY_CACHE {
        TEXT table_name PK
        TEXT primary_key PK
        BLOB center_fingerprint
        INTEGER updated_ms
    }
    QUARANTINE {
        INTEGER id PK
        TEXT origin
        INTEGER seq
        INTEGER stream_epoch
        INTEGER payload_schema_ver
        BLOB payload
        INTEGER created_ms
    }
    PUSH_PROGRESS {
        TEXT push_id PK
        TEXT origin
        TEXT peer
        INTEGER total_chunks
        INTEGER schema_ver
        TEXT status
        TEXT failed_code
        INTEGER updated_ms
    }
    PUSH_CHUNK_PROGRESS {
        TEXT push_id PK
        INTEGER chunk_seq PK
        TEXT status
        TEXT checksum
        INTEGER applied_ms
    }
    FROZEN_MANIFEST {
        TEXT push_id PK
        INTEGER chunk_seq PK
        TEXT table_name
        TEXT primary_key
        TEXT record_kind
        INTEGER topo_index
        BLOB fingerprint
    }
    PUSH_PROGRESS ||--o{ PUSH_CHUNK_PROGRESS : "分片进度"
    PUSH_PROGRESS ||--o{ FROZEN_MANIFEST : "分片清单"
    SYNC_CHANGELOG ||--o{ OUTBOUND_ACK : "按(epoch,seq)裁剪"
    SYNC_CHANGELOG ||--o{ APPLIED_VECTOR : "去重水位"
```

关键索引与规则：

- `sync_changelog`：索引 `(origin, seq)`、`(stream_epoch, seq)`；防回声/审计/2Mbps 体积测试依赖 `kind/authoritative/source_peer/byte_size`。
- **epoch 规则（F-13）**：应用前比对 `stream_epoch`；**低于当前 epoch 的载荷直接隔离/丢弃且不推进 `applied_vector`**。
- `outbound_ack`（D-09）：`last_ack_ms` 支撑 FR-10 时长阈值；`pending_baseline`/`last_push_id`/`last_chunk_seq` 支撑坍缩与多片 ACK；截断水位 = `min(活跃 peer 的 acked_seq)`，死对端退出计算。
- `push_chunk_progress`（D-10）：中心按 `chunk_seq` 顺序应用；重复 chunk **checksum 相同 = no-op**，不同 = `E_SYNC_PAYLOAD_CORRUPT`；`E_SYNC_PUSH_SCHEMA_MOVED` 时 `push_progress.status=failed`。`applied_chunks` 计数不可靠，以本表逐片 `status` 为准。
- `sync_table_state`：由 apply/import/save 路径**增量维护**，场景2 表级差异零全量扫描（F-17）。

> 阶段 1 即落上述表的**最小列与状态字段**（D-28），策略（基线/逐出/隔离重放）阶段 5 补，但不推迟持久化基础。

---

## 7. 关键流程

### 7.1 双状态机（D-06）

```mermaid
stateDiagram-v2
    state "前台 Operation 状态（state() 暴露）" as FG {
        [*] --> Idle
        Idle --> Capturing: sync()/syncSelected()/startImport/startExport
        Capturing --> Exporting: 打包/交 outbox
        Exporting --> Completed: 受理完成（不等第三方）
        Capturing --> Stopped: stop()
        Exporting --> Stopped: stop()
        Capturing --> Failed
        Completed --> Idle
        Stopped --> Idle
        Failed --> Idle
    }
    state "后台 Pipeline / PeerState（progress 可观测量体现）" as BG {
        [*] --> Watching
        Watching --> Applying: inbox 到达
        Applying --> Acking: 应用成功
        Acking --> Watching
        Applying --> Quarantined: schema/epoch 不符
        Watching --> Broadcasting: 攒批触发(Center)
        Broadcasting --> Watching: 收 ACK 前移锚点
    }
```
`stop()` 只作用于 FG；BG 持续运行（除非 `shutdown`，本期不暴露）。后台失败/隔离经 `errors()`/`progress()` 可观测，不污染前台终态。

### 7.2 一轮自动增量同步（D-20 修正）

```mermaid
sequenceDiagram
    autonumber
    participant App as 调用方
    participant SE as ISyncEngine
    participant WK as SyncWorker（写线程）
    participant CLOG as ChangelogStore
    participant PC as PayloadCodec
    participant OUT as OutboxWriter
    participant IN as InboxWatcher
    participant A1 as ChangesetApplier
    participant AV as AppliedVectorStore
    participant OA as OutboundAckStore

    App->>SE: sync()  (手动 drain)
    SE-->>App: true（受理）
    SE->>WK: 入队 drain 任务
    WK->>CLOG: readRange(peer, outbound_ack, epoch)
    WK->>PC: encodeChangeset(header, blob)
    PC->>OUT: 写 outbox + .ready
    Note over OUT: 第三方搬运（黑盒）
    IN->>PC: 收到对端载荷 → DecodeResult
    PC->>A1: apply_v2 + 冲突回调（同事务）
    A1->>AV: (origin,epoch,seq) 幂等去重
    A1-->>WK: ApplyOutcome
    WK->>OA: 收 ACK 才前移锚点
    WK-->>SE: 更新 progress/result 快照
    App->>SE: progress()/result() 轮询
```

### 7.3 上行人工选择性推送（D-05 修正：必经传输）

```mermaid
sequenceDiagram
    autonumber
    participant App as Edge 调用方
    participant SE as ISyncEngine(Edge)
    participant SR as SelectionResolver
    participant FCB as FkClosureBuilder
    participant FM as FrozenManifest
    participant CHS as ChunkStreamer
    participant PC as PayloadCodec(Edge)
    participant OUT as OutboxWriter(Edge)
    participant Third as 第三方传输
    participant IN as InboxWatcher(Center)
    participant SPA as SelectionPushApplier(Center)
    participant ACK as AckChannel

    App->>SE: syncSelected(selection)
    alt 空选择
        SE-->>App: false + E_SYNC_SELECTION_EMPTY
    end
    SE->>SR: 只读快照解析 PK
    SR->>FCB: 外键闭包 + 拓扑 + 一致性剪枝
    alt FK 环 / 悬挂父 / 超规模
        FCB-->>SE: E_SYNC_FK_CYCLE_UNSUPPORTED / E_SYNC_FK_CLOSURE_MISSING / E_SYNC_SELECTION_TOO_LARGE
    end
    FCB->>FM: 物化冻结清单（释放读快照）
    FM->>CHS: 拓扑序分片(pushId,chunkSeq)
    loop 每分片
        CHS->>PC: encodeSelectionPush
        PC->>OUT: 写 outbox + .ready
        OUT-->>Third: 文件制品
        Third-->>IN: 文件制品
        IN->>SPA: DecodeResult → 逐行 UPSERT（直选 DO UPDATE / 依赖 DO NOTHING）
        SPA->>ACK: 分片 ACK（记 push_chunk_progress）
    end
    Note over CHS: 全片 ACK → push_progress=done；中断靠 applied_vector + chunk 进度续传
```

### 7.4 下行自动广播 + rebase + 防回声（C14/F-04/D-13）

```mermaid
sequenceDiagram
    autonumber
    participant WK as SyncWorker(Center)
    participant A1 as ChangesetApplier
    participant ARB as ConflictArbiter
    participant RB as RebaseEngine
    participant RT as RoutingTable
    participant OUT as OutboxWriter
    participant C as 子节点 C

    Note over WK: 攒批窗口（broadcastIntervalMs 或 threshold 先到先发）
    WK->>A1: 应用入站，收集 rebase buffer
    WK->>ARB: 多源冲突按 (rank,seq) 规范序
    ARB->>RB: sqlite3rebaser_* 生成权威下行 changeset
    RB->>RT: 路由：origin≠对端 且 seq>对端锚点（不回推来源）
    RT->>OUT: 仅发 C/D（concat 成一发）
    OUT-->>C: AuthoritativeApply（强制 REPLACE）
    C-->>WK: ACK → 前移 per-peer 锚点
    Note over WK: 空闲不发空包
```

### 7.5 批量导入（非阻塞 + 轮询，写线程执行）

```mermaid
sequenceDiagram
    autonumber
    participant App as 调用方
    participant BT as IBatchTransfer
    participant FG as ForegroundGate(共享)
    participant WK as 写线程(wconn)
    participant IS as ImportService.run

    App->>BT: startImport(options)
    BT->>FG: 占用前台槽
    alt Busy
        FG-->>App: false + E_BUSY
    end
    BT-->>App: true（受理）+ 重置导入轮询态
    BT->>WK: 入队导入任务（wconn + 复用 profiles/catalog）
    WK->>IS: run(profile,catalog,xlsx,options,wconn)
    loop onPrefetch 同型计数钩子
        IS-->>BT: rowsDone++ → importProgress
    end
    IS-->>BT: ImportResult；释放前台槽
    App->>BT: importProgress()/importResult() 轮询
```

### 7.6 场景2 比对/合并（D-16 修正：表级暂停闸）

```mermaid
sequenceDiagram
    autonumber
    participant UI as 宿主 UI
    participant CS as IComparisonSession
    participant GATE as InboundTableGate
    participant DE as DiffEngine
    participant TSt as TableStateStore
    participant STG as StagingBuffer
    participant UX as UpsertExecutor

    UI->>CS: 开会话（钉 data_version 读快照）
    CS->>GATE: open(watchedTables) 暂停被比对表 inbox 应用
    CS->>DE: tableDiffs()
    DE->>TSt: 读增量维护的指纹/高水位（零全量扫描）
    DE-->>UI: 表级红/绿
    UI->>CS: rowDiffs / fetchRemoteRows(keyset 分页)
    UI->>CS: acceptLocal/acceptRemote/stageCell
    UI->>CS: save()
    CS->>STG: 暂存裁决
    STG->>UX: BEGIN IMMEDIATE → UPSERT（普通 origin 本地写）
    CS->>GATE: releaseAll() 放行 pending
    Note over CS: data_version 变动 → E_SYNC_STAGE_STALE 作废
```

---

## 8. 并发模型与线程（落实 §2.4 / D-01）

```mermaid
graph TD
    subgraph FG["前台（调用线程）· ForegroundGate 单活动（按 DataBridge 共享）"]
        A1["sync / syncSelected"]
        A2["startImport / startExport"]
        A3["state/progress/... 轮询（加锁快照，任意线程）"]
    end
    subgraph BG["后台 SyncWorker（唯一写线程 · wconn · 不受 E_BUSY）"]
        B1["写队列串行：apply / upsert / import / save / 捕获"]
        B2["周期扫 inbox + 攒批广播 + ACK + 锚点 + 缓存盖章"]
    end
    subgraph RD["读线程（只读连接 · WAL 读不阻塞写）"]
        R1["场景2 物化 / 闭包解析"]
    end
    A1 --> B1
    A2 --> B1
    A3 -. 只读 .-> SNAP["加锁可观测量"]
    B1 --> SNAP
    note1["禁止把 db_ 或 sqlite3* 跨线程传递；跨线程只传值/任务"]
```

---

## 9. DRY 复用映射

| 新组件 | 复用件 | 方式 |
|---|---|---|
| `UpsertExecutor` | `SqlBuilder::buildUpsert` + ImportService 写循环 | 提取，import/场景2/上行三路共用 |
| `FkClosureBuilder` | `SchemaIntrospector`/`FkInfo`、`TopoSorter`、`FkInjector` | Fk 图 + 拓扑 + 注入父键 |
| `BatchTransfer` | `DataBridge`（profiles/catalog）+ `ImportService/ExportService.run` | 组合 + 写线程 wconn + 轮询 |
| `DiffEngine`/`StagingBuffer` | `SchemaIntrospector`、`TableStateStore`、`UpsertExecutor` | 内省取键 + 指纹比对 + save 走 UPSERT |
| 进度 | `ImportService::onPrefetch` 同型钩子 | 注入计数 lambda |
| 错误归集 | `ErrorCollector` | 累积 RowError/SyncError |

---

## 10. 可扩展性（OCP/DIP 扩展点）

| 扩展点 | 抽象 | 现期 | 未来 |
|---|---|---|---|
| 传输 | `TransportAdapter` | 文件制品 + watcher | MQ/对象存储 |
| 冲突 | `ConflictPolicy` + 回调 | SourceWins + rank | 插件裁决 |
| 选择 | `SelectionResolver` | 主键集合（addWhere 受限） | 受限 DSL（§13 待定） |
| 拓扑 | `RoutingTable` | 单域星型 | 多域跨桥（预留） |
| 一致性 | `ConsistencyCache` | 本地指纹缓存 | 清单握手 |
| 通知 | 纯轮询 | getter 快照 | 回调/信号（YAGNI 延后） |

---

## 11. 循序最小可落地（D-28 调整）

```mermaid
graph LR
    P0["阶段0 可行性闸门（硬验收）"] --> P1["阶段1 两节点最小同步 + 持久化基础表"]
    P1 --> P2["阶段2 星型 + 上行选择性推送"]
    P2 --> P3["阶段3 批量导入导出门面"]
    P3 --> P4["阶段4 场景2 对比合并"]
    P4 --> P5["阶段5 加固（策略 + 故障注入）"]
```

| 阶段 | 目标 | 关键交付 |
|---|---|---|
| **0** | Session + 句柄穿透 + apply_v2 + rebaser + 目录契约**硬验收**（§13.1）；**不过不进** | `SqliteHandle`、最小验证程序、SQLite 构建方案落定 |
| **1** | 两节点双向最小增量；**线程/连接模型 + 全部 `__sync_*` 表最小列 + epoch/quarantine/ack 持久化基础** | `SyncWorker`/`WriteTxn`、`SessionRecorder`、`ChangelogStore`、`PayloadCodec`、`Outbox/Inbox/Ack`、`ChangesetApplier`、`AppliedVectorStore`、`OutboundAckStore`、`ISyncEngine` 8 接口骨架 |
| **2** | 星型广播 + 防回声 + 上行选择性推送 | `RoutingTable`、`ConflictArbiter`、`RebaseEngine`、`SelectionResolver`、`FkClosureBuilder`、`ConsistencyCache`、`FrozenManifest`、`ChunkStreamer`、`SelectionPushApplier`、`syncSelected` |
| **3** | 精简导入导出门面 | `UpsertExecutor`（提取）、`BatchTransfer` + `createBatchTransfer`、`ForegroundGate` |
| **4** | 场景2 对比/合并 | `DiffEngine`、`TableStateStore`、`InboundTableGate`、`StagingBuffer`、`IComparisonSession` |
| **5** | 加固 | `BaselineManager`、`SchemaGuard`/`QuarantineStore` 策略、`DeadPeerEvictor`、故障注入、2Mbps 实测 |

---

## 12. 目录结构与文件布局（新增）

```
include/dbridge/
  IBatchTransfer.h
  sync/{SyncTypes.h, SyncConfig.h, SyncSelection.h, ISyncEngine.h, IComparisonSession.h}
  Errors.h                       # 追加 §4.6 全部码
src/
  batch/BatchTransfer.{h,cpp}
  sync/
    SyncEngine.{h,cpp}  SyncWorker.{h,cpp}  ForegroundGate.h  WriteTxn.{h,cpp}
    state/{ForegroundStateMachine.{h,cpp}, BackgroundPipeline.{h,cpp}}
    capture/{SqliteHandle.h, SessionRecorder.{h,cpp}, ChangelogStore.{h,cpp}}
    payload/PayloadCodec.{h,cpp}
    transport/{OutboxWriter.{h,cpp}, InboxWatcher.{h,cpp}, AckChannel.{h,cpp}}
    apply/{ChangesetApplier.{h,cpp}, SelectionPushApplier.{h,cpp},
           UpsertExecutor.{h,cpp}, AppliedVectorStore.{h,cpp}}
    conflict/{ConflictArbiter.{h,cpp}, RebaseEngine.{h,cpp}, RoutingTable.{h,cpp}}
    anchor/OutboundAckStore.{h,cpp}
    baseline/BaselineManager.{h,cpp}
    schema/{SchemaGuard.{h,cpp}, QuarantineStore.{h,cpp}, TableStateStore.{h,cpp}}
    peer/DeadPeerEvictor.{h,cpp}
    selection/{SelectionResolver.{h,cpp}, FkClosureBuilder.{h,cpp},
               ConsistencyCache.{h,cpp}, FrozenManifest.{h,cpp}, ChunkStreamer.{h,cpp}}
    diff/{DiffEngine.{h,cpp}, InboundTableGate.{h,cpp}, StagingBuffer.{h,cpp},
          ComparisonSession.{h,cpp}}
```
`UpsertExecutor` 提取后 `ImportService` 改为调用它（重构 + 回归测试守护）。同步模块需 `-DSQLITE_ENABLE_SESSION -DSQLITE_ENABLE_PREUPDATE_HOOK`（§13.1）。

---

## 13. 关键风险与权衡

### 13.1 【最高】阶段 0 硬验收：SQLite Session 构建 + 句柄穿透（D-03/D-13/D-17）

仅把 amalgamation 放入 `3rdparty/` **不会**让 Qt 5.12 的 QSQLITE 插件使用它。方案与**硬验收清单**（任一不过 → 阶段 0 失败、**停止实施**，无运行时降级）：

1. 构建：以 `-DSQLITE_ENABLE_SESSION -DSQLITE_ENABLE_PREUPDATE_HOOK` 编译 SQLite amalgamation，并**重编 QSQLITE 驱动插件**链接到它（或静态链接、消除符号冲突）。
2. 运行期打印 `sqlite3_libversion()`、`PRAGMA compile_options` 须含 `ENABLE_SESSION`/`ENABLE_PREUPDATE_HOOK`。
3. 从**同一 `QSqlDatabase`** 取出的 `sqlite3*` 可成功调用 `sqlite3session_create/attach/changeset`。
4. `sqlite3changeset_apply_v2` + **rebaser 链路**（apply_v2 收集 rebase buffer → `sqlite3rebaser_*`）在 Qt 连接内、对两路冲突输入、反序到达跑通且收敛。
5. 若 Qt 插件实际未链接同一 SQLite（句柄不可用或 Session 符号缺失）→ 失败。

### 13.2 其它

| 风险 | 对策 |
|---|---|
| `UpsertExecutor` 提取回归 | 先补 `ImportService` 回归测试（现有 tests/），先红后绿 |
| 2Mbps | changeset 压缩 + 一致性剪枝 + 攒批合并 + 分片续传 |
| 大闭包/长推送 | 冻结清单（护 WAL）+ `E_SYNC_SELECTION_TOO_LARGE` + 分片可续 |
| FK 环 | `E_SYNC_FK_CYCLE_UNSUPPORTED`（本期仅无环） |
| WHERE 注入 | 受限 DSL + 参数绑定；MVP 仅 PK 集合（§4.4） |
| 量化阈值 | 随需求 §13 R5 设计阶段定值 |

---

## 14. 需求 → 设计逐条追溯（D-26）

### 14.1 FR 追溯

| FR | 设计落点（章节 / 组件 / 接口） | 测试断言 |
|---|---|---|
| FR-1 捕获/changelog | §5.2 SessionRecorder.sealInto（同事务）；§6 SYNC_CHANGELOG | 崩溃后无"已提交未捕获" |
| FR-2 同步表/外部写 | §2.4 SqliteHandle；TableStateStore；`data_version` | 外部写 → W_SYNC_UNTRACKED_CHANGE |
| FR-3 载荷 | §5.3 PayloadCodec（二分） | 缺头字段 → E_SYNC_PAYLOAD_CORRUPT |
| FR-4 传输/ACK | §3 TransportAdapter；§4.6 E_SYNC_TRANSPORT；ackMaxDelayMs | ACK 最迟发送 |
| FR-5 应用/冲突 | §5.4 ChangesetApplier（native）；E_SYNC_APPLY_FK/CONSTRAINT | 冲突映射正确 |
| FR-6 多源仲裁 | §5.6 ConflictArbiter（rank,seq） | 两序终态一致 |
| FR-7 schema 隔离 | §5/§6 SchemaGuard/QuarantineStore | 版本不符 → 隔离重放 |
| FR-8 基线/增量 | §5 BaselineManager；epoch | 缺口 → 基线 |
| FR-9 广播/rebase | §5.6 RebaseEngine；§7.4 | 静默后无新载荷 |
| FR-10 死对端 | §3.2 DeadPeerEvictor；OUTBOUND_ACK.last_ack_ms | 超阈逐出 + 截断恢复 |
| FR-11 状态机 | §7.1 双状态机 | Exporting percent=-1 |
| FR-12/13/14 场景2 | §5.8 InboundTableGate；DiffEngine；§7.6 | 零全量拉取 + STAGE_STALE |
| FR-15 批量门面 | §4.3 IBatchTransfer | E_BUSY 互斥 |
| FR-16/17 触发/上行 | §4.2⑨ + §5.4/5.5 + §7.3 | 闭包完整 + 剪枝 |

### 14.2 共识 C1~C17 追溯（要点）

| C | 落点 | C | 落点 |
|---|---|---|---|
| C1 | §5.2 | C10 | §5.5 ConsistencyCache（仅权威喂养） |
| C2 | §5.6/§7.4 | C11 | §5.5 本地自比指纹 |
| C3 | §5 BaselineManager | C12 | §5.4 逐行 DO UPDATE/DO NOTHING |
| C4 | §5 SchemaGuard | C13 | §5.5 ChunkStreamer + §6 push_chunk_progress |
| C5 | §7.6 save 普通本地写 | C14 | §7.4 攒批 |
| C6 | §5.7 OutboundAck + AppliedVector | C15 | §2.4/§8 单写线程 + 前台门控 |
| C7 | §5.6 ConflictArbiter | C16 | §5.5 FrozenManifest（护 WAL） |
| C8 | DeadPeerEvictor | C17 | SchemaGuard + ConsistencyCache.invalidate |
| C9 | §5.9 ForegroundGate | | |

### 14.3 Codex 整改 F-01~F-20 追溯（含本版新整改 D-xx）

| F | 落点 | F | 落点 |
|---|---|---|---|
| F-01 同事务收割 | §5.2（D-02 强化） | F-11 合并裁决接口 | §4.5 IComparisonSession |
| F-02 载荷二分 | §5.3/§5.4（D-04 强化） | F-12 有界远端取行 | §4.5 fetchRemoteRows |
| F-03 场景2 远端 | §7.6 + fetchRemoteRows | F-13 坍缩 epoch | §6 stream_epoch（D-08/09） |
| F-04 权威下行 | §5.6 AuthoritativeApply | F-14 ACK 最迟 | §4.6/§6 ackMaxDelayMs/last_ack_ms |
| F-05 位点分层 | §5.7（D-19 强化） | F-15 错误码触发点 | §4.6 + §14.1 |
| F-06 重试分层 | §2.4/§4.6（职责分层） | F-16 去解密 | §5.4（changeset 仅校验+解压） |
| F-07 删除连带 | §5.5/§7.3 整发失败 | F-17 校验和元数据 | §6 SYNC_TABLE_STATE（D-11） |
| F-08 FK 环 | §5.5 E_SYNC_FK_CYCLE_UNSUPPORTED | F-18 措辞 | §14（本逐条表） |
| F-09 阈值补齐 | §4.4 Builder 字段 | F-19 术语 | 需求 §2（设计沿用） |
| F-10 syncSelected 入接口 | §4.2 ⑨ | F-20 接口"8+3" | §4.3 |

> 本设计文档 v0.2 已将 Codex 设计评审 D-01~D-28 逐条整改；与需求 v0.4 一致。实现以阶段 0 硬验收为先决条件，若阶段 0 调整 SQLite 构建路径，§2.4 / §5.1 / §13.1 需同步修订。
