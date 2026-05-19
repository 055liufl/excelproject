#include "ProfileLoader.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

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

    // fkInject — absent/null/[] are all no-op; object form is rejected
    {
        QJsonValue fkVal = o.value(QStringLiteral("fkInject"));
        if (!fkVal.isUndefined() && !fkVal.isNull()) {
            if (fkVal.isObject()) {
                if (err)
                    *err =
                        QStringLiteral("route '") + out->table +
                        QStringLiteral(
                            "': fkInject must be an array [{from,pairs:[[p_col,c_col],...]},...]; "
                            "old {\"from\":\"t.c\",\"to\":\"t.c\"} object form is removed");
                return false;
            }
            if (!fkVal.isArray()) {
                if (err)
                    *err = QStringLiteral("route '") + out->table +
                           QStringLiteral("': fkInject must be an array");
                return false;
            }
            for (const auto& fkElem : fkVal.toArray()) {
                QJsonObject fkObj = fkElem.toObject();
                FkInjectSpec fk;
                fk.fromTable = fkObj.value(QStringLiteral("from")).toString();
                if (fk.fromTable.isEmpty()) {
                    if (err)
                        *err = QStringLiteral("route '") + out->table +
                               QStringLiteral("': fkInject group missing 'from'");
                    return false;
                }
                if (!isSimpleIdentifier(fk.fromTable)) {
                    if (err)
                        *err = QStringLiteral("route '") + out->table +
                               QStringLiteral("': fkInject 'from' is not a valid identifier: ") +
                               fk.fromTable;
                    return false;
                }
                QJsonValue pairsVal = fkObj.value(QStringLiteral("pairs"));
                if (!pairsVal.isArray()) {
                    if (err)
                        *err = QStringLiteral("route '") + out->table +
                               QStringLiteral("': fkInject group from='") + fk.fromTable +
                               QStringLiteral("' missing 'pairs' array");
                    return false;
                }
                QJsonArray pairsArr = pairsVal.toArray();
                if (pairsArr.isEmpty()) {
                    if (err)
                        *err = QStringLiteral("route '") + out->table +
                               QStringLiteral("': fkInject group from='") + fk.fromTable +
                               QStringLiteral("' has empty 'pairs'");
                    return false;
                }
                for (const auto& pairElem : pairsArr) {
                    QJsonArray pair = pairElem.toArray();
                    if (pair.size() != 2) {
                        if (err)
                            *err =
                                QStringLiteral("route '") + out->table +
                                QStringLiteral("': fkInject pair must be [parent_col, child_col]");
                        return false;
                    }
                    QString parentCol = pair[0].toString();
                    QString childCol = pair[1].toString();
                    if (!isSimpleIdentifier(parentCol) || !isSimpleIdentifier(childCol)) {
                        if (err)
                            *err = QStringLiteral("route '") + out->table +
                                   QStringLiteral("': fkInject pair contains invalid identifier");
                        return false;
                    }
                    fk.pairs.append({parentCol, childCol});
                }
                out->fkInject.append(fk);
            }
        }
    }

    // lookups — absent/null are no-op; must be array if present
    {
        QJsonValue lookupsVal = o.value(QStringLiteral("lookups"));
        if (!lookupsVal.isUndefined() && !lookupsVal.isNull()) {
            if (!lookupsVal.isArray()) {
                if (err)
                    *err = QStringLiteral("route '") + out->table +
                           QStringLiteral("': 'lookups' must be an array");
                return false;
            }
            QSet<QString> seenLookupNames;
            for (const auto& lv : lookupsVal.toArray()) {
                QJsonObject lo = lv.toObject();
                LookupSpec lk;
                lk.name = lo.value(QStringLiteral("name")).toString().trimmed();
                if (lk.name.isEmpty()) {
                    if (err)
                        *err = QStringLiteral("route '") + out->table +
                               QStringLiteral("': lookup missing non-empty 'name'");
                    return false;
                }
                if (seenLookupNames.contains(lk.name)) {
                    if (err)
                        *err = QStringLiteral("route '") + out->table +
                               QStringLiteral("': duplicate lookup name '") + lk.name + '\'';
                    return false;
                }
                lk.fromTable = lo.value(QStringLiteral("from")).toString();
                if (lk.fromTable.isEmpty()) {
                    if (err)
                        *err = QStringLiteral("route '") + out->table +
                               QStringLiteral("': lookup '") + lk.name +
                               QStringLiteral("' missing 'from'");
                    return false;
                }

                // match — must be [[G_col, excel_header],...] array, not object
                QJsonValue matchVal = lo.value(QStringLiteral("match"));
                if (matchVal.isObject()) {
                    if (err)
                        *err = QStringLiteral("route '") + out->table +
                               QStringLiteral("': lookup '") + lk.name +
                               QStringLiteral(
                                   "' match must be [[G_col,excel_header],...], not object");
                    return false;
                }
                if (!matchVal.isArray() || matchVal.toArray().isEmpty()) {
                    if (err)
                        *err = QStringLiteral("route '") + out->table +
                               QStringLiteral("': lookup '") + lk.name +
                               QStringLiteral("' missing non-empty 'match' array");
                    return false;
                }
                for (const auto& mv : matchVal.toArray()) {
                    QJsonArray pair = mv.toArray();
                    if (pair.size() != 2) {
                        if (err)
                            *err = QStringLiteral("route '") + out->table +
                                   QStringLiteral("': lookup '") + lk.name +
                                   QStringLiteral("' match entry must be [G_column, excel_header]");
                        return false;
                    }
                    lk.match.append({pair[0].toString(), pair[1].toString()});
                }

                // select — must be [[G_col, target_dbColumn],...] array, not object
                QJsonValue selectVal = lo.value(QStringLiteral("select"));
                if (selectVal.isObject()) {
                    if (err)
                        *err = QStringLiteral("route '") + out->table +
                               QStringLiteral("': lookup '") + lk.name +
                               QStringLiteral(
                                   "' select must be [[G_col,target_dbColumn],...], not object");
                    return false;
                }
                if (!selectVal.isArray() || selectVal.toArray().isEmpty()) {
                    if (err)
                        *err = QStringLiteral("route '") + out->table +
                               QStringLiteral("': lookup '") + lk.name +
                               QStringLiteral("' missing non-empty 'select' array");
                    return false;
                }
                for (const auto& sv : selectVal.toArray()) {
                    QJsonArray pair = sv.toArray();
                    if (pair.size() != 2) {
                        if (err)
                            *err = QStringLiteral("route '") + out->table +
                                   QStringLiteral("': lookup '") + lk.name +
                                   QStringLiteral(
                                       "' select entry must be [G_column, target_dbColumn]");
                        return false;
                    }
                    lk.select.append({pair[0].toString(), pair[1].toString()});
                }

                seenLookupNames.insert(lk.name);
                out->lookups.append(lk);
            }
        }
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

    // Validate required fields before delegating
    QString table = o.value(QStringLiteral("table")).toString();
    if (table.isEmpty()) {
        if (err)
            *err = QStringLiteral("singleTable profile missing 'table'");
        return false;
    }

    // Delegate to readRoute so lookups/fkInject are also parsed
    RouteSpec route;
    if (!readRoute(o, &route, err))
        return false;

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
