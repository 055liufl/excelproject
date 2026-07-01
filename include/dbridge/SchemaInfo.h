#pragma once
#include "dbridge/Export.h"

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <algorithm>

// ============================================================================
// SchemaInfo.h — 数据库表结构（schema）的「对外公共」值类型
// ============================================================================
//
// 【这个文件是什么 / 为什么存在】
//   dbridge 在内部（src/schema/SchemaCatalog.h，detail 命名空间）早已把「库里有哪些
//   表、每张表有哪些列/主键」自省成结构体缓存起来。但那是库内部实现细节（detail::），
//   不适合让最终调用方直接依赖。本文件把「调用方真正需要的那一小部分 schema 事实」
//   提炼成两个稳定、公开、可跨模块传递的值类型（POD 风格），作为 DataBridge 新增的
//   schema 发现接口（userTables / describeTable，见 DataBridge.h）的返回载体。
//
// 【典型用途】
//   调用方想「动态发现库里有哪些表、每张表的列序与主键」而**不必自己写 PRAGMA/
//   sqlite_master 查询**——直接：
//     QStringList tables = bridge.userTables();
//     dbridge::TableSchema ts; bridge.describeTable(tables.first(), &ts);
//     for (const auto& c : ts.columns) { ... }        // 按建表列序遍历
//     QStringList pk = ts.primaryKeyColumns();          // 主键列（按复合主键次序）
//
// 【设计取向】纯数据、无行为（仅一个便捷查询方法），值语义可自由拷贝/跨线程传递。
// ============================================================================

namespace dbridge {

// ── ColumnDef —— 一张表里「一列」的对外结构描述 ──────────────────────────────
//   对应 SQLite `PRAGMA table_info` 自省出来的一列，仅保留调用方常用的字段。
struct DBRIDGE_EXPORT ColumnDef {
    QString name;          // 列名（表内唯一）
    QString declaredType;  // CREATE TABLE 中声明的类型文本（如 "INTEGER"/"TEXT"，可能为空）
    bool notNull = false;     // 是否带 NOT NULL 约束
    bool primaryKey = false;  // 是否为主键的一部分（复合主键中任一列均为 true）
    int pkOrder = 0;  // 主键内 1-based 次序（1=主键第 1 列…）；非主键列为 0
};

// ── TableSchema —— 一张表的对外结构（表名 + 有序列集）──────────────────────────
struct DBRIDGE_EXPORT TableSchema {
    QString table;             // 表名
    QList<ColumnDef> columns;  // 全部列，按建表时的列序（cid 自然序）

    // primaryKeyColumns —— 取主键列名，按复合主键次序（pkOrder 升序）排列。
    //   无显式主键的表返回空列表；单列主键返回单元素列表。纯查询、无副作用。
    QStringList primaryKeyColumns() const {
        // 先收集 (pkOrder, name) 再按 pkOrder 排序，保证复合主键列序稳定正确。
        QList<QPair<int, QString>> pk;
        for (const ColumnDef& c : columns)
            if (c.primaryKey)
                pk.append(qMakePair(c.pkOrder, c.name));
        std::sort(pk.begin(), pk.end(),
                  [](const QPair<int, QString>& a, const QPair<int, QString>& b) {
                      return a.first < b.first;
                  });
        QStringList out;
        for (const auto& p : pk)
            out.append(p.second);
        return out;
    }
};

}  // namespace dbridge
