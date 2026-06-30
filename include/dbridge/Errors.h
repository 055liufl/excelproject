#pragma once

// ============================================================================
// Errors.h — dbridge 全局错误/警告码字典
// ============================================================================
//
// 【这个文件是什么】
//   整个 dbridge 库对外暴露的「错误码 / 警告码」常量集合。每个码都是一个稳定的
//   ASCII 字符串字面量（如 "E_OPEN_DB"），用作机器可识别的标识，便于：
//     · 调用方用 `==` 精确判断错误类型并据此分支处理；
//     · 日志/上报系统对错误归类统计（而不是去匹配易变的人类可读 message）。
//
// 【错误码如何被使用】
//   这些字符串不会单独抛出，而是被填进“错误载体”结构里随结果返回：
//     · Excel 批量导入导出路径 → dbridge::RowError{ code, message, row, column, ... }
//       （定义见 Types.h），按行/按表/按单元格粒度回报；
//     · 同步子系统路径 → dbridge::sync::SyncError{ code, severity, phase, ... }
//       （定义见 sync/SyncTypes.h），带严重级别与所处阶段。
//   故本文件只声明“词汇表”，真正的上下文（哪一行、哪张表、什么阶段）由载体补充。
//
// 【命名约定】
//   · 前缀 E_ = Error（错误，通常意味着该单元工作失败：行被跳过 / 操作中止）。
//   · 前缀 W_ = Warning（警告，非阻断：流程继续，只是给出诊断提示）。
//   · 名字按子系统/阶段分组（OPEN_* 打开资源、PROFILE_* 配置、VALIDATE_* 校验、
//     LOOKUP_* 外键查找、EXPORT_* 导出、TIME_* 时间格式、SYNC_* 同步引擎……）。
//
// 【为什么用 inline constexpr const char*】
//   · inline：C++17 起允许在头文件里定义内联变量，多个翻译单元包含本头也不会重复定义；
//   · constexpr：编译期常量，零运行时开销；
//   · const char*：纯 ASCII 码值，跨边界（日志、网络、协议）传输最稳妥。
// ============================================================================

