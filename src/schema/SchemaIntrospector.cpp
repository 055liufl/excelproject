#include "SchemaIntrospector.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace dbridge::detail {

bool SchemaIntrospector::readTables(QSqlDatabase& db, QStringList* tables, QString* err) {
    QSqlQuery q(db);
    // L-04 fix: exclude __sync_* meta-tables so the schema catalog only contains user tables.
    // Profile/export logic should never bind to internal sync state tables.
    if (!q.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type='table' "
                               "AND name NOT LIKE 'sqlite_%' AND name NOT LIKE '__sync_%'"))) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    while (q.next()) {
        tables->append(q.value(0).toString());
    }
    return true;
}

bool SchemaIntrospector::readColumns(QSqlDatabase& db, TableInfo* info, QString* err) {
    QSqlQuery q(db);
    // table_xinfo gives hidden column info (generated columns have hidden > 0).
    // M-01 fix: use double-quote identifier quoting instead of single-quote string literal so
    // table names with embedded double-quotes, spaces, or reserved words are handled correctly.
    const QString quotedName =
        QStringLiteral("\"") +
        QString(info->name).replace(QLatin1Char('"'), QLatin1String("\"\"")) + QStringLiteral("\"");
    QString sql = QStringLiteral("PRAGMA table_xinfo(") + quotedName + QLatin1Char(')');
    if (!q.exec(sql)) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    while (q.next()) {
        // cid, name, type, notnull, dflt_value, pk, hidden
        ColumnInfo col;
        col.name = q.value(1).toString();
        col.declaredType = q.value(2).toString();
        col.notNull = q.value(3).toBool();
        col.defaultValue = q.value(4).toString();
        int pk = q.value(5).toInt();
        if (pk > 0) {
            col.primaryKey = true;
            col.pkOrder = pk;
        }
        int hidden = q.value(6).toInt();
        col.generated = (hidden == 2 || hidden == 3);  // VIRTUAL or STORED generated

        info->columns.append(col);
    }

    // Check autoIncrement per column
    for (auto& col : info->columns) {
        if (col.primaryKey && col.pkOrder == 1) {
            if (col.declaredType.toUpper() == QStringLiteral("INTEGER")) {
                col.autoIncrement = isAutoIncrement(db, info->name, col.name);
            }
        }
    }
    return true;
}

bool SchemaIntrospector::isAutoIncrement(QSqlDatabase& db, const QString& tableName,
                                         const QString& colName) {
    Q_UNUSED(colName)
    // SQLite AUTOINCREMENT is indicated by presence of AUTOINCREMENT keyword in CREATE TABLE
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT sql FROM sqlite_master WHERE type='table' AND name=?"));
    q.addBindValue(tableName);
    if (!q.exec() || !q.next())
        return false;
    QString sql = q.value(0).toString().toUpper();
    return sql.contains(QStringLiteral("AUTOINCREMENT"));
}

bool SchemaIntrospector::readIndexes(QSqlDatabase& db, TableInfo* info, QString* err) {
    QSqlQuery q(db);
    // M-01 fix: use double-quote identifier quoting.
    const QString quotedTableName =
        QStringLiteral("\"") +
        QString(info->name).replace(QLatin1Char('"'), QLatin1String("\"\"")) + QStringLiteral("\"");
    QString sql = QStringLiteral("PRAGMA index_list(") + quotedTableName + QLatin1Char(')');
    if (!q.exec(sql)) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    // seq, name, unique, origin, partial
    QVector<QPair<QString, bool>> idxList;
    while (q.next()) {
        QString idxName = q.value(1).toString();
        bool unique = q.value(2).toBool();
        idxList.append({idxName, unique});
    }

    for (auto& [idxName, unique] : idxList) {
        IndexInfo idx;
        idx.name = idxName;
        idx.unique = unique;

        QSqlQuery qi(db);
        // M-01 fix: double-quote index name.
        const QString quotedIdx =
            QStringLiteral("\"") +
            QString(idxName).replace(QLatin1Char('"'), QLatin1String("\"\"")) +
            QStringLiteral("\"");
        qi.exec(QStringLiteral("PRAGMA index_info(") + quotedIdx + QLatin1Char(')'));
        while (qi.next()) {
            // seqno, cid, name
            idx.columns.append(qi.value(2).toString());
        }
        info->indexes.append(idx);
    }
    return true;
}

bool SchemaIntrospector::readForeignKeys(QSqlDatabase& db, TableInfo* info, QString* err) {
    QSqlQuery q(db);
    // M-01 fix: double-quote identifier.
    const QString quotedName2 =
        QStringLiteral("\"") +
        QString(info->name).replace(QLatin1Char('"'), QLatin1String("\"\"")) + QStringLiteral("\"");
    QString sql = QStringLiteral("PRAGMA foreign_key_list(") + quotedName2 + QLatin1Char(')');
    if (!q.exec(sql)) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    // id, seq, table, from, to, on_update, on_delete, match
    while (q.next()) {
        FkInfo fk;
        fk.refTable = q.value(2).toString();
        fk.fromColumn = q.value(3).toString();
        fk.toColumn = q.value(4).toString();
        info->foreignKeys.append(fk);
    }
    return true;
}

bool SchemaIntrospector::load(QSqlDatabase& db, SchemaCatalog* out, QString* err) {
    out->clear();

    QStringList tables;
    if (!readTables(db, &tables, err))
        return false;

    for (const auto& tname : tables) {
        TableInfo info;
        info.name = tname;
        if (!readColumns(db, &info, err))
            return false;
        if (!readIndexes(db, &info, err))
            return false;
        if (!readForeignKeys(db, &info, err))
            return false;
        out->addTable(info);
    }
    return true;
}

}  // namespace dbridge::detail
