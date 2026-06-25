#include "WriteTxn.h"

#include <QSqlError>
#include <QSqlQuery>

namespace dbridge::sync {

bool WriteTxn::begin(QString* err) {
    QSqlQuery q(db_);
    if (!q.exec(QStringLiteral("BEGIN IMMEDIATE"))) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    active_ = true;
    return true;
}

bool WriteTxn::commit(QString* err) {
    QSqlQuery q(db_);
    if (!q.exec(QStringLiteral("COMMIT"))) {
        if (err)
            *err = q.lastError().text();
        rollback();
        return false;
    }
    active_ = false;
    return true;
}

void WriteTxn::rollback() {
    if (!active_)
        return;
    QSqlQuery q(db_);
    q.exec(QStringLiteral("ROLLBACK"));
    active_ = false;
}

}  // namespace dbridge::sync
