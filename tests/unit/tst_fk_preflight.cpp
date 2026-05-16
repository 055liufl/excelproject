#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <QtTest>

#include "mapping/RowPayload.h"
#include "profile/ProfileSpec.h"
#include "service/ErrorCollector.h"
#include "validation/ForeignKeyPreflight.h"

using namespace dbridge::detail;

namespace {

RouteSpec makeOrdersRoute() {
    RouteSpec r;
    r.table = QStringLiteral("orders");
    r.conflict.columns << QStringLiteral("order_no");
    ColumnSpec c;
    c.dbColumn = QStringLiteral("order_no");
    c.source = QStringLiteral("OrderNo");
    r.columns << c;
    return r;
}

RouteSpec makeOrderItemsRoute() {
    RouteSpec r;
    r.table = QStringLiteral("order_items");
    r.parent = QStringLiteral("orders");
    r.conflict.columns << QStringLiteral("order_no") << QStringLiteral("line_no");
    FkInjectSpec fk;
    fk.fromTable = QStringLiteral("orders");
    fk.fromColumn = QStringLiteral("order_no");
    fk.toTable = QStringLiteral("order_items");
    fk.toColumn = QStringLiteral("order_no");
    r.fkInject = fk;
    ColumnSpec lineNo;
    lineNo.dbColumn = QStringLiteral("line_no");
    lineNo.source = QStringLiteral("LineNo");
    r.columns << lineNo;
    return r;
}

// Build a parent payload for table "orders" with the given conflict key value.
// routeKey mirrors what Mapper would produce: bare table for classId="",
// "<classId>:<table>" otherwise.
RoutePayload makeParentPayload(const QString& classId, const QString& orderNo) {
    RoutePayload p;
    p.table = QStringLiteral("orders");
    p.routeKey = classId.isEmpty() ? p.table : classId + QLatin1Char(':') + p.table;
    p.dbColumns << QStringLiteral("order_no");
    p.binds << QVariant(orderNo);
    p.conflictKey << QStringLiteral("order_no");
    p.conflictVals << QVariant(orderNo);
    return p;
}

// Build a child payload for table "order_items" with the FK already injected.
RoutePayload makeChildPayload(const QString& classId, const QString& orderNo, int lineNo) {
    RoutePayload p;
    p.table = QStringLiteral("order_items");
    p.routeKey = classId.isEmpty() ? p.table : classId + QLatin1Char(':') + p.table;
    p.dbColumns << QStringLiteral("line_no") << QStringLiteral("order_no");
    p.binds << QVariant(lineNo) << QVariant(orderNo);
    p.conflictKey << QStringLiteral("order_no") << QStringLiteral("line_no");
    p.conflictVals << QVariant(orderNo) << QVariant(lineNo);
    return p;
}

QString uniqueConn() {
    return QStringLiteral("fkpreflight_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QSqlDatabase openMemoryDb() {
    auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), uniqueConn());
    db.setDatabaseName(QStringLiteral(":memory:"));
    db.open();
    QSqlQuery q(db);
    q.exec(QStringLiteral("CREATE TABLE orders (order_no TEXT PRIMARY KEY)"));
    return db;
}

}  // namespace

class TstFkPreflight : public QObject {
    Q_OBJECT

   private slots:
    // Non-mixed multiTable: child FK references parent in same batch.
    void testNonMixedParentInBatch() {
        QSqlDatabase db = openMemoryDb();

        RowContext ctx;
        ctx.excelRow = 2;
        ctx.classId = QString();
        ctx.payloads << makeParentPayload(QString(), QStringLiteral("SO-001"))
                     << makeChildPayload(QString(), QStringLiteral("SO-001"), 1);

        QVector<RouteSpec> routes;
        routes << makeOrdersRoute() << makeOrderItemsRoute();

        ErrorCollector errors;
        ForeignKeyPreflight fk;
        QVERIFY(fk.check({ctx}, routes, db, QStringLiteral("Orders"), &errors));
        QVERIFY(errors.empty());
    }

    // Regression: mixed mode. payload.routeKey is "A:orders" / "A:order_items",
    // but ForeignKeyPreflight must still find the parent by bare table name.
    void testMixedParentInBatch() {
        QSqlDatabase db = openMemoryDb();

        RowContext ctx;
        ctx.excelRow = 2;
        ctx.classId = QStringLiteral("A");
        ctx.payloads << makeParentPayload(QStringLiteral("A"), QStringLiteral("SO-001"))
                     << makeChildPayload(QStringLiteral("A"), QStringLiteral("SO-001"), 1);

        QVector<RouteSpec> routes;
        routes << makeOrdersRoute() << makeOrderItemsRoute();

        ErrorCollector errors;
        ForeignKeyPreflight fk;
        QVERIFY(fk.check({ctx}, routes, db, QStringLiteral("Mixed"), &errors));
        QVERIFY2(errors.empty(), errors.list().isEmpty()
                                     ? "no errors"
                                     : errors.list().first().message.toUtf8().constData());
    }

    // Parent not in batch but already in DB.
    void testParentInDb() {
        QSqlDatabase db = openMemoryDb();
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral("INSERT INTO orders(order_no) VALUES('SO-999')")));

        RowContext ctx;
        ctx.excelRow = 3;
        ctx.classId = QString();
        ctx.payloads << makeChildPayload(QString(), QStringLiteral("SO-999"), 1);

        QVector<RouteSpec> routes;
        routes << makeOrdersRoute() << makeOrderItemsRoute();

        ErrorCollector errors;
        ForeignKeyPreflight fk;
        QVERIFY(fk.check({ctx}, routes, db, QStringLiteral("Orders"), &errors));
        QVERIFY(errors.empty());
    }

    // Parent neither in batch nor in DB -> E_VALIDATE_FK.
    void testParentMissing() {
        QSqlDatabase db = openMemoryDb();

        RowContext ctx;
        ctx.excelRow = 4;
        ctx.classId = QString();
        ctx.payloads << makeChildPayload(QString(), QStringLiteral("SO-MISSING"), 1);

        QVector<RouteSpec> routes;
        routes << makeOrdersRoute() << makeOrderItemsRoute();

        ErrorCollector errors;
        ForeignKeyPreflight fk;
        QVERIFY(!fk.check({ctx}, routes, db, QStringLiteral("Orders"), &errors));
        QVERIFY(!errors.empty());
        QCOMPARE(errors.list().first().code, QStringLiteral("E_VALIDATE_FK"));
        QCOMPARE(errors.list().first().row, 4);
        QCOMPARE(errors.list().first().column, QStringLiteral("order_no"));
        QCOMPARE(errors.list().first().rawValue, QStringLiteral("SO-MISSING"));
    }
};

QTEST_MAIN(TstFkPreflight)
#include "tst_fk_preflight.moc"
