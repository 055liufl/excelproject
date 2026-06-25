#pragma once
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

namespace dbridge::sync {

// 在 session attach 前检查每张同步表是否满足 UPSERT 合法条件。
// 拒绝：虚表、视图、影子表、无显式非空PK、无可用冲突目标。
class SchemaEligibility {
   public:
    // 逐表校验。rejected 填不合格表名+原因，返回 false 表示有不合格项。
    static bool verify(QSqlDatabase& db, const QStringList& syncTables, QStringList* rejected,
                       QString* err);

   private:
    struct TableInfo {
        bool exists = false;
        bool isView = false;
        bool isVirtual = false;
        bool isShadow = false;   // 名称含 "_" 且被 FTS/R*Tree 引用
        QStringList pkCols;      // PK 列，按 pk 序号排列
        bool pkNotNull = false;  // 所有 PK 列均 NOT NULL
    };

    static TableInfo introspect(QSqlDatabase& db, const QString& table, QString* err);

    // 检查是否存在非 partial 的 UNIQUE 索引可做 ON CONFLICT 目标（PK 本身就够）
    static bool hasUpsertTarget(QSqlDatabase& db, const QString& table, QString* err);

    // 影子表判断：sqlite_master 中有 CREATE VIRTUAL TABLE 的表名是该表前缀
    static bool isShadowTable(QSqlDatabase& db, const QString& table, QString* err);
};

}  // namespace dbridge::sync
