## 1. 数据结构升级（基础设施层）

- [x] 1.1 在 `src/profile/ProfileSpec.h` 新增 `enum class TemporalPhysType { String, EpochSec };`
- [x] 1.2 新增 `struct TemporalSideSpec { bool declared; TemporalPhysType type; QString format; QStringList fallback; };`
- [x] 1.3 将 `TemporalFormatSpec` 重写为 `{ bool declared; TemporalSideSpec excel; TemporalSideSpec db; }`，删除旧字段 `excelFormat` / `dbFormat` / `excelFormatFallback`
- [x] 1.4 重写 `effectiveTemporalFor` 为按 side 整体覆盖：`eff.excel = col.excel.declared ? col.excel : profile.excel`，`db` 同理
- [x] 1.5 添加辅助函数 `temporalPhysTypeFromString(QString) → optional<TemporalPhysType>`，未知值返回 nullopt（用于 Loader 报错路径）

## 2. ProfileLoader 改造

- [x] 2.1 添加 `parseTemporalSide(QJsonValue, /*out*/ TemporalSideSpec, /*owner label*/ QString) → bool`，识别 `{ type, format, fallback }` 形态；`type` 缺省视为 `"string"`；`null` 报 `E_PROFILE_PARSE`
- [x] 2.2 重写 `readTemporalSlot`：先判断同 slot JSON object 内是否新旧形态共存（含 `excelFormat`/`dbFormat`/`excelFormatFallback` 任一 + `excel`/`db` 任一），共存即 `E_PROFILE_PARSE`
- [x] 2.3 在 `readTemporalSlot` 中：若仅旧形态，正规化为新形态 `excel={type:"string", format:..., fallback:...}` + `db={type:"string", format:...}`
- [x] 2.4 若仅新形态，分别调用 `parseTemporalSide` 填充 excel/db；任一 side 缺省视为未 declared
- [x] 2.5 加载列级 slot 后，在 Loader 末尾添加显式校验：每列声明的 temporal slot 数量 ≤ 1，违反 `E_PROFILE_PARSE`
- [x] 2.6 加载完成后，对每个被 temporal slot 治理的列计算 effective spec 并执行**后置一致性校验**：
  - `type == "string"` ↔ `format` 必须非空
  - `type == "epochSec"` ↔ `format` 必须为空
  - 错误消息携带列名 + slot 名 + side 名 + 触发原因
- [x] 2.7 后置校验：`type == "epochSec"` 只允许在 `datetimeFormat.db` 上出现；其他 slot × side 组合 → `E_PROFILE_PARSE`
- [x] 2.8 后置校验：`type` 未知字符串（非 `"string"` / `"epochSec"`）→ `E_PROFILE_PARSE`
- [x] 2.9 `slotFormatTokensOk` 仅在 `type == "string"` 时执行 Qt token 校验；其他 type 跳过

## 3. TemporalConvert API 升级

- [x] 3.1 修改 `formatValue` 签名：`QString → QVariant`；失败统一返回 `QVariant()` (invalid)
- [x] 3.2 `type=string` 路径：保持现有 Qt 字符串格式化逻辑，返回 `QVariant(QString)`
- [x] 3.3 新增 `type=epochSec` 序列化路径：`QDateTime.toSecsSinceEpoch()` → `QVariant(qlonglong)`；structured 非 QDateTime 时返回 `QVariant()`
- [x] 3.4 新增公共函数 `toStructured(raw, kind, TemporalSideSpec, errCode, errMsg) → QVariant`：按 `side.type` 分派到 `parseString` (string) 或 `QDateTime::fromSecsSinceEpoch` (epochSec)
- [x] 3.5 `fromSecsSinceEpoch` 边界：超出 Qt5 可表示范围时返回 invalid QDateTime → 调用方记 `E_TIME_PARSE` / `E_TIME_PARSE_DB`
- [x] 3.6 `isEmptyForTemporal` 行为不变（null/空串视为空；数字 0 / 负数不视为空）

## 4. Mapper 适配

