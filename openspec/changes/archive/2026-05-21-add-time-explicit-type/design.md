## Context

### 现有时间字段模型

`time-format` capability 当前内部数据结构（`src/profile/ProfileSpec.h`）：

```cpp
struct TemporalFormatSpec {
    bool declared = false;
    QString excelFormat;            // Qt 日期格式串
    QString dbFormat;               // Qt 日期格式串
    QStringList excelFormatFallback;
};
```

转换路径（`src/mapping/TemporalConvert.{h,cpp}`）：

- `parseString(s, kind, primary, fallback)` → 字符串 + Qt 格式串 → `QDate/QDateTime/QTime`
- `formatValue(structured, kind, fmt)` → `QString`（用 Qt 格式串序列化）

调用点（`Mapper.cpp` / `ExportService.cpp`）：

- **导入**：`QXlsx 单元格 → parseString → structured → formatValue → QString → bind 到 SQL`
- **导出**：`SQL 取出 QVariant → parseString(toString) → structured → formatValue → 写 Excel`

### 当前模型的本质局限

整个链路 **format = Qt 格式串、value = QString**。无法表达：

1. SQLite INTEGER 列存的 Unix epoch 秒数（用户的当下场景）
2. 未来可能的 epoch 毫秒、ISO 8601 with timezone、自定义编码

### 用户场景

用户的真实 schema：`event(happen_at INTEGER NOT NULL)`，列中存 `1716286800`。

- Qt SQLite 驱动返回 `QVariant(qlonglong, 1716286800)`
- 当前导出路径：`dbVal.toString() = "1716286800"` → `parseString("1716286800", DateTime, "yyyy-MM-dd HH:mm:ss")` → 失败 → `E_TIME_PARSE_DB` → 该单元格置 NULL

用户无法不重写 DB schema 直接使用 dbridge。

---

## Goals / Non-Goals

### Goals

1. 允许 Profile 显式声明 DB 侧存 Unix epoch 秒（INTEGER 列直读直写）
2. 保留旧 Profile（用 `excelFormat`/`dbFormat` 写法）的零迁移兼容性
3. 建立 type 字段作为未来类型扩展的统一入口（epochMs / nativeDate / iso8601 …）
4. 合并语义、空值定义、错误码全部 spec 化，可测试
5. 公共 API 零变更（只动 `dbridge::detail::*` 内部）

### Non-Goals（v1）

- Excel 端 epoch 数字单元格读写（`excel.type=epochSec`）
- 毫秒精度（`epochMs`）
- Excel 原生日期单元格的显式声明（`nativeDate`）—— 当前运行时嗅探已正确处理
- 非 datetime slot 配 epoch（`dateFormat.db.type=epochSec`）—— 涉及时区裁剪歧义
- AutoProfileBuilder 启发式推断 epoch 列
- 时区支持

---

## Decisions

### D1：合并语义 → **按 side 整体覆盖**

| 选项 | 描述 | 取舍 |
|---|---|---|
| 字段级合并 | `excel.type`/`excel.format`/`excel.fallback` 各自独立继承 | ✗ 产生不可解 effective spec（详见 proposal §1.1 死结） |
| 智能合并 | type 变更时静默清空相关字段 | ✗ 隐藏行为；用户调试地狱 |
| **按 side 整体覆盖**（采用） | 列级声明 `excel` 子对象 → 整体替换 profile 级 `excel` | ✓ 规则线性可解；代价是用户"只改 type"时需重写 format |

**代码层落点**：`effectiveTemporalFor` 改为：

```cpp
TemporalFormatSpec eff;
eff.declared = colSlot.declared || profileSlot.declared;
eff.excel = colSlot.excel.declared ? colSlot.excel : profileSlot.excel;
eff.db    = colSlot.db.declared    ? colSlot.db    : profileSlot.db;
return eff;
```

无中间态、无字段比较，纯指针选择。

### D2：JSON 表达 → **显式 `type` 字段**

| 选项 | 描述 | 取舍 |
|---|---|---|
| Sentinel 值 | `"dbFormat": "@epochSec"` | ✗ 隐式；与 Qt 格式串不可区分（"yyyy" vs "@epochSec" 字面规则） |
| **`{ type, format }` 子对象**（采用） | 类型与格式正交分离 | ✓ 扩展点清晰；type 枚举可统一校验 |

`type` 缺省 = `"string"`，最小破坏。

### D3：epoch 限定到 `datetimeFormat` slot

| 组合 | 决策 | 理由 |
|---|---|---|
| `datetimeFormat.db.type=epochSec` | 允许 | epoch 语义即"自 1970 起的瞬时" = `QDateTime` |
| `dateFormat.db.type=epochSec` | **禁止** | epoch → date 必须经 `QDateTime` 中转，本地时区/UTC 边界会导致 "1716249600 UTC" 在 UTC+8 取 date 得到次日；歧义未解 |
| `timeFormat.db.type=epochSec` | **禁止** | `QTime` 是"一天内某时刻"，没有自 1970 起的语义 |

