#pragma once
#include "dbridge/sync/SyncSelection.h"

#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QVariantMap>

namespace dbridge::sync {

// Resolves SyncSelection entries to concrete rows on a read-only connection.
class SelectionResolver {
   public:
    struct ResolveResult {
        QString table;
        QString pk;       // primary key value as string
        QVariantMap row;  // full row data
    };

    // Resolve all records/whereClauses in sel to concrete rows.
    // Uses rconn (read-only). Appends to *out.
    bool resolvePk(QSqlDatabase& rconn, const SyncSelection& sel, QList<ResolveResult>* out,
                   QString* err);

   private:
    bool resolveRecord(QSqlDatabase& rconn, const QString& table, const QString& pk,
                       QList<ResolveResult>* out, QString* err);
    bool resolveWhere(QSqlDatabase& rconn, const QString& table, const QString& whereExpr,
                      QList<ResolveResult>* out, QString* err);
    static QString rowToPk(const QVariantMap& row, const QString& table, QSqlDatabase& rconn);
};

}  // namespace dbridge::sync
