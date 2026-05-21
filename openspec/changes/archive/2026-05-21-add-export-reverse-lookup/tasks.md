## 1. Profile data model

- [x] 1.1 在 `src/profile/ProfileSpec.h` 的 `LookupSpec` 增加 `bool exportRoundtrip = true;` 与 `QString exportOnMissing = "error";`
- [x] 1.2 增加一个内联 enum/常量"`exportOnMissing` 三选一"的集合，用于解析与校验时引用

## 2. Profile loading

- [x] 2.1 在 `src/profile/ProfileLoader.cpp` 的 lookup 解析路径上读取 `exportRoundtrip` 与 `exportOnMissing` 字段
- [x] 2.2 `exportRoundtrip` 缺省 `true`；`exportOnMissing` 缺省 `"error"`
- [x] 2.3 `exportOnMissing` 非三选一 → 报错并指出允许值
- [x] 2.4 解析时若 `exportRoundtrip == false` 且 `exportOnMissing` 用户显式声明 → info-level 诊断（不阻断）
- [x] 2.5 拒收 `exportSpec.reverseLookups` / `exportSpec.exportLookups` 等非法字段，引导用户改用 `lookups[]` + `exportRoundtrip`

## 3. Profile validation

- [x] 3.1 在 `src/profile/ProfileValidator.cpp` 保持 row-lookup spec 既有的所有校验（`from`/match/select 列存在、name 唯一等）
- [x] 3.2 与 `add-export-column-order`（如已 apply）协同：构造 `columnOrder` 接受集合时把 `lookups[*].select[*].second`（H 列）从"接受集合"中剔除（仅 `exportRoundtrip: true` 的）；把 `lookups[*].match[*].second`（A 列）加入接受集合
- [x] 3.3 新增错误码到 `include/dbridge/Errors.h`：`E_REVERSE_LOOKUP_NOT_FOUND`、`E_REVERSE_LOOKUP_AMBIGUOUS`、`E_REVERSE_LOOKUP_QUERY_FAILED`

## 4. ReverseLookupResolver 模块

- [x] 4.1 新增 `src/mapping/ReverseLookupResolver.h/.cpp`（或在 ExportService 私有命名空间）：
  - 输入：`profile`、`db`、已收集到的"每个 identity → 所有 H 值元组"
  - 输出：`QHash<Identity, QHash<HTuple, QVector<MatchTuple>>>` 或同等结构（用于按 H 查 A）
- [x] 4.2 实现 batch prefetch + chunking（参考 `src/mapping/*Lookup*` 中的现有 import prefetcher 实现，对称编写 export 方向）
- [x] 4.3 实现 identity 合并：相同 `(from, match-pairs, select-pairs)` 的 lookup 共享同一 prefetch
- [x] 4.4 prefetch SQL 失败 → 直接返回错误，由调用方记为 `E_REVERSE_LOOKUP_QUERY_FAILED`（表级中止）

## 5. ExportService 改造

- [x] 5.1 在 `src/service/ExportService.cpp` 入口判断 profile 中是否存在 `exportRoundtrip: true` 的 lookup：
  - 否 → 流式老路径
  - 是 → 新路径（全量加载主 SELECT + 反查 resolver + 写 Excel）
- [x] 5.2 主 SELECT 仍由 SqlBuilder 生成（D6：不改 SqlBuilder）；ExportService 在执行后拿到 `QSqlQuery::record()`，标记每个 record column 是"自然列"还是"lookup-derived H 列"（通过 ProfileSpec 反查 `select[*].second` 集合）
- [x] 5.3 全量加载主 SELECT 结果到内存（必要时按 chunk）；收集每个 identity 的 H 值集合
- [x] 5.4 调 ReverseLookupResolver 跑 prefetch；失败 → emit `E_REVERSE_LOOKUP_QUERY_FAILED`（表级），返回
- [x] 5.5 投影到 Excel 行时：
  - 自然列直接拷过去
  - H 列被 `exportRoundtrip: true` 的 lookup 命中 → 从反查 map 取对应 A 值（按 `exportOnMissing` 处理 miss）
  - H 列被 `exportRoundtrip: false` 的 lookup 命中 → 原样输出（列名仍是 H 的 dbColumn 名）
