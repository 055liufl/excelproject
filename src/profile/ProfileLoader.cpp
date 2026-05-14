#include "ProfileLoader.h"

#include <QJsonArray>
#include <QRegularExpression>

namespace dbridge::detail {

static bool isSimpleIdentifier(const QString& s) {
    static QRegularExpression re(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
    return re.match(s).hasMatch();
}

static bool isTableDotColumn(const QString& s) {
    static QRegularExpression re(
        QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*\\.[A-Za-z_][A-Za-z0-9_]*$"));
    return re.match(s).hasMatch() || isSimpleIdentifier(s);
}

bool ProfileLoader::validateToken(const QString& token, QString* err) {
    static QRegularExpression lenRe(QStringLiteral("^len[<>]=\\d+$"));
    static QRegularExpression intGeRe(QStringLiteral("^int>=-?\\d+$"));
    static QRegularExpression dateRe(QStringLiteral("^date:.+$"));
    static QRegularExpression regexRe(QStringLiteral("^regex:.+$"));
    static QRegularExpression enumRe(QStringLiteral("^enum:.+$"));

    if (token == QStringLiteral("notNull") || token == QStringLiteral("int") ||
        token == QStringLiteral("decimal")) {
        return true;
    }
    if (lenRe.match(token).hasMatch())
        return true;
    if (intGeRe.match(token).hasMatch())
        return true;
    if (dateRe.match(token).hasMatch())
        return true;
    if (regexRe.match(token).hasMatch())
        return true;
    if (enumRe.match(token).hasMatch())
        return true;

    if (err)
        *err = QStringLiteral("Unknown validator token: ") + token;
    return false;
}

bool ProfileLoader::readColumn(const QString& dbCol, const QJsonObject& o, ColumnSpec* out,
                               QString* err) {
    if (!isSimpleIdentifier(dbCol)) {
        if (err)
            *err = QStringLiteral("column name is not a valid identifier: ") + dbCol;
        return false;
    }
    out->dbColumn = dbCol;
    out->source = o.value(QStringLiteral("source")).toString(dbCol);

    QJsonArray va = o.value(QStringLiteral("validators")).toArray();
    for (const auto& v : va) {
        QString token = v.toString();
        if (!validateToken(token, err))
            return false;
        out->validatorTokens.append(token);
    }
    return true;
}

bool ProfileLoader::readRoute(const QJsonObject& o, RouteSpec* out, QString* err) {
    out->table = o.value(QStringLiteral("table")).toString();
    if (out->table.isEmpty()) {
        if (err)
            *err = QStringLiteral("route missing 'table'");
        return false;
    }
    if (!isSimpleIdentifier(out->table)) {
        if (err)
            *err = QStringLiteral("route table is not a valid identifier: ") + out->table;
        return false;
    }

    out->parent = o.value(QStringLiteral("parent")).toString();

    // conflict
    QJsonObject conflictObj = o.value(QStringLiteral("conflict")).toObject();
    QJsonArray conflictCols = conflictObj.value(QStringLiteral("columns")).toArray();
    for (const auto& c : conflictCols) {
        QString col = c.toString();
        if (!isSimpleIdentifier(col)) {
            if (err)
                *err = QStringLiteral("conflict column is not a valid identifier: ") + col;
            return false;
        }
        out->conflict.columns.append(col);
    }

    // fkInject
    QJsonObject fkObj = o.value(QStringLiteral("fkInject")).toObject();
    if (!fkObj.isEmpty()) {
        FkInjectSpec fk;
        QString fromStr = fkObj.value(QStringLiteral("from")).toString();
        QString toStr = fkObj.value(QStringLiteral("to")).toString();
        // format: "table.column"
        auto splitDot = [](const QString& s, QString* tbl, QString* col, QString* errMsg) -> bool {
            int dot = s.indexOf('.');
            if (dot < 0) {
                if (errMsg)
                    *errMsg = QStringLiteral("fkInject requires 'table.column' format: ") + s;
                return false;
            }
            *tbl = s.left(dot);
            *col = s.mid(dot + 1);
            return true;
        };
        if (!splitDot(fromStr, &fk.fromTable, &fk.fromColumn, err))
            return false;
        if (!splitDot(toStr, &fk.toTable, &fk.toColumn, err))
            return false;
        if (fk.toTable != out->table) {
            if (err)
                *err = QStringLiteral("fkInject.to table must match route table: ") + fk.toTable;
            return false;
        }
        out->fkInject = fk;
    }

    // columns
    QJsonObject colsObj = o.value(QStringLiteral("columns")).toObject();
    for (auto it = colsObj.begin(); it != colsObj.end(); ++it) {
        ColumnSpec col;
        if (!readColumn(it.key(), it.value().toObject(), &col, err))
            return false;
        out->columns.append(col);
    }

    return true;
}

bool ProfileLoader::readSingleTable(const QJsonObject& o, ProfileSpec* out, QString* err) {
    out->mode = ProfileMode::SingleTable;

    RouteSpec route;
    route.table = o.value(QStringLiteral("table")).toString();
    if (route.table.isEmpty()) {
        if (err)
            *err = QStringLiteral("singleTable profile missing 'table'");
        return false;
    }
    if (!isSimpleIdentifier(route.table)) {
        if (err)
            *err = QStringLiteral("table is not a valid identifier");
        return false;
    }

    // conflict
    QJsonObject conflictObj = o.value(QStringLiteral("conflict")).toObject();
    QJsonArray conflictCols = conflictObj.value(QStringLiteral("columns")).toArray();
    for (const auto& c : conflictCols) {
        QString col = c.toString();
        if (!isSimpleIdentifier(col)) {
            if (err)
                *err = QStringLiteral("conflict column is not a valid identifier: ") + col;
            return false;
        }
        route.conflict.columns.append(col);
    }

    // columns
    QJsonObject colsObj = o.value(QStringLiteral("columns")).toObject();
    for (auto it = colsObj.begin(); it != colsObj.end(); ++it) {
        ColumnSpec col;
        if (!readColumn(it.key(), it.value().toObject(), &col, err))
            return false;
        route.columns.append(col);
    }

    out->routes.append(route);
    return true;
}

bool ProfileLoader::readMultiTable(const QJsonObject& o, ProfileSpec* out, QString* err) {
    out->mode = ProfileMode::MultiTable;

    QJsonArray routesArr = o.value(QStringLiteral("routes")).toArray();
    if (routesArr.isEmpty()) {
        if (err)
            *err = QStringLiteral("multiTable profile missing non-empty 'routes'");
        return false;
    }
    for (const auto& rv : routesArr) {
        RouteSpec route;
        if (!readRoute(rv.toObject(), &route, err))
            return false;
        out->routes.append(route);
    }
    return true;
}

bool ProfileLoader::readMixed(const QJsonObject& o, ProfileSpec* out, QString* err) {
    out->mode = ProfileMode::Mixed;

    QJsonObject discObj = o.value(QStringLiteral("discriminator")).toObject();
    out->discriminatorSource = discObj.value(QStringLiteral("source")).toString();
    if (out->discriminatorSource.isEmpty()) {
        if (err)
            *err = QStringLiteral("mixed profile missing 'discriminator.source'");
        return false;
    }

    QJsonArray classesArr = o.value(QStringLiteral("classes")).toArray();
    if (classesArr.isEmpty()) {
        if (err)
            *err = QStringLiteral("mixed profile missing non-empty 'classes'");
        return false;
    }

    for (const auto& cv : classesArr) {
        QJsonObject co = cv.toObject();
        ClassSpec cls;
        cls.id = co.value(QStringLiteral("id")).toString();
        if (cls.id.isEmpty()) {
            if (err)
                *err = QStringLiteral("class missing 'id'");
            return false;
        }
        QJsonObject matchObj = co.value(QStringLiteral("match")).toObject();
        cls.matchEquals = matchObj.value(QStringLiteral("equals")).toString();

        QJsonArray routesArr = co.value(QStringLiteral("routes")).toArray();
        if (routesArr.isEmpty()) {
            if (err)
                *err = QStringLiteral("class '") + cls.id + QStringLiteral("' has no routes");
            return false;
        }
        for (const auto& rv : routesArr) {
            RouteSpec route;
            if (!readRoute(rv.toObject(), &route, err))
                return false;
            cls.routes.append(route);
        }
        out->classes.append(cls);
    }

    // Check matchEquals uniqueness
    QHash<QString, QString> seen;
    for (const auto& cls : out->classes) {
        if (seen.contains(cls.matchEquals)) {
            if (err)
                *err = QStringLiteral("duplicate matchEquals '") + cls.matchEquals +
                       QStringLiteral("' in classes '") + seen[cls.matchEquals] +
                       QStringLiteral("' and '") + cls.id + QStringLiteral("'");
            return false;
        }
        seen[cls.matchEquals] = cls.id;
    }

    // mixed mode must NOT have explicitSql
    QJsonObject expObj = o.value(QStringLiteral("export")).toObject();
    if (expObj.contains(QStringLiteral("sql"))) {
        if (err)
            *err = QStringLiteral("mixed mode does not support export.sql");
        return false;
    }

    return true;
}

bool ProfileLoader::load(const QJsonDocument& doc, ProfileSpec* out, QString* err) {
    if (doc.isNull() || !doc.isObject()) {
        if (err)
            *err = QStringLiteral("Profile JSON is not an object");
        return false;
    }

    QJsonObject o = doc.object();

    out->name = o.value(QStringLiteral("profileName")).toString();
    if (out->name.isEmpty()) {
        if (err)
            *err = QStringLiteral("Profile missing 'profileName'");
        return false;
    }

    out->sheet = o.value(QStringLiteral("sheet")).toString();
    if (out->sheet.isEmpty()) {
        if (err)
            *err = QStringLiteral("Profile missing 'sheet'");
        return false;
    }

    out->headerRow = o.value(QStringLiteral("headerRow")).toInt(1);
    if (out->headerRow < 1) {
        if (err)
            *err = QStringLiteral("headerRow must be >= 1");
        return false;
    }

    QString modeStr = o.value(QStringLiteral("mode")).toString();
    bool ok = false;
    if (modeStr == QStringLiteral("singleTable")) {
        ok = readSingleTable(o, out, err);
    } else if (modeStr == QStringLiteral("multiTable")) {
        ok = readMultiTable(o, out, err);
    } else if (modeStr == QStringLiteral("mixed")) {
        ok = readMixed(o, out, err);
    } else {
        if (err)
            *err = QStringLiteral("Unknown profile mode '") + modeStr +
                   QStringLiteral("'; expected singleTable/multiTable/mixed");
        return false;
    }
    if (!ok)
        return false;

    // export spec (common)
    QJsonObject expObj = o.value(QStringLiteral("export")).toObject();
    QJsonArray orderByArr = expObj.value(QStringLiteral("orderBy")).toArray();
    for (const auto& v : orderByArr) {
        QString s = v.toString();
        if (!isTableDotColumn(s)) {
            if (err)
                *err = QStringLiteral("orderBy contains invalid identifier: ") + s;
            return false;
        }
        out->exportSpec.orderBy.append(s);
    }
    out->exportSpec.explicitSql = expObj.value(QStringLiteral("sql")).toString();
    out->exportSpec.classColumn = expObj.value(QStringLiteral("classColumn")).toString();

    return true;
}

}  // namespace dbridge::detail
