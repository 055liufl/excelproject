#pragma once
#include "dbridge/Types.h"

#include <QSqlDatabase>

#include "profile/ProfileSpec.h"
#include "schema/SchemaCatalog.h"

// ============================================================================
// ExportService.h — SQLite→Excel「导出编排层」的入口声明
// ============================================================================
//
// 【它在整套 ETL 里的位置】
//   ExportService 是导出方向的“总指挥”，与 ImportService 互为镜像：根据 Profile
//   生成 SELECT（或用 Profile 提供的原生 SQL）查库 → 可选地做“反向查找”（把存进库
//   的代理主键换回用户认得的业务键）→ 时间格式转换 → 按 columnOrder 写出 .xlsx。
//   实现与各路径的逐行讲解见 ExportService.cpp。
//
// 【三条写出路径（按是否需要列重排 / 反向查找分流，细节见 .cpp）】
//   1) 流式路径：无 columnOrder 且无反向查找 —— 边查边写，零额外内存（最省）。
//   2) 仅列重排：有 columnOrder 但无反向查找 —— 全量载入内存、按列序重排后写出。
//   3) 反向查找：扩展 SELECT 取出 H 列 → 预取反向缓存 → 用业务键投影替换 → 写出。
//   Mixed（混合多类）模式另有一条按 class 分别查询、再合并表头统一写出的分支。
//
// 【关键概念速查】反向查找（代理主键 H → 业务键 A）；exportRoundtrip（是否启用替换）；
//   exportOnMissing（零命中时 error/null/skip 三种处置）；NOT_FOUND/AMBIGUOUS；
//   columnOrder（导出列顺序，重排见 ExportHelpers.h）；时间 V→U 转换。错误码见 Errors.h。
// ============================================================================

namespace dbridge::detail {

class ExportService {
   public:
    // 执行一次完整导出。
    // 参数：
    //   profile  —— ETL 映射配置（路由/列映射/查找/导出列序/原生 SQL 等）。
    //   catalog  —— 数据库表结构目录，供反向查找的 affinity 强转与列定位。
    //   xlsxPath —— 输出 .xlsx 文件路径。
    //   options  —— 导出选项（sheet 名等，见 Types.h）。
    //   db       —— 已打开的 SQLite 连接；本服务在其上执行 SELECT 与反向查找预取。
    // 返回：ExportResult（ok、写出行数 writtenRows、errors/warnings）。
    // 错误模式：表级错误（SELECT 失败 E_EXPORT_QUERY、反向预取失败
    //   E_REVERSE_LOOKUP_QUERY_FAILED、拓扑成环、写文件失败 E_WRITE_XLSX）→ 立即返回；
    //   行级错误（反向查找 NOT_FOUND/AMBIGUOUS、时间解析失败 E_TIME_PARSE_DB）按各自策略处置。
    // 线程：调用线程内同步执行；与 db 同线程使用。无自管事务（导出只读，不需要写事务）。
    ExportResult run(const ProfileSpec& profile, const SchemaCatalog& catalog,
                     const QString& xlsxPath, const ExportOptions& options, QSqlDatabase& db);
};

}  // namespace dbridge::detail