namespace dbridge::err {

// ── Excel 批量导入导出 / Profile（ETL 配置）相关 ─────────────────────────────

inline constexpr const char* E_OPEN_DB = "E_OPEN_DB";      // 打开 SQLite 数据库失败
inline constexpr const char* E_OPEN_XLSX = "E_OPEN_XLSX";  // 打开/读取 .xlsx 文件失败
inline constexpr const char* E_PROFILE_PARSE = "E_PROFILE_PARSE";  // Profile JSON 解析失败
// Profile 引用了数据库中不存在的表
inline constexpr const char* E_PROFILE_TABLE_NOT_FOUND = "E_PROFILE_TABLE_NOT_FOUND";
// Profile 引用了表中不存在的列
inline constexpr const char* E_PROFILE_COLUMN_NOT_FOUND = "E_PROFILE_COLUMN_NOT_FOUND";
// UPSERT 需要冲突键（唯一约束）来判定“插入还是更新”，但 Profile 未提供
inline constexpr const char* E_PROFILE_NO_CONFLICT_KEY = "E_PROFILE_NO_CONFLICT_KEY";
// 多表写入需按外键依赖排出拓扑序，但表间依赖存在环，无法定序
inline constexpr const char* E_PROFILE_TOPOLOGY_CYCLE = "E_PROFILE_TOPOLOGY_CYCLE";
// Excel 表头里找不到 Profile 期望的列标题
inline constexpr const char* E_HEADER_NOT_FOUND = "E_HEADER_NOT_FOUND";
// 一行数据没有匹配到任何路由规则（无法决定写入哪张表）
inline constexpr const char* E_ROUTE_UNMATCHED = "E_ROUTE_UNMATCHED";

// ── 单元格/行级数据校验失败（VALIDATE_*，通常导致该行被跳过）────────────────
inline constexpr const char* E_VALIDATE_NULL = "E_VALIDATE_NULL";  // 非空约束被违反
inline constexpr const char* E_VALIDATE_TYPE = "E_VALIDATE_TYPE";  // 类型不符（如期望数字得到文本）
inline constexpr const char* E_VALIDATE_REGEX = "E_VALIDATE_REGEX";  // 正则格式校验不通过
inline constexpr const char* E_VALIDATE_DUPLICATE =
    "E_VALIDATE_DUPLICATE";  // 批内唯一性冲突（重复键）
inline constexpr const char* E_VALIDATE_FK = "E_VALIDATE_FK";  // 外键约束校验失败

// ── 外键“正向查找”（用业务键查代理主键）相关 ───────────────────────────────
inline constexpr const char* E_LOOKUP_KEY_EMPTY = "E_LOOKUP_KEY_EMPTY";      // 查找键为空
inline constexpr const char* E_LOOKUP_KEY_INVALID = "E_LOOKUP_KEY_INVALID";  // 查找键非法
inline constexpr const char* E_LOOKUP_NOT_FOUND = "E_LOOKUP_NOT_FOUND";      // 查无此记录
inline constexpr const char* E_LOOKUP_AMBIGUOUS = "E_LOOKUP_AMBIGUOUS";  // 命中多条，无法唯一确定
inline constexpr const char* E_LOOKUP_QUERY_FAILED = "E_LOOKUP_QUERY_FAILED";  // 查找 SQL 执行失败

// ── 写库 / 导出 ──────────────────────────────────────────────────────────────
inline constexpr const char* E_DB_UPSERT = "E_DB_UPSERT";        // UPSERT 写入数据库失败
inline constexpr const char* E_EXPORT_QUERY = "E_EXPORT_QUERY";  // 导出时的 SELECT 查询失败
inline constexpr const char* E_WRITE_XLSX = "E_WRITE_XLSX";      // 写出 .xlsx 文件失败

// add-time-format-profile: 时间格式的输入/输出失败。
// E_TIME_PARSE     — Excel→内存 解析失败（导入方向）；行级，该行对应路由被跳过。
// E_TIME_PARSE_DB  — DB→内存 解析失败（导出方向）；行级，出错单元格写 NULL，整行继续。
inline constexpr const char* E_TIME_PARSE = "E_TIME_PARSE";
inline constexpr const char* E_TIME_PARSE_DB = "E_TIME_PARSE_DB";
// 非阻断警告——orderBy 命中了一个 dbFormat 不以 `yyyy` 开头的时间列
// （启发式判断“字典序==时间序”不成立）。由 ProfileValidator 发出。
inline constexpr const char* W_TIME_ORDERBY_NONSORTABLE = "W_TIME_ORDERBY_NONSORTABLE";

// add-export-column-order: 导出列顺序（columnOrder）校验失败。
inline constexpr const char* E_EXPORT_UNKNOWN_HEADER =
    "E_EXPORT_UNKNOWN_HEADER";  // 列顺序里出现未知表头
inline constexpr const char* E_EXPORT_DUPLICATE_ORDER =
    "E_EXPORT_DUPLICATE_ORDER";  // 列顺序里有重复项
inline constexpr const char* E_EXPORT_ORDER_WITH_RAW_SQL =
    "E_EXPORT_ORDER_WITH_RAW_SQL";  // 列顺序与原生 SQL 不兼容

// add-export-reverse-lookup: 导出方向的“反向查找”（用代理主键查回业务键）失败。
// E_REVERSE_LOOKUP_NOT_FOUND    — 行级；G 端零命中；行为受 exportOnMissing 控制。
// E_REVERSE_LOOKUP_AMBIGUOUS    — 行级；G 端多命中；始终视为错误。
// E_REVERSE_LOOKUP_QUERY_FAILED — 表级；预取 SELECT 失败；整个 sheet 的导出中止。
inline constexpr const char* E_REVERSE_LOOKUP_NOT_FOUND = "E_REVERSE_LOOKUP_NOT_FOUND";
inline constexpr const char* E_REVERSE_LOOKUP_AMBIGUOUS = "E_REVERSE_LOOKUP_AMBIGUOUS";
inline constexpr const char* E_REVERSE_LOOKUP_QUERY_FAILED = "E_REVERSE_LOOKUP_QUERY_FAILED";

// ── 同步引擎 错误/致命 码（v0.5）────────────────────────────────────────────
inline constexpr const char* E_SYNC_INIT = "E_SYNC_INIT";  // 同步引擎初始化失败
// 关键：当前 QSQLITE 驱动未启用 SQLite session 扩展，无法捕获变更集（见项目 README/插件方案）
inline constexpr const char* E_SYNC_SESSION_UNAVAILABLE = "E_SYNC_SESSION_UNAVAILABLE";
inline constexpr const char* E_SYNC_SCHEMA_MISMATCH =
    "E_SYNC_SCHEMA_MISMATCH";  // 两端表结构指纹不一致
inline constexpr const char* E_SYNC_PAYLOAD_CORRUPT =
    "E_SYNC_PAYLOAD_CORRUPT";  // 收到的变更包损坏/校验失败
inline constexpr const char* E_SYNC_TRANSPORT =
    "E_SYNC_TRANSPORT";  // 传输层错误（收发 artifact 失败）
inline constexpr const char* E_SYNC_APPLY_FK = "E_SYNC_APPLY_FK";  // 应用变更时外键约束失败
inline constexpr const char* E_SYNC_APPLY_CONSTRAINT =
    "E_SYNC_APPLY_CONSTRAINT";  // 应用变更时其它约束失败
inline constexpr const char* E_SYNC_NODE_UNKNOWN =
    "E_SYNC_NODE_UNKNOWN";  // 出现未知节点（不在配置的 peer 列表）
inline constexpr const char* E_SYNC_GAP = "E_SYNC_GAP";  // 序列号出现空洞（漏收了中间的变更）
inline constexpr const char* E_SYNC_STAGE_STALE =
    "E_SYNC_STAGE_STALE";  // 比对暂存已过期（本地库在暂存后被改动）
inline constexpr const char* E_SYNC_STAGE_CONFLICT = "E_SYNC_STAGE_CONFLICT";  // 暂存合并时发生冲突
inline constexpr const char* E_SYNC_PEER_DEAD = "E_SYNC_PEER_DEAD";  // 对端长时间无响应，被判定失联
inline constexpr const char* E_SYNC_SELECTION_EMPTY =
    "E_SYNC_SELECTION_EMPTY";  // 选择性推送的选择集为空/非法
inline constexpr const char* E_SYNC_FK_CLOSURE_MISSING =
    "E_SYNC_FK_CLOSURE_MISSING";  // 选择集的外键闭包不完整
inline constexpr const char* E_SYNC_FK_CYCLE_UNSUPPORTED =
    "E_SYNC_FK_CYCLE_UNSUPPORTED";  // 外键存在环，暂不支持
inline constexpr const char* E_SYNC_SELECTION_TOO_LARGE =
    "E_SYNC_SELECTION_TOO_LARGE";  // 选择集超过上限
inline constexpr const char* E_SYNC_PUSH_SCHEMA_MOVED =
    "E_SYNC_PUSH_SCHEMA_MOVED";                  // 推送期间表结构发生迁移
inline constexpr const char* E_BUSY = "E_BUSY";  // 资源忙（已有前台操作在进行 / 数据库被占用）
inline constexpr const char* E_SYNC_WRITE_BLOCKED =
    "E_SYNC_WRITE_BLOCKED";  // 同步活动期间直写被门控阻止
inline constexpr const char* E_SYNC_UNSUPPORTED_SCHEMA =
    "E_SYNC_UNSUPPORTED_SCHEMA";  // 表结构不被同步支持
// 复合主键暂不支持（同步以单列主键为前提）
inline constexpr const char* E_SYNC_COMPOSITE_PK_NOT_SUPPORTED =
    "E_SYNC_COMPOSITE_PK_NOT_SUPPORTED";
inline constexpr const char* E_SYNC_ACK_TIMEOUT = "E_SYNC_ACK_TIMEOUT";  // 等待对端 ACK 超时
inline constexpr const char* E_SYNC_REBASE_FAILED = "E_SYNC_REBASE_FAILED";  // 变更重放/变基失败
inline constexpr const char* E_SYNC_BASELINE_FAILED =
    "E_SYNC_BASELINE_FAILED";  // 基线导出/导入失败

// ── 同步引擎 警告码（非阻断）────────────────────────────────────────────────
inline constexpr const char* W_SYNC_CONFLICT_REPLACED =
    "W_SYNC_CONFLICT_REPLACED";  // 冲突已按策略替换（旧值被覆盖）
inline constexpr const char* W_SYNC_BASELINE_LARGE = "W_SYNC_BASELINE_LARGE";  // 基线体积偏大
inline constexpr const char* W_SYNC_PAYLOAD_LARGE = "W_SYNC_PAYLOAD_LARGE";  // 单个变更包偏大
inline constexpr const char* W_SYNC_UNTRACKED_CHANGE =
    "W_SYNC_UNTRACKED_CHANGE";  // 检测到未经 session 捕获的直写改动
inline constexpr const char* W_SYNC_PEER_LAGGING =
    "W_SYNC_PEER_LAGGING";  // 某对端进度滞后（超过软阈值）
inline constexpr const char* W_SYNC_PUSH_ROW_DRIFTED =
    "W_SYNC_PUSH_ROW_DRIFTED";  // 推送的行在冻结后又被改动（漂移）
inline constexpr const char* W_SYNC_CONCURRENT_MANUAL_PUSH =
    "W_SYNC_CONCURRENT_MANUAL_PUSH";  // 检测到并发的手动推送

}  // namespace dbridge::err
