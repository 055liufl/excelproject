#include "AutoProfileBuilder.h"

#include "dbridge/Errors.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace dbridge::detail {

bool AutoProfileBuilder::build(const TableInfo& table, ProfileSpec* out, QString* err) {
    out->name = QStringLiteral("auto_") + table.name;
    out->sheet = table.name;
    out->headerRow = 1;
    out->mode = ProfileMode::SingleTable;

    // Step 1: Choose conflict columns
    // Priority: non-autoincrement composite PK, then smallest UNIQUE index
    QStringList conflictCols;

    // Collect primary key columns
    QStringList pkCols;
    bool hasAutoInc = false;
    for (const auto& col : table.columns) {
        if (col.primaryKey) {
            pkCols.append(col.name);
            if (col.autoIncrement)
                hasAutoInc = true;
        }
    }

    if (!pkCols.isEmpty() && !hasAutoInc) {
        // Non-autoincrement PK: use all PK columns
        conflictCols = pkCols;
    } else if (!pkCols.isEmpty() && hasAutoInc && pkCols.size() > 1) {
        // Composite PK with autoincrement: use non-autoincrement PK columns
        for (const auto& col : table.columns) {
            if (col.primaryKey && !col.autoIncrement)
                conflictCols.append(col.name);
        }
    } else {
        // Try smallest UNIQUE index
        int minSize = INT_MAX;
        for (const auto& idx : table.indexes) {
            if (!idx.unique)
                continue;
            if (idx.columns.size() < minSize) {
                minSize = idx.columns.size();
                conflictCols = idx.columns;
            }
        }
    }

    // M-03 fix: instead of returning false when no conflict key is available, produce a
    // draft profile with executable=false and a descriptive issue entry. This lets callers
    // surface the mapping draft (column list) for the user to review and complete.
    // *err is NOT set here — a draft is not an error; callers check out->executable.
    if (conflictCols.isEmpty()) {
        out->executable = false;
        out->issues.append(QString::fromLatin1(err::E_PROFILE_NO_CONFLICT_KEY) +
                           QStringLiteral(": table '") + table.name +
                           QStringLiteral("' has no PRIMARY KEY or UNIQUE constraint; "
                                          "please add a unique constraint or specify "
                                          "conflict.columns manually"));
        // Fall through: populate column list in the draft so the caller can inspect the
        // field mapping and decide which columns to use as a conflict key.
    }

    RouteSpec route;
    route.table = table.name;
    route.conflict.columns = conflictCols;

    // Step 2: Select writable columns
    for (const auto& col : table.columns) {
        if (col.autoIncrement)
            continue;  // skip autoincrement PK
        if (col.generated)
            continue;  // skip generated columns

        ColumnSpec cs;
        cs.dbColumn = col.name;
        cs.source = col.name;  // Excel header = column name

        // Add validators based on type
        QString typeUp = col.declaredType.toUpper();
        if (col.notNull && !conflictCols.contains(col.name)) {
            cs.validatorTokens.append(QStringLiteral("notNull"));
        }
        if (typeUp.contains(QStringLiteral("INT"))) {
            cs.validatorTokens.append(QStringLiteral("int"));
        } else if (typeUp.contains(QStringLiteral("REAL")) ||
                   typeUp.contains(QStringLiteral("NUMERIC")) ||
                   typeUp.contains(QStringLiteral("DECIMAL"))) {
            cs.validatorTokens.append(QStringLiteral("decimal"));
        } else if (typeUp.contains(QStringLiteral("DATE"))) {
            cs.validatorTokens.append(QStringLiteral("date:yyyy-MM-dd"));
        }

        route.columns.append(cs);
    }

    out->routes.append(route);

    // Export spec
    if (!conflictCols.isEmpty()) {
        out->exportSpec.orderBy.append(conflictCols.first());
    }

    // Return true for both executable and draft profiles; callers check out->executable.
    return true;
}

QString AutoProfileBuilder::toJson(const ProfileSpec& profile) const {
    QJsonObject root;

    root.insert(QStringLiteral("profileName"), profile.name);
    root.insert(QStringLiteral("sheet"), profile.sheet);
    root.insert(QStringLiteral("headerRow"), profile.headerRow);
    root.insert(QStringLiteral("mode"), QStringLiteral("singleTable"));
    if (!profile.executable) {
        root.insert(QStringLiteral("executable"), false);
        QJsonArray issArr;
        for (const auto& iss : profile.issues)
            issArr.append(iss);
        root.insert(QStringLiteral("issues"), issArr);
    }

    if (!profile.routes.isEmpty()) {
        const RouteSpec& route = profile.routes[0];
        root.insert(QStringLiteral("table"), route.table);

        QJsonObject conflictObj;
        QJsonArray conflictCols;
        for (const auto& c : route.conflict.columns)
            conflictCols.append(c);
        conflictObj.insert(QStringLiteral("columns"), conflictCols);
        root.insert(QStringLiteral("conflict"), conflictObj);

        QJsonObject colsObj;
        for (const auto& col : route.columns) {
            QJsonObject colObj;
            colObj.insert(QStringLiteral("source"), col.source);
            if (!col.validatorTokens.isEmpty()) {
                QJsonArray va;
                for (const auto& t : col.validatorTokens)
                    va.append(t);
                colObj.insert(QStringLiteral("validators"), va);
            }
            colsObj.insert(col.dbColumn, colObj);
        }
        root.insert(QStringLiteral("columns"), colsObj);
    }

    QJsonObject exportObj;
    if (!profile.exportSpec.orderBy.isEmpty()) {
        QJsonArray ob;
        for (const auto& s : profile.exportSpec.orderBy)
            ob.append(s);
        exportObj.insert(QStringLiteral("orderBy"), ob);
    }
    root.insert(QStringLiteral("export"), exportObj);

    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

}  // namespace dbridge::detail
