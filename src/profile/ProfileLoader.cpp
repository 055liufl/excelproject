#include "ProfileLoader.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

namespace dbridge::detail {

namespace {

// add-time-format-profile: per-slot token validation.
// Date slots reject time tokens; time slots reject date tokens; datetime is unconstrained.
// Tokens listed verbatim from spec "Format token validation per slot type".
bool slotFormatTokensOk(TemporalSlotKind kind, const QString& fmt, QString* offendingToken) {
    if (fmt.isEmpty())
        return true;
    QStringList banned;
    if (kind == TemporalSlotKind::Date) {
        // Time-related tokens forbidden on a Date slot.
        banned << QStringLiteral("HH") << QStringLiteral("H") << QStringLiteral("hh")
               << QStringLiteral("h") << QStringLiteral("mm") << QStringLiteral("m")
               << QStringLiteral("ss") << QStringLiteral("s") << QStringLiteral("zzz")
               << QStringLiteral("z") << QStringLiteral("AP") << QStringLiteral("ap")
               << QStringLiteral("A") << QStringLiteral("a") << QStringLiteral("t");
    } else if (kind == TemporalSlotKind::Time) {
        // Date tokens forbidden on a Time slot.
        banned << QStringLiteral("yyyy") << QStringLiteral("yy") << QStringLiteral("y")
               << QStringLiteral("MMMM") << QStringLiteral("MMM") << QStringLiteral("MM")
               << QStringLiteral("M") << QStringLiteral("dddd") << QStringLiteral("ddd")
               << QStringLiteral("dd") << QStringLiteral("d");
    } else {
        return true;  // DateTime / None — unconstrained
    }
    // Longest-first so multi-char tokens are matched before their substrings.
    std::sort(banned.begin(), banned.end(),
              [](const QString& a, const QString& b) { return a.length() > b.length(); });
    // Walk the format and skip quoted segments ('text' is literal in Qt date format).
    bool inQuote = false;
    for (int i = 0; i < fmt.size(); ++i) {
        QChar c = fmt[i];
        if (c == QLatin1Char('\'')) {
            inQuote = !inQuote;
            continue;
        }
        if (inQuote)
            continue;
        for (const QString& tk : banned) {
            if (fmt.midRef(i, tk.size()) == tk) {
                if (offendingToken)
                    *offendingToken = tk;
                return false;
            }
        }
    }
    return true;
}

const char* slotKindName(TemporalSlotKind kind) {
    switch (kind) {
        case TemporalSlotKind::Date:
            return "dateFormat";
        case TemporalSlotKind::DateTime:
            return "datetimeFormat";
        case TemporalSlotKind::Time:
            return "timeFormat";
        case TemporalSlotKind::None:
        default:
            return "(none)";
    }
}

// Parse a single side sub-object { type?, format?, fallback? } into TemporalSideSpec.
// null value → E_PROFILE_PARSE. absent/undefined → undeclared (returns true, out unchanged).
bool parseTemporalSide(const QJsonValue& v, TemporalSlotKind kind, const QString& ownerLabel,
                       const QString& sideName, TemporalSideSpec* out, QString* err) {
    if (v.isUndefined())
        return true;
    if (v.isNull()) {
        if (err)
            *err = ownerLabel + QStringLiteral(": ") + sideName +
                   QStringLiteral(" must be an object, not null");
        return false;
    }
    if (!v.isObject()) {
        if (err)
            *err = ownerLabel + QStringLiteral(": ") + sideName +
                   QStringLiteral(" must be a JSON object");
        return false;
    }
    QJsonObject o = v.toObject();
    out->declared = true;

    // type: absent/"" → "string"
    QJsonValue typeVal = o.value(QStringLiteral("type"));
    QString typeStr = QStringLiteral("string");
    if (!typeVal.isUndefined() && !typeVal.isNull()) {
        typeStr = typeVal.toString();
    } else if (typeVal.isNull()) {
        if (err)
            *err = ownerLabel + QStringLiteral(": ") + sideName +
                   QStringLiteral(".type must not be null");
        return false;
    }
    auto physType = temporalPhysTypeFromString(typeStr);
    if (!physType.has_value()) {
        if (err)
            *err = ownerLabel + QStringLiteral(": ") + sideName + QStringLiteral(".type='") +
                   typeStr +
                   QStringLiteral("' is not recognized; allowed: \"string\", \"epochSec\"");
        return false;
    }
    out->type = physType.value();

    // format: null → error; absent/"" → empty (valid for epochSec)
    QJsonValue fmtVal = o.value(QStringLiteral("format"));
    if (fmtVal.isNull()) {
        if (err)
            *err = ownerLabel + QStringLiteral(": ") + sideName +
                   QStringLiteral(".format must not be null");
        return false;
    }
    out->format = fmtVal.toString();  // absent → empty string

    // fallback: null → error; absent/[] → empty list
    QJsonValue fbVal = o.value(QStringLiteral("fallback"));
    if (fbVal.isNull()) {
        if (err)
            *err = ownerLabel + QStringLiteral(": ") + sideName +
                   QStringLiteral(".fallback must not be null");
        return false;
    }
    if (!fbVal.isUndefined()) {
        if (!fbVal.isArray()) {
            if (err)
                *err = ownerLabel + QStringLiteral(": ") + sideName +
                       QStringLiteral(".fallback must be a JSON array of format strings");
            return false;
        }
        for (const auto& fv : fbVal.toArray()) {
            QString s = fv.toString();
            if (s.isEmpty()) {
                if (err)
                    *err = ownerLabel + QStringLiteral(": ") + sideName +
                           QStringLiteral(".fallback contains empty string");
                return false;
            }
            out->fallback.append(s);
        }
    }

    // Qt token validation (only for type=string)
    if (out->type == TemporalPhysType::String) {
        QString offending;
        if (!slotFormatTokensOk(kind, out->format, &offending)) {
            if (err)
                *err = ownerLabel + QStringLiteral(": ") + sideName +
                       QStringLiteral(".format contains forbidden token '") + offending +
                       QStringLiteral("' for this slot type");
            return false;
        }
        for (const QString& s : qAsConst(out->fallback)) {
            if (!slotFormatTokensOk(kind, s, &offending)) {
                if (err)
                    *err = ownerLabel + QStringLiteral(": ") + sideName +
                           QStringLiteral(".fallback entry '") + s +
                           QStringLiteral("' contains forbidden token '") + offending + '\'';
                return false;
            }
        }
    }

    return true;
}

// Parse a single temporal slot object. Supports:
//   Legacy form: { excelFormat, dbFormat?, excelFormatFallback? }
//   New form:    { excel?: {type,format,fallback}, db?: {type,format,fallback} }
// Mixed (both forms in same object) → E_PROFILE_PARSE.
bool readTemporalSlot(const QJsonValue& v, TemporalSlotKind kind, const QString& ownerLabel,
                      TemporalFormatSpec* out, QString* err) {
    if (v.isUndefined() || v.isNull())
        return true;
    if (!v.isObject()) {
        if (err)
            *err = ownerLabel + QStringLiteral(": ") + QLatin1String(slotKindName(kind)) +
                   QStringLiteral(" must be a JSON object");
        return false;
    }
    QJsonObject o = v.toObject();
    out->declared = true;

    bool hasLegacy = o.contains(QStringLiteral("excelFormat")) ||
                     o.contains(QStringLiteral("dbFormat")) ||
                     o.contains(QStringLiteral("excelFormatFallback"));
    bool hasNew = o.contains(QStringLiteral("excel")) || o.contains(QStringLiteral("db"));

    if (hasLegacy && hasNew) {
        if (err)
            *err = ownerLabel + QStringLiteral(": ") + QLatin1String(slotKindName(kind)) +
                   QStringLiteral(
                       ": cannot mix legacy fields (excelFormat/dbFormat/excelFormatFallback)"
                       " with new sub-objects (excel/db) in the same slot object");
        return false;
    }

    if (hasLegacy) {
        // Normalize legacy form → new form (type=string).
        // A side is declared only if the corresponding legacy key is explicitly present.
        // This preserves the original behavior: absent excelFormat/dbFormat inherits from profile.
        bool hasExcelFormat = o.contains(QStringLiteral("excelFormat"));
        bool hasDbFormat = o.contains(QStringLiteral("dbFormat"));
        bool hasFallback = o.contains(QStringLiteral("excelFormatFallback"));

        if (hasExcelFormat || hasFallback) {
            TemporalSideSpec excelSide;
            excelSide.declared = true;
            excelSide.type = TemporalPhysType::String;
            excelSide.format = o.value(QStringLiteral("excelFormat")).toString();

            QJsonValue fbVal = o.value(QStringLiteral("excelFormatFallback"));
            if (!fbVal.isUndefined() && !fbVal.isNull()) {
                if (!fbVal.isArray()) {
                    if (err)
                        *err = ownerLabel + QStringLiteral(": ") +
                               QLatin1String(slotKindName(kind)) +
                               QStringLiteral(".excelFormatFallback must be a JSON array");
                    return false;
                }
                for (const auto& fv : fbVal.toArray()) {
                    QString s = fv.toString();
                    if (s.isEmpty()) {
                        if (err)
                            *err = ownerLabel + QStringLiteral(": ") +
                                   QLatin1String(slotKindName(kind)) +
                                   QStringLiteral(".excelFormatFallback contains empty string");
                        return false;
                    }
                    QString offending;
                    if (!slotFormatTokensOk(kind, s, &offending)) {
                        if (err)
                            *err = ownerLabel + QStringLiteral(": ") +
                                   QLatin1String(slotKindName(kind)) +
                                   QStringLiteral(".excelFormatFallback entry '") + s +
                                   QStringLiteral("' contains forbidden token '") + offending +
                                   '\'';
                        return false;
                    }
                    excelSide.fallback.append(s);
                }
            }

            QString offending;
            if (!slotFormatTokensOk(kind, excelSide.format, &offending)) {
                if (err)
                    *err = ownerLabel + QStringLiteral(": ") + QLatin1String(slotKindName(kind)) +
                           QStringLiteral(".excelFormat contains forbidden token '") + offending +
                           QStringLiteral("' for this slot type");
                return false;
            }
            out->excel = excelSide;
        }

        if (hasDbFormat) {
            TemporalSideSpec dbSide;
            dbSide.declared = true;
            dbSide.type = TemporalPhysType::String;
            dbSide.format = o.value(QStringLiteral("dbFormat")).toString();

            QString offending;
            if (!slotFormatTokensOk(kind, dbSide.format, &offending)) {
                if (err)
                    *err = ownerLabel + QStringLiteral(": ") + QLatin1String(slotKindName(kind)) +
                           QStringLiteral(".dbFormat contains forbidden token '") + offending +
                           QStringLiteral("' for this slot type");
                return false;
            }
            out->db = dbSide;
        }
    } else {
        // New form: parse excel/db sub-objects (either may be absent → undeclared)
        if (!parseTemporalSide(
                o.value(QStringLiteral("excel")), kind,
                ownerLabel + QStringLiteral(": ") + QLatin1String(slotKindName(kind)),
                QStringLiteral("excel"), &out->excel, err))
            return false;
        if (!parseTemporalSide(
                o.value(QStringLiteral("db")), kind,
                ownerLabel + QStringLiteral(": ") + QLatin1String(slotKindName(kind)),
                QStringLiteral("db"), &out->db, err))
            return false;
    }

    return true;
}

}  // namespace

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