未来如需开放，可在 spec 加一个 `db.timezone` 字段，但 v1 不做。

### D4："空"的定义 → **absent / "" 同义；null 报错**

| 选项 | 描述 | 取舍 |
|---|---|---|
| 三者等价 | absent / "" / null 都视为空 | ✗ 屏蔽 "变量未填值" 的 bug |
| **absent ≡ ""**, **null → error**（采用） | 区分"没写"与"显式 null" | ✓ 用户 bug 兜底；JSON 语义清晰 |

`format`（字符串）与 `fallback`（数组）适用同一规则。

### D5：`formatValue` 返回 `QVariant`，失败统一 invalid

| 选项 | 描述 | 取舍 |
|---|---|---|
| 保留 `QString` + bool out param | 旧 API 保留，新增 success 输出 | ✗ 双形态拖慢 epoch 路径；SQL bind 仍需要 `qlonglong` |
| `std::optional<QVariant>` | 现代 C++ 风格 | ✗ Qt5 项目里不一致；Mapper 已用 QVariant 表达 null |
| **`QVariant`，失败 = `QVariant()`**（采用） | 统一返回类型；调用方 `isValid()` 检查 | ✓ Qt 风格自然；epoch 路径直接绑 qlonglong |

**bind 规则**：

| `type` | 内部返回 | SQL bind 类型 |
|---|---|---|
| `string` | `QVariant(QString)` | TEXT |
| `epochSec` | `QVariant(qlonglong)` | INTEGER（依赖 SQLite affinity） |
| 失败 | `QVariant()` (invalid) | bind NULL |

### D6：新旧形态共存边界 → **同 slot object 禁止，跨层允许**

| 场景 | 决策 |
|---|---|
| 同一个 slot JSON object 内既有 `excelFormat` 又有 `excel` | `E_PROFILE_PARSE` |
| profile 级用旧形态、列级用新形态 | 允许，Loader 先正规化 profile 级旧形态 → 新形态再合并 |

**Loader 内部流程**：

```
parseProfileSlot(rawJson)
  → if has "excelFormat" or "dbFormat" or "excelFormatFallback":
      if also has "excel" or "db": ERROR
      else: normalize legacy → new form (TemporalSideSpec with type="string")
  → else if has "excel" or "db":
      parseNewForm
  → else (empty object):
      declared=true, excel/db undeclared
```

### D7：`W_TIME_ORDERBY_NONSORTABLE` 用 **反向定义**

旧条件：`dbFormat.startsWith("yyyy") == false → 警告`

新条件：`effective db.type != "string" → 跳过` && `db.type == "string" && format.startsWith("yyyy") == false → 警告`

反向定义的价值：未来加 `epochMs` / `nativeDate` 自动包含，**这条规则不必重写**。

### D8：AutoProfileBuilder → **v1 不变更**

不做启发式（列名 `_at` / `_ts` 推断 epoch）。理由：

- SQLite 元数据不区分 "存 epoch 的 INTEGER" 与 "存普通整数的 INTEGER"
- 启发式误判会让 AutoProfile 输出对用户不可信
- AutoProfile 本就是"草稿生成器"，人工 review 时显式声明 `type=epochSec` 更稳

未来可在新增 ADR 中讨论"基于列名规则 + 用户配置文件"的启发式方案。

### D9：lookup × temporal 交互 → **零代码改动**

现有 `convertTemporalForExport` 已在所有输出值上统一调用一次（含 lookup 反查的 A 列）。新增 `epochSec` 路径只是 `parseString` / `formatValue` 内部多一个分支，**对调用方完全透明**。

### D10：列级多 slot 显式禁止

现状：`temporalSlotKindFor` 注释提到"多 slot is invalid"，但 Loader 未强制。本 change **顺手补上**：

```cpp
int declaredCount = (col.dateFormat.declared ? 1 : 0)
                  + (col.datetimeFormat.declared ? 1 : 0)
                  + (col.timeFormat.declared ? 1 : 0);
if (declaredCount > 1) return ERROR;
```

不视为 breaking change —— 现有 profile 里没有合理的多 slot 声明。

---

## Architecture

### 数据结构变更（`ProfileSpec.h`）

```cpp
enum class TemporalPhysType {
    String,       // 默认，字符串 + Qt 格式
    EpochSec,     // INTEGER 秒数（仅 db 侧 + datetime slot）
};

struct TemporalSideSpec {
    bool declared = false;
    TemporalPhysType type = TemporalPhysType::String;
    QString format;
    QStringList fallback;     // 仅 string 类型 + excel 侧使用
};

struct TemporalFormatSpec {
    bool declared = false;
    TemporalSideSpec excel;
    TemporalSideSpec db;
};
```

