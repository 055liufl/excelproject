#pragma once
#include <QSqlDatabase>

#include "SchemaCatalog.h"

// ============================================================================
// SchemaIntrospector.h — 通过 SQLite 自省填充 SchemaCatalog 的「读取器」
// ============================================================================
//
// 【这个文件是什么】
//   声明一个无状态的工具类 SchemaIntrospector：给它一个已打开的 SQLite 连接，
//   它执行一系列 PRAGMA 自省语句，把真实库结构读出来、装进一个 SchemaCatalog。
//   它是 schema 目录（SchemaCatalog.h 所定义的数据模型）的唯一「生产者」。
//
// 【在 ETL 管线中的位置】
//   连接数据库 → 【SchemaIntrospector::load】把结构读进 SchemaCatalog
//   → 路由 / 外键查找 / 拓扑排序 等环节查询该目录 → 导入/导出。
//   本类只负责「读结构」这一步，读完即交棒，自身不保存任何状态。
//
// 【用到的 SQLite 自省手段（实现细节见 .cpp）】
//   · sqlite_master            —— 列出所有表、取建表 SQL 原文（识别 AUTOINCREMENT）。
//   · PRAGMA table_xinfo(T)     —— 列出表 T 的列（含隐藏/生成列）。
//   · PRAGMA index_list(T)      —— 列出表 T 的索引（含 unique / partial 标志）。
//   · PRAGMA index_info(I)      —— 列出索引 I 覆盖的列。
//   · PRAGMA foreign_key_list(T)—— 列出表 T 的外键关系。
//
// 【协作者】
//   · SchemaCatalog / TableInfo / ColumnInfo / IndexInfo / FkInfo（同目录头）——
//     本类的输出目标与数据载体。
//   · QSqlDatabase / QSqlQuery（Qt SQL 模块）—— 执行查询的底层手段。
//
// 命名空间 dbridge::detail：库内部实现细节。
// ============================================================================

namespace dbridge::detail {

// SchemaIntrospector —— schema 自省读取器（无成员状态，纯函数式工具类）。
class SchemaIntrospector {
   public:
    // load —— 顶层入口：自省整库结构并填满 out 目录。
    //   做什么：先清空 out，再列出所有用户表，逐表读取「列 / 索引 / 外键」并加入 out。
    //   参数 db ：已打开的 SQLite 连接（引用传入；本函数只读不改库）。
    //   参数 out：[出参] 待填充的目录，非空；函数开头会先 clear()。
    //   参数 err：[出参，可空] 失败时写入底层 SQL 错误文本；传 nullptr 表示不关心详情。
    //   返回：全程成功为 true；任一 PRAGMA 执行失败立即返回 false（部分结果可能已写入 out）。
    //   副作用：清空并改写 *out；不修改数据库。
    bool load(QSqlDatabase& db, SchemaCatalog* out, QString* err);

   private:
    // 以下均为 load 的分步私有助手，各自负责一类自省，约定：成功 true / 失败 false 并写 err。

    // readTables —— 列出库中所有「用户表」表名（排除 sqlite_ 与 __sync_ 内部表）。
    //   出参 tables：追加写入查到的表名。
    bool readTables(QSqlDatabase& db, QStringList* tables, QString* err);
    // readColumns —— 读 info->name 这张表的所有列，填入 info->columns；
    //   并额外判定单列整型主键是否 AUTOINCREMENT。
    bool readColumns(QSqlDatabase& db, TableInfo* info, QString* err);
    // readIndexes —— 读该表所有索引（含 unique/partial 标志与各自覆盖的列），填 info->indexes。
    bool readIndexes(QSqlDatabase& db, TableInfo* info, QString* err);
    // readForeignKeys —— 读该表所有外键关系，填 info->foreignKeys。
    bool readForeignKeys(QSqlDatabase& db, TableInfo* info, QString* err);
    // isAutoIncrement —— 判断某表是否使用了 AUTOINCREMENT（靠扫描建表 SQL 原文里的关键字）。
    //   返回 bool（无 err 出参，查询失败即视为 false）。
    bool isAutoIncrement(QSqlDatabase& db, const QString& tableName, const QString& colName);
};

}  // namespace dbridge::detail
