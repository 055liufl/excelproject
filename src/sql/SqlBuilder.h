#pragma once
#include <QString>
#include <QVector>

#include "mapping/RowPayload.h"
#include "profile/ProfileSpec.h"

// ============================================================================
// SqlBuilder.h — 安全地把「映射配置 / 行载荷」拼装成 SQL 文本（接口声明）
// ============================================================================
//
// 【这个文件是什么】
//   声明 SqlBuilder —— 一个无状态的「SQL 文本工厂」。它只负责把上游已经准备好的
//   结构化数据（路由载荷 RoutePayload、路由规格 RouteSpec、导出规格 ExportSpec）
//   翻译成可直接交给 Qt SQL 层执行的 SQL 字符串，自己不打开连接、不执行、不绑值。
//   实现见同目录 SqlBuilder.cpp（每个函数的转义/拼装细节在那里逐行讲解）。
//
// 【它构建哪两类 SQL】
//   · buildUpsert        —— 写库方向：INSERT ... ON CONFLICT(...) DO UPDATE/NOTHING
//                           （SQLite 的「有则更新、无则插入」UPSERT 写法）。
//   · buildAutoJoinSelect —— 导出方向：按多条路由的父子外键关系自动拼 LEFT JOIN 的
//                           SELECT，把分散在多张表里的列「拉平」成一张可导出的宽表。
//   两者都依赖第三个、也是最关键的一个工具：
//   · quoteIdent          —— 标识符引用：把表名/列名用双引号包裹并转义内部双引号。
//
// 【被谁使用】
//   · ETL 导入路径（service/ 下的 ImportService、mapping/ 下的 Mapper 等）调用
//     buildUpsert 得到写库语句；值通过占位符 `?` + bindOrder 在执行时绑定。
//   · ETL 导出路径（ExportService）调用 buildAutoJoinSelect 生成读库的 SELECT。
//   · 同步子系统（sync/ 下 apply、diff 等）在需要拼 SQL 时复用同样的 quoteIdent，
//     确保「标识符引用规则」全库唯一、一致（防注入与关键字冲突的统一闸口）。
//
// 【协作者】
//   · profile/ProfileSpec.h —— 提供 RouteSpec / ExportSpec / FkInjectSpec 等配置结构。
//   · mapping/RowPayload.h  —— 提供 RoutePayload（写一张表所需的列/值/冲突键）。
//   · 调用方自身的 Qt SQL 执行层 —— 真正去 prepare/bind/exec 本类产出的 SQL。
//
// 【命名空间】dbridge::detail —— 库内部实现细节，不作为公共 API 暴露。
// 【错误码】本类只拼字符串、不产出错误码；相关写库/导出失败码（E_DB_UPSERT /
//   E_EXPORT_QUERY 等）由真正执行 SQL 的调用方按结果产出，定义见 include/dbridge/Errors.h。
// ============================================================================

namespace dbridge::detail {

// ── UpsertSql —— buildUpsert 的产出：一条 UPSERT 语句 + 它的占位符绑定顺序 ──────
//   SQL 文本里用的是匿名占位符 `?`（位置参数），所以「第 i 个 ? 该绑哪一列的值」
//   这个信息无法从 SQL 字面量里读出，必须由 bindOrder 显式带出。调用方据此从
//   RoutePayload::binds 里按同样顺序取值逐个 bindValue，二者一一对应、绝不串位。
struct UpsertSql {
    QString sql;                 // 完整的 INSERT ... ON CONFLICT ... 语句文本
    QVector<QString> bindOrder;  // 占位符 ? 的绑定顺序：第 i 个 ? 对应这里第 i 个列名
                                 // column names in bind order（原注释：列名按绑定顺序排列）
};

// ── SqlBuilder —— 无状态 SQL 文本工厂（所有方法都是纯函数式的「输入→字符串」）──
class SqlBuilder {
   public:
    // 由一条路由载荷构建 UPSERT 语句。详见 .cpp 中实现处的逐行注释。
    // 输入：payload（目标表名 + 列名 + 冲突键，值不在这里用，仅用列名拼占位符顺序）。
    // 输出：UpsertSql（sql 文本 + bindOrder）；当 payload 无列时返回空壳（sql 为空）。
    UpsertSql buildUpsert(const RoutePayload& payload);

    // 由多条路由 + 导出规格构建「自动 JOIN」的导出 SELECT。详见 .cpp 实现处。
    // 输入：routes（routes[0] 为根表，其余按 fkInject 关系挂 LEFT JOIN）、exportSpec（排序等）。
    // 输出：SELECT ... FROM 根表 [LEFT JOIN ...] [ORDER BY ...] 文本；routes 为空时返回空串。
    QString buildAutoJoinSelect(const QVector<RouteSpec>& routes, const ExportSpec& exportSpec);

    // H-05 fix: escape an identifier for use in SQL (double-quote + escape internal quotes).
    // All table names and column names from profiles/catalogs must pass through this.
    // 【翻译保留原注释 + 扩展】H-05 修复：把一个 SQL 标识符（表名 / 列名）安全转义——
    //   外层加双引号、并把内部出现的每个双引号翻倍转义。所有来自 profile / 数据库目录
    //   的表名、列名都「必须」先经过本函数，绝不能直接拼进 SQL。
    // 【为什么必须经过它】① 防注入：名字里若含 `"`、`;`、空格等也只会被当成普通标识符
    //   字符，无法越权改变语句结构；② 防关键字冲突：像 order、select 这类 SQLite 关键字
    //   被引号包裹后即可合法用作表名/列名；③ 大小写/特殊字符按字面保留。
    // 【为什么是 static】它不依赖任何对象状态，是纯字符串变换，便于其它子系统直接
    //   `SqlBuilder::quoteIdent(...)` 复用同一套引用规则。
    static QString quoteIdent(const QString& name);
};

}  // namespace dbridge::detail
