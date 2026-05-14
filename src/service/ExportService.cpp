#include "ExportService.h"

#include "dbridge/Errors.h"

#include <QHash>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>

#include "ErrorCollector.h"
#include "excel/ExcelWriter.h"
#include "mapping/TopoSorter.h"
#include "sql/SqlBuilder.h"
#include <algorithm>

namespace dbridge::detail {

static bool execAndWrite(const QString& sql, const QString& sheet, QSqlDatabase& db,
                         ExcelWriter& writer, bool writeHeader, QStringList* outHeaders,
                         ErrorCollector* errors, int* rowCount) {
    QSqlQuery q(db);
    if (!q.exec(sql)) {
        errors->addTable(sheet, QString::fromLatin1(err::E_EXPORT_QUERY),
                         QStringLiteral("Query failed: ") + q.lastError().text() +
                             QStringLiteral(" SQL: ") + sql);
        return false;
    }

    QSqlRecord rec = q.record();
    QStringList headers;
    for (int i = 0; i < rec.count(); ++i) {
        headers.append(rec.fieldName(i));
    }

    if (writeHeader) {
        writer.writeHeader(headers);
        if (outHeaders)
            *outHeaders = headers;
    }

    while (q.next()) {
        QVector<QVariant> row;
        for (int i = 0; i < rec.count(); ++i) {
            row.append(q.value(i));
        }
        writer.writeRow(row);
        (*rowCount)++;
    }
    return true;
}

ExportResult ExportService::run(const ProfileSpec& profile, const SchemaCatalog& catalog,
                                const QString& xlsxPath, const ExportOptions& options,
                                QSqlDatabase& db) {
    Q_UNUSED(catalog)
    ExportResult result;
    ErrorCollector errors;

    QString sheetName = options.sheetName.isEmpty() ? profile.sheet : options.sheetName;

    ExcelWriter writer;
    QString writerErr;
    if (!writer.open(xlsxPath, sheetName, &writerErr)) {
        errors.addTable(sheetName, QString::fromLatin1(err::E_WRITE_XLSX), writerErr);
        result.errors = errors.list();
        return result;
    }

    SqlBuilder sqlBuilder;
    TopoSorter topoSorter;
    int rowCount = 0;

    if (profile.mode == ProfileMode::Mixed) {
        // --- Mixed export ---
        // Collect all rows from each class, then sort in memory
        struct MixedRow {
            QString classId;
            QHash<QString, QVariant> data;
        };
        QVector<MixedRow> allRows;
        QStringList allHeaders;
        QSet<QString> headerSet;

        for (const auto& cls : profile.classes) {
            QVector<RouteSpec> sorted;
            QString topoErr;
            if (!topoSorter.sort(cls.routes, &sorted, &topoErr)) {
                errors.addTable(sheetName, QString::fromLatin1(err::E_PROFILE_TOPOLOGY_CYCLE),
                                topoErr);
                result.errors = errors.list();
                return result;
            }

            QString sql = sqlBuilder.buildAutoJoinSelect(sorted, profile.exportSpec);
            if (sql.isEmpty())
                continue;

            QSqlQuery q(db);
            if (!q.exec(sql)) {
                errors.addTable(sheetName, QString::fromLatin1(err::E_EXPORT_QUERY),
                                QStringLiteral("Query failed: ") + q.lastError().text());
                result.errors = errors.list();
                return result;
            }

            QSqlRecord rec = q.record();
            for (int i = 0; i < rec.count(); ++i) {
                QString h = rec.fieldName(i);
                if (!headerSet.contains(h)) {
                    headerSet.insert(h);
                    allHeaders.append(h);
                }
            }

            while (q.next()) {
                MixedRow row;
                row.classId = cls.id;
                for (int i = 0; i < rec.count(); ++i) {
                    row.data[rec.fieldName(i)] = q.value(i);
                }
                allRows.append(row);
            }
        }

        // Add classColumn to headers if specified
        QString classCol = profile.exportSpec.classColumn;
        if (!classCol.isEmpty() && !headerSet.contains(classCol)) {
            allHeaders.prepend(classCol);
        }

        // Sort allRows by orderBy (in-memory sort)
        if (!profile.exportSpec.orderBy.isEmpty()) {
            QString sortKey = profile.exportSpec.orderBy.first();
            // Strip "table." prefix if present
            if (sortKey.contains('.'))
                sortKey = sortKey.section('.', -1);
            std::stable_sort(
                allRows.begin(), allRows.end(), [&sortKey](const MixedRow& a, const MixedRow& b) {
                    return a.data.value(sortKey).toString() < b.data.value(sortKey).toString();
                });
        }

        writer.writeHeader(allHeaders);
        for (const auto& mr : allRows) {
            QVector<QVariant> rowVals;
            for (const auto& h : allHeaders) {
                if (h == classCol) {
                    rowVals.append(mr.classId);
                } else {
                    rowVals.append(mr.data.value(h, QVariant()));
                }
            }
            writer.writeRow(rowVals);
            rowCount++;
        }
    } else {
        // --- SingleTable / MultiTable export ---
        QString sql;
        if (!profile.exportSpec.explicitSql.isEmpty()) {
            sql = profile.exportSpec.explicitSql;
        } else {
            QVector<RouteSpec> sorted;
            QString topoErr;
            if (!topoSorter.sort(profile.routes, &sorted, &topoErr)) {
                errors.addTable(sheetName, QString::fromLatin1(err::E_PROFILE_TOPOLOGY_CYCLE),
                                topoErr);
                result.errors = errors.list();
                return result;
            }
            sql = sqlBuilder.buildAutoJoinSelect(sorted, profile.exportSpec);
        }

        QStringList headers;
        if (!execAndWrite(sql, sheetName, db, writer, true, &headers, &errors, &rowCount)) {
            result.errors = errors.list();
            return result;
        }
    }

    if (!writer.save(&writerErr)) {
        errors.addTable(sheetName, QString::fromLatin1(err::E_WRITE_XLSX), writerErr);
        result.errors = errors.list();
        return result;
    }

    result.ok = true;
    result.writtenRows = rowCount;
    result.errors = errors.list();
    return result;
}

}  // namespace dbridge::detail