### 转换路径（`TemporalConvert`）

新增按 `TemporalPhysType` 分派的两条函数：

```cpp
// 任一形态 → QDate/QDateTime/QTime
QVariant toStructured(const QVariant& raw, TemporalSlotKind kind,
                      const TemporalSideSpec& side,
                      QString* errCode, QString* errMsg);

// QDate/QDateTime/QTime → 目标 side QVariant（QString 或 qlonglong）
QVariant fromStructured(const QVariant& structured, TemporalSlotKind kind,
                        const TemporalSideSpec& side);
```

`parseString` / `formatValue` **保留为内部 helper**（只处理 `type=string` 路径），但被 `toStructured` / `fromStructured` 间接调用。Mapper / ExportService 改调用新函数，不直接调 parseString。

### Loader 解析顺序

1. 读 `dateFormat` / `datetimeFormat` / `timeFormat` 三个 slot
2. 对每个 slot：
   - 同 slot 内新旧形态共存检测
   - 旧形态正规化为 `TemporalSideSpec`
   - 新形态按 `excel` / `db` 子对象解析
   - type × format 单点校验（type=epochSec 必须 format 为空）
3. profile 级解析完后，对每个列同样流程
4. 列级解析后，对每个列执行 effective spec 计算（按 side 整体覆盖）
5. 对 effective spec 做最终校验（type=string 必须 format 非空、slot kind × type 合法性、列级多 slot 禁止）

---

## Risks / Trade-offs

| 风险 | 缓解 |
|---|---|
| **现有 `tst_temporal_*` 单测因 `formatValue` 返回类型变更全部失败** | 一次性 grep/replace 适配；保持测试断言语义不变；tasks.md 单独列出 |
| **用户写出非法 effective spec（如 `db.type=epochSec` + 继承 `format`）后报错位置不直观** | Loader 报错消息必须带列名 + side + 触发字段，例：`column 'happen_at': db.type=epochSec but effective db.format='yyyy-MM-dd' (inherited from profile-level)` |
| **type 枚举值未来扩展兼容性** | spec 强制 unknown type → `E_PROFILE_PARSE`；不允许 lenient parsing 静默接受新值；旧代码无法读未来 profile 时立即报错而非沉默错误 |
| **`QDateTime::fromSecsSinceEpoch` 边界行为（公元 9999 以后返回 invalid）** | 文档化 v1 支持范围 `[INT64_MIN, ~253402300799]`；超出范围 → `E_TIME_PARSE` 行级错误，单元格置 NULL |
| **用户把 epoch 整数写入 TEXT 列**（SQLite affinity 不强制） | spec 不约束 DB 列类型；用户错配（INTEGER profile + TEXT 列）会让 ORDER BY 走字典序；文档（README + ADR）显式建议 INTEGER 列；不在库层校验 |
| **新增 18+ 测试用例增加 CI 时间** | 都是 `tst_profile_loader` 内的轻量加载用例，每个 < 10ms；可忽略 |
| **新旧形态并存的文档负担** | README §14.7 / Q11 加并排示例；ADR-0004 记录决策；validation 文档场景 III 加 epoch 子场景 |

---

## Migration Plan

### 部署步骤

1. **代码 + spec** 在同一 PR 合入（避免 spec 与实现脱节）
2. 现有 profile **零迁移**：所有 `tests/data/profiles/*.json` 不需要任何改动
3. 新增 epoch 夹具 `epoch_time.json` 验证新路径
4. 文档同步更新（README / ADR / validation doc）

### 验证清单

- [ ] 旧 profile（如 `time_formats.json`）跑 `tst_temporal_import` / `tst_temporal_export` 全绿
- [ ] 新增 `tst_profile_loader` 加载校验矩阵全绿
- [ ] 新增 epoch 路径单测（import + export）全绿
- [ ] `dbridge-cli` 跑 epoch profile + 真实 SQLite INTEGER 列，导入导出对账

### 回滚策略

revert PR。旧形态 profile 不受影响；新形态 profile 使用方需自行回退到旧形态。

由于公共 API 零变更，回滚不影响下游链接 dbridge 的宿主程序。

---

## Open Questions

1. **`epochMs` 何时入 v2**：触发条件 = 有真实业务需求或 v1 落地后由前端埋点场景驱动。
2. **AutoProfileBuilder 启发式是否要做**：等用户反馈；如要做，先在 ADR 讨论列名规则与用户配置如何组合。
3. **是否需要 `db.timezone` 字段以放开 `dateFormat.db.type=epochSec`**：v1 不做；如需求出现，单独发起 change。
4. **新增 ADR 编号选 `0004` 还是合并入 `0001`**：倾向新增 `0004-explicit-temporal-type.md`，保持每个 ADR 单一主题；`0001` 仅做交叉引用更新。
