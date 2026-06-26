#include "SchemaGuard.h"

#include <QCryptographicHash>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

#include "sql/SqlBuilder.h"

namespace dbridge::sync {

void SchemaGuard::setLocal(qint64 localVer, const QString& localFp) {
    localVer_ = localVer;
    localFp_ = localFp;
}

bool SchemaGuard::verifyPayload(qint64 payloadVer, const QString& payloadFp, QString* err) {
    if (payloadVer != localVer_) {
        if (err)
            *err = QStringLiteral("schema version mismatch: payload=%1 local=%2")
                       .arg(payloadVer)
                       .arg(localVer_);
        return false;
    }
    // M-02 fix: skip fingerprint comparison when verifySchemaFingerprint is disabled.
    if (verifyFingerprint_ && payloadFp != localFp_) {
        if (err)
            *err = QStringLiteral("schema fingerprint mismatch: payload=%1 local=%2")
                       .arg(payloadFp)
                       .arg(localFp_);
        return false;
    }
    return true;
}

QString SchemaGuard::computeFingerprint(QSqlDatabase& db, const QStringList& tables) {
    QStringList sorted = tables;
    sorted.sort();

    QByteArray material;
    for (const QString& tbl : qAsConst(sorted)) {
        material.append("TABLE:");
        material.append(tbl.toUtf8());
        material.append('\n');

        // Column info — M-2 fix: include notnull and default_value so schema changes that
        // add NOT NULL constraints or change defaults are detected as fingerprint differences.
        QSqlQuery colQ(db);
        colQ.exec(QStringLiteral("PRAGMA table_info(") + detail::SqlBuilder::quoteIdent(tbl) +
                  QLatin1Char(')'));
        while (colQ.next()) {
            // cid, name, type, notnull, dflt_value, pk
            QString colName = colQ.value(1).toString();
            QString colType = colQ.value(2).toString();
            int notnull = colQ.value(3).toInt();
            QString dflt = colQ.value(4).toString();
            int pk = colQ.value(5).toInt();
            material.append("COL:");
            material.append(colName.toUtf8());
            material.append(':');
            material.append(colType.toUtf8());
            material.append(":PK=");
            material.append(QByteArray::number(pk));
            material.append(":NN=");
            material.append(QByteArray::number(notnull));
            material.append(":DFT=");
            material.append(dflt.toUtf8());
            material.append('\n');
        }

        // Unique indexes — changes to uniqueness constraints affect fingerprint.
        QSqlQuery idxQ(db);
        idxQ.exec(QStringLiteral("PRAGMA index_list(") + detail::SqlBuilder::quoteIdent(tbl) +
                  QLatin1Char(')'));
        while (idxQ.next()) {
            if (idxQ.value(2).toInt() != 1)  // unique flag
                continue;
            QString idxName = idxQ.value(1).toString();
            material.append("UIDX:");
            material.append(idxName.toUtf8());
            material.append('\n');
        }

        // FK relationships — changes in FK targets affect fingerprint.
        QSqlQuery fkQ(db);
        fkQ.exec(QStringLiteral("PRAGMA foreign_key_list(") + detail::SqlBuilder::quoteIdent(tbl) +
                 QLatin1Char(')'));
        while (fkQ.next()) {
            material.append("FK:");
            material.append(fkQ.value(2).toString().toUtf8());  // refTable
            material.append(':');
            material.append(fkQ.value(3).toString().toUtf8());  // from col
            material.append("->:");
            material.append(fkQ.value(4).toString().toUtf8());  // to col
            material.append('\n');
        }
    }
    return QString::fromLatin1(
        QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex());
}

}  // namespace dbridge::sync
