#include "SchemaGuard.h"

#include <QCryptographicHash>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

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
    if (payloadFp != localFp_) {
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

        // column info
        QSqlQuery colQ(db);
        colQ.exec(QStringLiteral("PRAGMA table_info(%1)").arg(tbl));
        while (colQ.next()) {
            // cid, name, type, notnull, dflt_value, pk
            QString colName = colQ.value(1).toString();
            QString colType = colQ.value(2).toString();
            int pk = colQ.value(5).toInt();
            material.append("COL:");
            material.append(colName.toUtf8());
            material.append(':');
            material.append(colType.toUtf8());
            material.append(":PK=");
            material.append(QByteArray::number(pk));
            material.append('\n');
        }
    }
    return QString::fromLatin1(
        QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex());
}

}  // namespace dbridge::sync