    // add-time-format-profile: per-column temporal slots (override profile defaults
    // field-by-field).
    QString colLabel = QStringLiteral("column '") + dbCol + '\'';
    if (!readTemporalSlot(o.value(QStringLiteral("dateFormat")), TemporalSlotKind::Date, colLabel,
                          &out->dateFormat, err))
        return false;
    if (!readTemporalSlot(o.value(QStringLiteral("datetimeFormat")), TemporalSlotKind::DateTime,
                          colLabel, &out->datetimeFormat, err))
        return false;
    if (!readTemporalSlot(o.value(QStringLiteral("timeFormat")), TemporalSlotKind::Time, colLabel,
                          &out->timeFormat, err))
        return false;

    return true;
}

bool ProfileLoader::readRoute(const QJsonObject& o, RouteSpec* out, QString* err,
                              QStringList* warnings) {
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

                // add-export-reverse-lookup: exportRoundtrip (default true)
                QJsonValue ertVal = lo.value(QStringLiteral("exportRoundtrip"));
                if (!ertVal.isUndefined() && !ertVal.isNull()) {
                    if (!ertVal.isBool()) {
                        if (err)
                            *err = QStringLiteral("route '") + out->table +
                                   QStringLiteral("': lookup '") + lk.name +
                                   QStringLiteral("': exportRoundtrip must be a boolean");
                        return false;
                    }
                    lk.exportRoundtrip = ertVal.toBool();
                }

                // exportOnMissing (default "error")
                bool exportOnMissingExplicit = false;
                QJsonValue eomVal = lo.value(QStringLiteral("exportOnMissing"));
                if (!eomVal.isUndefined() && !eomVal.isNull()) {
                    QString eom = eomVal.toString();
                    if (!ExportOnMissing::isValid(eom)) {
                        if (err)
                            *err = QStringLiteral("route '") + out->table +
                                   QStringLiteral("': lookup '") + lk.name +
                                   QStringLiteral("': exportOnMissing value '") + eom +
                                   QStringLiteral("' is not allowed; use one of ") +
                                   ExportOnMissing::allowedList();
                        return false;
                    }
                    lk.exportOnMissing = eom;
                    exportOnMissingExplicit = true;
                } else {
                    lk.exportOnMissing = QString::fromLatin1(ExportOnMissing::kError);
                }

                // 2.4: exportRoundtrip=false + explicit exportOnMissing → info diagnostic
                if (!lk.exportRoundtrip && exportOnMissingExplicit && warnings) {
                    warnings->append(
                        QStringLiteral("route '") + out->table + QStringLiteral("': lookup '") +
                        lk.name +
                        QStringLiteral("': exportOnMissing has no effect when exportRoundtrip is "
                                       "false"));
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
    if (!readRoute(o, &route, err, &out->loadWarnings))
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
        if (!readRoute(rv.toObject(), &route, err, &out->loadWarnings))
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
            if (!readRoute(rv.toObject(), &route, err, &out->loadWarnings))
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

    // add-export-column-order: parse optional columnOrder array.
    QJsonValue colOrderVal = expObj.value(QStringLiteral("columnOrder"));
    if (!colOrderVal.isUndefined() && !colOrderVal.isNull()) {
        if (!colOrderVal.isArray()) {
            if (err)
                *err = QStringLiteral("export.columnOrder must be an array of strings");
            return false;
        }
        QJsonArray colOrderArr = colOrderVal.toArray();
        for (int i = 0; i < colOrderArr.size(); ++i) {
            QString s = colOrderArr[i].toString();
            if (s.isEmpty()) {
                if (err)
                    *err = QStringLiteral("export.columnOrder[") + QString::number(i) +
                           QStringLiteral("] must be a non-empty string");
                return false;
            }
            out->exportSpec.columnOrder.append(s);
        }
    }

    // add-export-reverse-lookup: 2.5 — reject illegal exportSpec.reverseLookups / exportLookups
    if (expObj.contains(QStringLiteral("reverseLookups")) ||
        expObj.contains(QStringLiteral("exportLookups"))) {
        if (err)
            *err = QStringLiteral(
                "export.reverseLookups / export.exportLookups are not supported; "
                "use the route-level lookups[] array with exportRoundtrip / exportOnMissing");
        return false;
    }

    // add-time-format-profile: profile-level temporal slots (defaults inherited by columns).
    if (!readTemporalSlot(o.value(QStringLiteral("dateFormat")), TemporalSlotKind::Date,
                          QStringLiteral("profile"), &out->dateFormat, err))
        return false;
    if (!readTemporalSlot(o.value(QStringLiteral("datetimeFormat")), TemporalSlotKind::DateTime,
                          QStringLiteral("profile"), &out->datetimeFormat, err))
        return false;
    if (!readTemporalSlot(o.value(QStringLiteral("timeFormat")), TemporalSlotKind::Time,
                          QStringLiteral("profile"), &out->timeFormat, err))
        return false;

    // Info-level diagnostic: column declares dateFormat AND a date:fmt validator.
    // Per spec "Compatibility with `date:fmt` validator", dateFormat wins; validator becomes
    // pass-through.
    auto walkRoutes = [&out](const QVector<RouteSpec>& routes, const QString& classCtx) {
        for (const RouteSpec& r : routes) {
            for (const ColumnSpec& c : r.columns) {
                if (!c.dateFormat.declared)
                    continue;
                for (const QString& t : c.validatorTokens) {
                    if (t.startsWith(QStringLiteral("date:"))) {
                        QString ctx = classCtx.isEmpty()
                                          ? QStringLiteral("route '") + r.table + '\''
                                          : QStringLiteral("class '") + classCtx +
                                                QStringLiteral("' route '") + r.table + '\'';
                        out->loadWarnings.append(
                            ctx + QStringLiteral(" column '") + c.dbColumn +
                            QStringLiteral("': dateFormat overrides validator '") + t +
                            QStringLiteral("'; validator becomes pass-through"));
                        break;
                    }
                }
            }
        }
    };
    if (out->mode == ProfileMode::Mixed) {
        for (const auto& cls : out->classes)
            walkRoutes(cls.routes, cls.id);
    } else {
        walkRoutes(out->routes, QString());
    }

    // Post-load validation: multi-slot, type×format consistency, epochSec slot restriction.
    auto validateColumn = [&](const ColumnSpec& col, const QString& colCtx) -> bool {
        // 2.5: at most one temporal slot per column
        int declaredCount = (col.dateFormat.declared ? 1 : 0) +
                            (col.datetimeFormat.declared ? 1 : 0) +
                            (col.timeFormat.declared ? 1 : 0);
        if (declaredCount > 1) {
            if (err)
                *err = colCtx + QStringLiteral(
                                    ": column may declare at most one of dateFormat,"
                                    " datetimeFormat, timeFormat");
            return false;
        }

        // 2.6-2.8: effective spec consistency per slot kind
        static const TemporalSlotKind allKinds[] = {
            TemporalSlotKind::Date, TemporalSlotKind::DateTime, TemporalSlotKind::Time};
        for (TemporalSlotKind k : allKinds) {
            TemporalFormatSpec eff = effectiveTemporalFor(k, col, *out);
            if (!eff.declared)
                continue;

            auto checkSide = [&](const TemporalSideSpec& side, const QString& sideName,
                                 TemporalSlotKind slotKind) -> bool {
                if (!side.declared)
                    return true;

                // 2.7: epochSec only on datetimeFormat.db
                if (side.type == TemporalPhysType::EpochSec) {
                    if (slotKind != TemporalSlotKind::DateTime ||
                        sideName != QStringLiteral("db")) {
                        if (err)
                            *err = colCtx + QStringLiteral(": ") +
                                   QLatin1String(slotKindName(slotKind)) + QStringLiteral(".") +
                                   sideName +
                                   QStringLiteral(
                                       ".type=epochSec is only allowed on datetimeFormat.db");
                        return false;
                    }
                }

                // 2.6: type=string requires non-empty format
                if (side.type == TemporalPhysType::String && side.format.isEmpty()) {
                    if (err)
                        *err = colCtx + QStringLiteral(": ") +
                               QLatin1String(slotKindName(slotKind)) + QStringLiteral(".") +
                               sideName +
                               QStringLiteral(".type=string requires a non-empty format");
                    return false;
                }

                // 2.6: type=epochSec requires empty format
                if (side.type == TemporalPhysType::EpochSec && !side.format.isEmpty()) {
                    if (err)
                        *err = colCtx + QStringLiteral(": ") +
                               QLatin1String(slotKindName(slotKind)) + QStringLiteral(".") +
                               sideName +
                               QStringLiteral(".type=epochSec must have no format (got '") +
                               side.format + QStringLiteral("')");
                    return false;
                }

                return true;
            };

            if (!checkSide(eff.excel, QStringLiteral("excel"), k))
                return false;
            if (!checkSide(eff.db, QStringLiteral("db"), k))
                return false;
        }
        return true;
    };

    auto validateRouteColumns = [&](const QVector<RouteSpec>& routes,
                                    const QString& classCtx) -> bool {
        for (const RouteSpec& r : routes) {
            for (const ColumnSpec& c : r.columns) {
                QString colCtx =
                    (classCtx.isEmpty() ? QStringLiteral("route '") + r.table + '\''
                                        : QStringLiteral("class '") + classCtx +
                                              QStringLiteral("' route '") + r.table + '\'') +
                    QStringLiteral(" column '") + c.dbColumn + '\'';
                if (!validateColumn(c, colCtx))
                    return false;
            }
        }
        return true;
    };

    if (out->mode == ProfileMode::Mixed) {
        for (const auto& cls : out->classes) {
            if (!validateRouteColumns(cls.routes, cls.id))
                return false;
        }
    } else {
        if (!validateRouteColumns(out->routes, QString()))
            return false;
    }

    return true;
}

}  // namespace dbridge::detail