- [x] 4.1 重构 `Mapper::map` 中的 temporal 转换段：`tconv::formatValue` 返回值由 `QString` 改为 `QVariant`；判空逻辑改为 `result.isValid() && !result.isNull()`
- [x] 4.2 改造 `eff.declared` 检查为 `eff.excel.declared || eff.db.declared`（任一 side declared 即生效）
- [x] 4.3 调用路径改为 `toStructured(excel side)` → `fromStructured(db side)`；中间结构化值用 `QVariant`
- [x] 4.4 epoch 路径下 `payload.binds.append(QVariant(qlonglong))`，依赖 SQLite affinity 写入 INTEGER 列；不再 toString 转字符串

## 5. ExportService 适配

- [x] 5.1 重构 `convertTemporalForExport` 接受 `TemporalSideSpec` 而非 `(kind, dbFormat, excelFormat)` 三元组
- [x] 5.2 显式 `dbVal.isNull()` 早返回：返回 `QVariant()`，确保 NULL 写空单元格
- [x] 5.3 `db.type == "epochSec"` 分支：`dbVal.toLongLong(&ok)`，失败记 `E_TIME_PARSE_DB`；成功调 `QDateTime::fromSecsSinceEpoch`；显式覆盖 qlonglong(0) 不被视为空的测试
- [x] 5.4 `db.type == "string"` 分支：保持现有 `parseString` 路径，错误信息更新为指向 `db.format`
- [x] 5.5 序列化输出仍由 `excel.format` (string) 控制；写 Excel 单元格为 `QString`

## 6. ProfileValidator 适配

- [x] 6.1 改写 `W_TIME_ORDERBY_NONSORTABLE` 触发条件：仅当 effective `db.type == "string"` 且 `format.startsWith("yyyy") == false` 时发警告；其他 type 一律跳过
- [x] 6.2 删除任何残留的"按字面 dbFormat 串判断"的旧路径

## 7. 测试夹具与测试用例

### 7.1 新增基础夹具

- [x] 7.1.1 创建 `tests/data/sql/08_epoch_time.sql`：`event(event_id INTEGER PRIMARY KEY, happen_at INTEGER NOT NULL)`
- [x] 7.1.2 创建 `tests/data/profiles/epoch_time.json`：`datetimeFormat: {excel:{type:"string", format:"yyyy-MM-dd HH:mm:ss"}, db:{type:"epochSec"}}`，单 happen_at 列
- [x] 7.1.3 在 `tools/build_fixtures.py` 增加 `EpochEvents.xlsx` 生成；运行后 `tests/data/xlsx/EpochEvents.xlsx` 签入

### 7.2 Profile 加载校验矩阵（`tst_profile_loader`）

每个 case 是一条独立用例（profile JSON 字面量 + 期望加载结果），覆盖：

- [x] 7.2.1 dateFormat.db.type=string，profile 级声明，无列级 → 加载成功
- [x] 7.2.2 dateFormat.db.type=string，仅列级声明 → 加载成功
- [x] 7.2.3 dateFormat.db.type=string，profile + 列级都有 → 列级整体覆盖
- [x] 7.2.4 dateFormat.db.type=epochSec（任一位置）→ `E_PROFILE_PARSE`（slot 不允许 epoch）
- [x] 7.2.5 datetimeFormat.db.type=string，profile / 列 / 双层 → 加载成功（3 个用例）
- [x] 7.2.6 datetimeFormat.db.type=epochSec，profile / 列 / 双层 → 加载成功（3 个用例）
- [x] 7.2.7 timeFormat.db.type=string，profile / 列 / 双层 → 加载成功（3 个用例）
- [x] 7.2.8 timeFormat.db.type=epochSec（任一位置）→ `E_PROFILE_PARSE`
- [x] 7.2.9 excel.type=epochSec（任一 slot、任一位置）→ `E_PROFILE_PARSE`
- [x] 7.2.10 type=string + 无 format → `E_PROFILE_PARSE`
- [x] 7.2.11 type=epochSec + 非空 format → `E_PROFILE_PARSE`
- [x] 7.2.12 unknown type 字符串 → `E_PROFILE_PARSE`
- [x] 7.2.13 format / fallback 为 JSON `null` → `E_PROFILE_PARSE`
- [x] 7.2.14 format = `""` 空字符串 视为空（在 epochSec 下合法、在 string 下报错）
- [x] 7.2.15 同 slot object 内新旧形态共存 → `E_PROFILE_PARSE`
- [x] 7.2.16 profile 级旧形态 + 列级新形态 → 加载成功，正规化后做 side-level overwrite
- [x] 7.2.17 profile 级新形态 + 列级旧形态 → 加载成功，列级旧形态正规化后整体替换 db side
- [x] 7.2.18 列级同时声明 `dateFormat` 和 `datetimeFormat` → `E_PROFILE_PARSE`
- [x] 7.2.19 列级声明空对象 `"datetimeFormat": {}` → declared=true，sides 全继承 profile 级
- [x] 7.2.20 列级 `db` 子对象覆盖时 profile 级 `db.format` 不被字段级继承（验证 side 整体覆盖）

