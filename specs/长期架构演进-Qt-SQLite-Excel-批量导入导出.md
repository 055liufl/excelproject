# Qt + SQLite + Excel 批量导入导出长期架构演进设计

> 本文承接 MVP 之后的演进方向。MVP 的硬目标见 `specs/MVP-Qt-SQLite-Excel-批量导入导出-实现设计.md`。  
> 长期演进不阻塞 MVP 交付，只有在 MVP 端到端稳定后再分阶段引入。

---

## 1. 演进原则

1. **先稳定业务闭环，再扩展平台能力**
   - MVP 已覆盖 Upsert、前置校验、未知表、多表集合、A/B/C 混编。
   - 长期演进只增强规模、性能、可维护性和跨数据库能力。

2. **保持 Profile 向后兼容**
   - MVP Profile 能继续被新版本加载。
   - 新字段必须有默认值。
   - 废弃字段先 warning，不直接删除。

3. **抽象从真实重复中提取**
   - 不在 SQLite 路径稳定前提前做多数据库 SPI。
   - 不在 QXlsx 普通读写成为瓶颈前自研 xlsx 流式 reader。

---

## 2. 阶段 A：工程化增强

### 2.1 异步 Job API

MVP 使用同步 API，长期可演进为异步任务：

```cpp
class IImportJob {
public:
    virtual JobState state() const = 0;
    virtual void cancel() = 0;
    virtual bool wait(int timeoutMs = -1) = 0;
    virtual int okRows() const = 0;
    virtual int failedRows() const = 0;
};
```

新增能力：

- 进度回调。
- 取消导入。
- 后台线程执行。
- UI 线程安全通知。

### 2.2 结构化日志

新增：

- 导入批次 ID。
- Profile 名称和版本。
- 每个 route 的写入统计。
- 校验错误审计文件。
- SQLite 原生错误码记录。

### 2.3 更完整错误码 Registry

将 MVP 字符串错误码升级为稳定数值错误码：

| 区间 | 类别 |
|---|---|
| 1000-1999 | IO / 资源 |
| 2000-2999 | Profile |
| 3000-3999 | 校验 |
| 4000-4999 | DB 执行 |
| 5000-5999 | 路由 / 拓扑 |
| 7000-7999 | 警告 |

---

## 3. 阶段 B：事务与容错增强

### 3.1 ChunkCommit

在大文件场景下支持按 chunk 提交：

- 每 1000 或 5000 行开启一个事务。
- 当前 chunk 失败只回滚当前 chunk。
- 前序 chunk 保留。
- 最终状态可为 `PartialSuccess`。

适用：数据清洗类导入，不要求全量原子。

### 3.2 ContinueOnError

支持行级错误跳过：

- L1/L2 前置校验仍可选择失败即终止。
- DB 写入阶段的单行错误可以记录后跳过。
- SQLite 如果无法定位 batch 内错误，可二分拆批定位。

### 3.3 Savepoint 状态机

引入 Savepoint 以支持 chunk 内回退：

```sql
SAVEPOINT dbridge_sp_1;
ROLLBACK TO SAVEPOINT dbridge_sp_1;
RELEASE SAVEPOINT dbridge_sp_1;
```

---

## 4. 阶段 C：Excel 大文件能力

### 4.1 流式读取 xlsx

QXlsx 默认不是严格 SAX 流式读取。长期可实现：

- 直接读取 xlsx zip 包。
- 使用 `QXmlStreamReader` 解析 `xl/worksheets/sheetN.xml`。
- 解析 `xl/sharedStrings.xml`。
- 行级迭代输出。
- 避免全 workbook 入内存。

预研验收：

- 10w / 50w / 100w 行读取。
- sharedStrings 占比 30% / 60% / 90%。
- 内存峰值可控。

### 4.2 流式写出策略

增强：

- 超大导出按 Sheet 拆分。
- 超大导出按文件拆分。
- 样式对象复用。
- sharedStrings 池上限控制。

---

## 5. 阶段 D：Profile 能力增强

### 5.1 更丰富的列来源

在 MVP `source` 表头映射基础上，增加：

| 来源 | 含义 |
|---|---|
| `header` | 表头名 |
| `colIndex` | 列号 |
| `literal` | 常量 |
| `expr` | 受限表达式 |

### 5.2 转换器链

新增：

- `trim`
- `upper`
- `lower`
- `decimal:p,s`
- `date:fmt`
- `default:value`

### 5.3 复杂匹配规则

MVP 混编使用简单 `equals`，长期增加：

- 正则匹配。
- 多列组合匹配。
- `firstMatch`。
- `uniqueMatch`。
- 未匹配默认 class。

### 5.4 JSON Schema

为 Profile 提供完整 JSON Schema：

- 禁止未知字段静默放行。
- 在加载阶段报出字段路径。
- 配套示例和测试数据。

---

## 6. 阶段 E：更完整的未知表能力

MVP 只自动支持简单单表。长期增强：

### 6.1 ProfileDraft

生成可人工确认的草稿：

```cpp
struct ProfileIssue {
    QString code;
    QString severity;
    QString location;
    QString message;
};

struct ProfileDraft {
    QString profileJson;
    bool executable = false;
    QList<ProfileIssue> issues;
};
```