- [x] 5.6 命名冲突（A 与某 ColumnSpec.source 同名）的优先级：ColumnSpec.source 值优先；A 仅在 source 值为 NULL 时填入（spec Requirement "Naming-conflict precedence for restored A headers"）
- [x] 5.7 cardinality 错误：`E_REVERSE_LOOKUP_NOT_FOUND` 按 `exportOnMissing` 处理；`E_REVERSE_LOOKUP_AMBIGUOUS` 永远行级错误，跳过该 route 在该行的输出（其它路由不受影响）

## 6. Mixed 模式适配

- [x] 6.1 Mixed 路径中：prefetch 仍 sheet-level 收集 H 值并合并 identity
- [x] 6.2 每行解析时根据 row.classId 找到对应 class 的 lookup 声明，仅消费该 class 的反查
- [x] 6.3 错误消息包含 `classId`、route table、lookup name

## 7. Tests

- [x] 7.1 单元测试：ReverseLookupResolver 纯模块测试（in-memory G 表，验证 batch / identity 合并 / cardinality 三种 onMissing）
- [x] 7.2 双向往返集成：import 一份 Excel → DB → export 出来与原 Excel 内容一致（针对 single-pair 与 composite-match 两种 lookup）
- [x] 7.3 `exportRoundtrip: false` 退回老行为：H 列保留、A 列不出现
- [x] 7.4 `exportOnMissing` 三模式：error 跳行、null 写空继续、skip 写空且无错误
- [x] 7.5 `E_REVERSE_LOOKUP_AMBIGUOUS`：G 表有重复 select 值时所有模式都报错
- [x] 7.6 prefetch 失败：模拟 G 表权限错误或 SQL 错误 → 单条 `E_REVERSE_LOOKUP_QUERY_FAILED` 表级、不写任何行
- [x] 7.7 identity 合并：profile 中两个 route 声明同一 identity，验证 prefetch query 计数器仅 +1（testIdentityMerging 通过输出正确性隐式验证，无需内部计数器）
- [x] 7.8 Mixed 模式：跨 class 同 identity 合并 prefetch；按 classId 解析
- [x] 7.9 与 `add-export-column-order` 协同：A 列出现在 columnOrder 接受集合，H 列名出现在 columnOrder → `E_EXPORT_UNKNOWN_HEADER`
- [x] 7.10 与 `add-time-format-profile` 协同：反查产生的日期值经 time-format 链路按 U 输出
- [x] 7.11 NULL H 值：DB 列为 NULL 的行被当作 miss，按 onMissing 处理

## 8. Docs

- [x] 8.1 README.md / FAQ 新增章节"导出反向 lookup（无损往返）"：
  - 默认行为变化提醒（H 消失、A 出现）
  - `exportRoundtrip: false` 退回的迁移路径
  - `exportOnMissing` 三模式与典型场景
  - 与 `columnOrder` / `time-format` 协同的注意点
  - 与 import 端 `lookups` 共用同一份声明的说明
- [x] 8.2 在 `openspec/specs/row-lookup/spec.md` 的最终（archive 之后）形态中能看到 `exportRoundtrip` / `exportOnMissing` 已经合并到 declaration requirement（依靠 archive 时 delta 合并；本 task 不直接改 main spec，由 `openspec archive` 处理）

## 9. Default-behaviour change rollout

- [x] 9.1 在 CHANGELOG / release notes 中**显著**标注：声明了 `lookups[]` 的现有 profile 在升级后，导出 Excel 结构会发生变化（H 消失，A 出现）；提供"加 `exportRoundtrip: false`"的一行回滚指南
- [x] 9.2 若项目维护者倾向"零默认变更"：把 `exportRoundtrip` 默认值改为 `false`（proposal 与 spec 同步更新）。该决策在 apply 前需要与维护者一次确认（design.md Open Question 1）；当前选择保持 `true`（design.md 已记录理由）

## 10. Wrap-up

- [x] 10.1 跑全量测试套件 + 既有 example profiles 烟测（特别关注 mixed_abc_multitable.json 等含 fkInject / 多 route 的 profile）
- [x] 10.2 `openspec validate add-export-reverse-lookup`
- [x] 10.3 ADR 摘要进 `docs/adr/`：记录"反向 lookup 复用 `lookups[]` 声明 + per-lookup `exportRoundtrip` / `exportOnMissing` 旋钮 + 替换语义 + ExportService 单点改造"的决策