### 7.3 Temporal 导入路径（`tst_temporal_import`）

- [x] 7.3.1 现有用例适配 `formatValue` 新返回类型（QVariant → 旧代码可能用 toString，需检查）
- [x] 7.3.2 新增：epochSec import — Excel 字符串 `"2024-05-21 10:00:00"` → DB `qlonglong(1716286800 等本地时区对应值)`
- [x] 7.3.3 新增：epochSec import — Excel 原生 `QDateTime` → DB qlonglong（结构化 bypass excel 解析）
- [x] 7.3.4 新增：epochSec import 边界 — Excel 1970-01-01T00:00:00 → DB `qlonglong(0)`
- [x] 7.3.5 新增：epochSec import 失败 — Excel 不可解析的字符串 → `E_TIME_PARSE` 行级错误

### 7.4 Temporal 导出路径（`tst_temporal_export`）

- [x] 7.4.1 现有用例适配 `convertTemporalForExport` 新签名（参数从三元组改为 `TemporalSideSpec`）
- [x] 7.4.2 新增：epochSec export — DB `qlonglong(1716286800)` → Excel 按 excel.format 序列化
- [x] 7.4.3 新增：NULL vs 0 区分 — `QVariant().isNull()` → 空单元格；`qlonglong(0)` → `"1970-01-01 00:00:00"`
- [x] 7.4.4 新增：epochSec 负数 — DB `qlonglong(-86400)` → `"1969-12-31"`
- [x] 7.4.5 新增：epochSec export 失败 — DB `QString("not a number")` 配 `db.type=epochSec` → `E_TIME_PARSE_DB`，单元格空，行继续

### 7.5 W_TIME_ORDERBY_NONSORTABLE 警告（`tst_profile_validator`）

- [x] 7.5.1 `orderBy` 含 `db.type=epochSec` 列 → 无警告
- [x] 7.5.2 `orderBy` 含 `db.type=string` + `format=dd/MM/yyyy` 列 → 触发警告
- [x] 7.5.3 `orderBy` 含 `db.type=string` + `format=yyyy-MM-dd` 列 → 无警告

### 7.6 端到端集成（`tst_import_single` 或新增）

- [x] 7.6.1 用 `epoch_time.json` + `EpochEvents.xlsx` + `08_epoch_time.sql` 完成 import → SELECT 断言 happen_at 列为正确的 qlonglong → export 回 xlsx → 与原 xlsx CSV 对账

## 8. 文档

- [x] 8.1 README §14.7：在 Profile 顶层字段说明里加入 `excel`/`db` 子对象示例与 type 取值表
- [x] 8.2 README §14.7 Q11：以并排表格展示新旧两种形态的等价关系；epoch 子场景独立示例
- [x] 8.3 新增 `docs/adr/0004-explicit-temporal-type.md`：记录 "按 side 整体覆盖" 决策、type 字段选型、epochSec 限定到 datetime slot 等关键权衡
- [x] 8.4 `docs/adr/0001-time-format-in-profile.md` 末尾追加一段交叉引用 ADR-0004
- [x] 8.5 `docs/validation/row-to-multitable.md` 场景 III 加入 epoch 子场景（III-E：DB 存 epoch 秒的端到端验证）

## 9. 验证清单（实施完成后逐项打勾）

- [x] 9.1 所有现有 `tests/data/profiles/*.json` 不修改即可通过加载（零迁移）
- [x] 9.2 `ctest --output-on-failure` 全绿（17 → 17+ 套件）
- [x] 9.3 `openspec validate add-time-explicit-type` 持续通过
- [ ] 9.4 `dbridge-cli` 手工跑 epoch profile + 真实 SQLite INTEGER 列，导入/导出对账
- [x] 9.5 grep 仓库内残留旧 API（`TemporalFormatSpec::excelFormat` / `dbFormat` 直接访问）已清零；只在 Loader 旧形态正规化路径中存在