### 6.2 复杂表检测

检测：

- 复合主键。
- 多唯一索引。
- 外键拓扑。
- generated / identity 列。
- 表达式索引。
- partial index。
- 保留字列名。

不满足自动执行条件时，生成草稿并要求人工确认。

---

## 7. 阶段 F：多数据库架构

只有在 SQLite MVP 稳定后，再抽取数据库 SPI。

### 7.1 Driver Capabilities

```cpp
struct DriverCapabilities {
    bool explicitConflictTarget = true;
    bool returningSupported = false;
    bool generatedKeyOnUpdate = false;
    bool batchUpsertNative = false;
    bool perRowErrorInBatch = false;
    bool savepointNative = true;
};
```

### 7.2 MySQL

使用：

```sql
INSERT INTO t (...)
VALUES (...)
ON DUPLICATE KEY UPDATE ...
```

风险：

- MySQL 不支持显式 conflict target。
- 多唯一索引表可能命中非预期唯一约束。
- 需要 Profile 策略控制严格失败或 warning 继续。

### 7.3 Oracle

使用：

```sql
MERGE INTO ...
```

风险：

- 并发下仍可能唯一冲突。
- `MERGE + RETURNING` 支持受驱动影响。
- 需要重试策略和错误分类。

### 7.4 PostgreSQL

使用：

```sql
INSERT INTO ...
ON CONFLICT (...) DO UPDATE ...
RETURNING ...
```

PostgreSQL 能力更接近 SQLite 的 explicit conflict target，但仍需独立测试。

---

## 8. 阶段 G：代理主键 FK 回填

MVP 只支持业务键 FK 注入。长期支持代理主键：

1. 父表 upsert。
2. 获取父表主键。
3. 注入子表 FK。
4. 子表 upsert。

路径：

- `RETURNING id`。
- `last_insert_rowid()` 仅 INSERT 分支。
- upsert 后按业务唯一键 SELECT-back。

不可达场景：

- 父表没有业务唯一键。
- 驱动不支持 update 分支回填。
- 无法可靠 SELECT-back。

这种场景必须在 Profile 加载阶段拒绝。

---

## 9. 阶段 H：反向导出增强

MVP 支持 `expandRows`。长期增加：

| 策略 | 含义 |
|---|---|
| `expandRows` | 父行按子行重复 |
| `concatCells` | 子表多行拼接到单元格 |
| `nestedJson` | 子表多行写为 JSON |

增强：

- Row Interleaver。
- 多 class 全局排序。
- routeSignature 回放。
- round-trip strict 校验。

---

## 10. 阶段 I：性能与并发

### 10.1 Producer / Consumer

长期流水线：

```
Excel Reader -> Validator/Router -> DB Writer
```

约束：

- QtSql 连接不可跨线程共享。
- SQLite 仍然单写者。
- DB Writer 保持单线程。
- Validator 可并行。

### 10.2 按需键集加载

对于外键和唯一性校验：

- 不全量加载大表主键。
- 只 SELECT 当前 chunk 涉及的键。
- 必要时为缺索引列发 warning。

### 10.3 Bloom Filter

可作为快速提示：

- 未命中可快速判定大概率不存在。
- 命中不能直接拒绝，必须精确 SELECT。

---

## 11. 阶段 J：接口与 ABI 稳定

长期若作为第三方 SDK 发布，才考虑：

- `DBRIDGE_API` 导出宏。
- PImpl。
- 公共头最小化。
- 语义化版本。
- 同编译器/同 Qt 版本兼容矩阵。
- 可选 C ABI handle。

不建议在 MVP 过早投入 ABI 测试矩阵。

---

## 12. 长期测试矩阵

| 能力 | 测试 |
|---|---|
| 异步 Job | 进度、取消、wait、析构安全 |
| ChunkCommit | 部分成功、失败清单 |
| ContinueOnError | 行级错误跳过 |
| 流式 xlsx | 10w/50w/100w 行内存峰值 |
| Profile Schema | 未知字段、缺字段、类型错误 |
| 多数据库 | MySQL/Oracle/PostgreSQL 方言一致性 |
| 代理主键 FK | insert/update 分支主键回填 |
| 导出策略 | expandRows/concatCells/nestedJson |
| 并发 | 读写线程、SQLite busy_timeout |
| ABI | 兼容矩阵正向和负向用例 |

---

## 13. 推荐演进顺序

1. MVP 全部验收通过。
2. 异步 Job + 进度 + 取消。
3. ChunkCommit。
4. Profile JSON Schema。
5. Excel 流式读取预研。
6. 更完整未知表 ProfileDraft。
7. 代理主键 FK 回填。
8. 多数据库 SPI。
9. MySQL 驱动样板。
10. SDK 级 ABI 稳定。

---

## 14. 不建议提前做的事项

- 在 SQLite 路径未稳定前做 MySQL/Oracle。
- 在没有真实大文件压力前自研完整 xlsx reader。
- 在未确定对外 SDK 分发方式前做 C ABI。
- 在 Profile 尚未被业务验证前加入表达式脚本。
- 在前置校验不完善前支持 ContinueOnError。
