## Why

导入 Excel 时常遇到两类痛点：(1) 行里只有业务键（例如 `客户编号`），但落库需要的字段集合（`customer_name / tier / region` 等）已经存在于库内的参考表，强迫录入员手抄一次既冗余又易错；(2) 子表的业务外键并非单列，例如 `(order_no, tenant_id)` 复合键，或者一张子表同时引用 `orders` 和 `tenants` 两个父表，当前 `fkInject` 单列/单父表无法表达。这两类诉求改的是 `RouteSpec` / `RoutePayload` / `ImportService::run` 同一组核心结构，因此合并为一个 change 提交。

## What Changes

- **NEW** route 级 `lookups[]`：声明从同库另一张参考表 G 按业务键查回一组字段 H，合并到当前 route payload（route-local，不进入全局逻辑 row）
- **NEW** lookup-prefetch 阶段（Phase A.5）：第一遍扫 Excel 收 match key 集合，对每个 lookup 执行一次 `SELECT ... WHERE key IN (...)` 批量取回，建 in-memory map；第二遍扫按行命中
- **NEW** lookup 严格语义：命中 0 行、>1 行、match key 在 Excel 为空 → 全部 row-error 进 `ErrorCollector`（不 fatal）
- **NEW** 错误码 `E_LOOKUP_KEY_EMPTY` / `E_LOOKUP_NOT_FOUND` / `E_LOOKUP_AMBIGUOUS` / `E_LOOKUP_QUERY_FAILED`
- **BREAKING** `fkInject` 从可选对象升级为数组 `[{from, pairs:[[parent_col, child_col], ...]}, ...]`，支持多列对与多个父表；旧 `{from:"t.c", to:"t.c"}` 单列写法彻底移除，无兼容层
- **CHANGED** `doFkInject` 遍历 fkInject 数组 × pairs，多列对齐 conflictVals
- **CHANGED** `ForeignKeyPreflight` 改为复合 `WHERE c1=? AND c2=? ...`；对 lookup-derived 的 from 列跳过 preflight（G 表在预取阶段已隐式校验该 key 存在）
- **CHANGED** ProfileValidator 增补 lookup 与多列 fkInject 的存在性 / 唯一性校验
- **CHANGED** 仓内所有 `tests/data/profiles/*.json` 与 docs 示例同步改写为新 schema（一次性，本 change 内完成）

明确不做：跨 SQLite 文件 `ATTACH DATABASE`；非 SQLite 数据源 lookup；lookup 级联（lookupA 输出做 lookupB 的 match key）；自增主键回填式 fkInject；lookup 命中 N 行的"取首行"宽松模式；旧 fkInject 单列写法的兼容层 / 迁移脚本。

## Capabilities

### New Capabilities
- `row-lookup`: route 级 lookup 声明、prefetch 流程契约、严格 0/1/N 与空 key 错误语义、route-local 字段归属与"lookup 输出可作 fkInject.from"的两跳传播
- `fk-injection`: 多列对 + 多父表的外键值注入；复合 ForeignKeyPreflight；lookup-derived from 的 preflight 豁免规则

### Modified Capabilities
<!-- 当前 openspec/specs/ 为空，无已存在 capability 需变更。fkInject 的旧契约从未以 spec 形式落档，所以这里按新建处理。 -->

## Impact

- **代码** `src/profile/ProfileSpec.h`, `src/profile/ProfileLoader.cpp`, `src/profile/ProfileValidator.cpp`, `src/service/ImportService.cpp`, `src/mapping/FkInjector.cpp`, `src/validation/ForeignKeyPreflight.{h,cpp}`, `src/sql/SqlBuilder.cpp`（仅在多列 conflict 路径上确认无回归）, `include/dbridge/Errors.h`
- **测试** 新增 `tests/unit/tst_lookup_prefetch.cpp`、`tst_lookup_semantics.cpp`；扩 `tst_profile_loader.cpp`、`tst_fk_preflight.cpp` 覆盖多列与多父表分支；全量改写 `tests/data/profiles/*.json` fixture
- **文档** `README.md`、`docs/validation/row-to-multitable.md`、FAQ 同步；新增 lookup 用法章节
- **breaking 影响范围** 所有仓内 profile JSON（fixture + docs 示例）一次性改写；外部使用者需要同步迁移老 profile（无脚本，手改）
- **依赖** 无新增 Qt / 三方依赖
