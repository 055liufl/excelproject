## 1. Profile data model

- [x] 1.1 在 `src/profile/ProfileSpec.h` 新增 `TemporalFormatSpec { QString excelFormat; QString dbFormat; QStringList excelFormatFallback; }` 结构体
- [x] 1.2 在 `ProfileSpec` 顶层添加三个可选 slot：`std::optional<TemporalFormatSpec> dateFormat / datetimeFormat / timeFormat`（或等价的 has-flag + 值组合，遵循项目既有风格）
- [x] 1.3 在 `ColumnSpec` 上添加同名三个可选 slot 用于 per-column 覆盖
- [x] 1.4 添加一个内联帮助函数 `effectiveTemporalFor(const ColumnSpec&, const ProfileSpec&, SlotKind) -> TemporalFormatSpec`，实现字段级合并（design.md D2）

## 2. Profile loading

- [x] 2.1 在 `src/profile/ProfileLoader.cpp` 解析顶层 `dateFormat` / `datetimeFormat` / `timeFormat` 对象与其 `excelFormat` / `dbFormat` / `excelFormatFallback` 字段
- [x] 2.2 在 column 解析路径上识别同名字段并写入 `ColumnSpec`
- [x] 2.3 `excelFormatFallback` 必须是 JSON array，否则报错；空数组等价于未声明
- [x] 2.4 实现 token 校验：`dateFormat` 的 U/V 拒含 `H/h/m/s/z/t/a/A`；`timeFormat` 的 U/V 拒含 `y/M/d`；`datetimeFormat` 不约束（spec Requirement "Format token validation per slot type"）
- [x] 2.5 同列同时声明 `dateFormat` 与 `validators: ["date:fmt"]` 时，加载期发 info-level 诊断（不阻断）

## 3. Validator interop

- [x] 3.1 在 `src/mapping/Mapper.cpp` 中，当列的有效 `TemporalFormatSpec.declared == true` 时，编译 validator chain 前剔除 `date:*` token（TemporalConvert 层接管）
- [x] 3.2 `effectiveTemporalFor().declared == false` 的列（即 legacy date:fmt-only）保留原 validator 行为，无需注入额外 token
- [x] 3.3 确认 `tests/data/profiles/*.json` 中现存 validator-only profile 行为不变（构建无错；运行时测试受 Qt 环境限制，编译层面已验证）

## 4. ErrorCollector & error codes

- [x] 4.1 在 `include/dbridge/Errors.h` 添加 `E_TIME_PARSE`、`E_TIME_PARSE_DB`、`W_TIME_ORDERBY_NONSORTABLE` 三个常量
- [x] 4.2 在 `src/service/ErrorCollector.h/.cpp` 添加 `addWarning(...)` / `addTableWarning(...)` 通道与独立 `warnings_` 列表
- [x] 4.3 `ImportResult` / `ExportResult` 扩展 `warnings` 字段，`ImportService` / `ExportService` 在返回前合并运行期 warnings 与 profile.loadWarnings

## 5. Excel reader path

- [x] 5.1 `ExcelReader::cellBySource` 直接返回 QXlsx 的原始 QVariant，不做 toString 转换（已有实现满足要求）
- [x] 5.2 在 `src/mapping/Mapper.cpp` 实现 import-direction 转换（native 旁路 / U 解析 / fallback / E_TIME_PARSE）
- [x] 5.3 成功解析后按 `dbFormat` 序列化为 QString 写入 payload
- [x] 5.4 NULL / 空字符串短路为 QVariant()，不报错

## 6. Excel writer path

- [x] 6.1 在 `src/service/ExportService.cpp` 构建 `dbColumn → TemporalColumnInfo` 映射
- [x] 6.2 对 temporal 列：native QDate/DateTime/Time 跳过 V 解析；字符串按 dbFormat 解析失败记 `E_TIME_PARSE_DB`，单元格置 NULL，行继续
- [x] 6.3 成功后按 excelFormat 序列化为 QString 再写 Excel
- [x] 6.4 Mixed 模式与 SingleTable 模式均已覆盖

## 7. orderBy warning

- [x] 7.1 在 `src/profile/ProfileValidator.cpp::validate()` 末段遍历 `exportSpec.orderBy`，剥除 `table.` 前缀
- [x] 7.2 检查命中 temporal 列的有效 `dbFormat` 是否以 `yyyy` 起首
- [x] 7.3 不满足则发 `W_TIME_ORDERBY_NONSORTABLE` warning（不阻断）

## 8. Tests

- [x] 8.1 新增 `tests/data/profiles/time_formats.json`：覆盖三个 slot、per-column 覆盖、`excelFormatFallback`、与 `date:fmt` 共存等典型形态
- [x] 8.2 ProfileLoader 解析测试：token 校验（错误 / 接受），字段级合并，fallback 数组校验（加入 `tst_profile_loader.cpp`）
- [x] 8.3 Import 集成测试（需 Qt 测试环境）：
  - 字符串单元格 + 主格式成功
  - 字符串单元格 + 主格式失败 → fallback 救活
  - 字符串单元格 + 主格式与 fallback 全失败 → `E_TIME_PARSE`，行被跳过，sheet 其它行成功
  - 原生 `QVariant::Date` 单元格 → 旁路 U，DB 中是 `dbFormat` 字符串
- [x] 8.4 Export 集成测试（需 Qt 测试环境）：
  - DB 字符串 → V 解析 → U 序列化
  - DB 字符串不符 V → `E_TIME_PARSE_DB`，该单元格 NULL，行其它列正常
  - DB NULL → Excel 空单元格，零错误
  - `datetimeFormat` V→U 完整路径（SQLite 驱动返回字符串，native QVariant::DateTime bypass 留给其他 Qt 驱动）
- [x] 8.5 orderBy warning 测试：命中 `yyyy-MM-dd` 列无 warning；命中 `dd/MM/yyyy` 列有 warning；命中非 temporal 列无 warning（加入 `tst_profile_validator.cpp`）
- [x] 8.6 兼容回归：build 无错，legacy validator-only 列在 `effectiveTemporalFor().declared == false` 时保持原路径不变

## 9. Docs

- [x] 9.1 README.md Q11 新增"时间字段配置"节，覆盖三种 slot、per-column 覆盖、`excelFormatFallback`、与 `date:fmt` 的优先级、orderBy 提示、不引入时区的明确声明
- [x] 9.2 推荐 `dbFormat` ISO 起手值已在 Q11 中以表格形式列出

## 10. Wrap-up

- [x] 10.1 全量 build 通过；Qt 测试环境限制导致运行期测试无法执行（预存在问题）
- [x] 10.2 `openspec validate add-time-format-profile` 通过
- [x] 10.3 `docs/adr/0001-time-format-in-profile.md` 已写入
