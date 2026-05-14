#include <QVariant>
#include <QtTest>

#include "mapping/RowPayload.h"
#include "profile/ProfileSpec.h"
#include "sql/SqlBuilder.h"

using namespace dbridge::detail;

class TstSqlBuilder : public QObject {
    Q_OBJECT

   private slots:
    void testBuildUpsertWithUpdate() {
        RoutePayload payload;
        payload.table = QStringLiteral("orders");
        payload.dbColumns << QStringLiteral("order_no") << QStringLiteral("customer")
                          << QStringLiteral("amount");
        payload.binds << QVariant(QStringLiteral("001")) << QVariant(QStringLiteral("Alice"))
                      << QVariant(100.0);
        payload.conflictKey << QStringLiteral("order_no");

        SqlBuilder builder;
        UpsertSql us = builder.buildUpsert(payload);

        QVERIFY(us.sql.contains(QStringLiteral("INSERT INTO orders")));
        QVERIFY(us.sql.contains(QStringLiteral("ON CONFLICT(order_no)")));
        QVERIFY(us.sql.contains(QStringLiteral("DO UPDATE SET")));
        QVERIFY(us.sql.contains(QStringLiteral("customer = excluded.customer")));
        QVERIFY(us.sql.contains(QStringLiteral("amount = excluded.amount")));
        QVERIFY(!us.sql.contains(QStringLiteral("order_no = excluded.order_no")));
    }

    void testBuildUpsertDoNothing() {
        RoutePayload payload;
        payload.table = QStringLiteral("keys");
        payload.dbColumns << QStringLiteral("key_col");
        payload.binds << QVariant(QStringLiteral("k1"));
        payload.conflictKey << QStringLiteral("key_col");

        SqlBuilder builder;
        UpsertSql us = builder.buildUpsert(payload);

        QVERIFY(us.sql.contains(QStringLiteral("DO NOTHING")));
        QVERIFY(!us.sql.contains(QStringLiteral("DO UPDATE")));
    }

    void testBuildUpsertMultiConflict() {
        RoutePayload payload;
        payload.table = QStringLiteral("order_items");
        payload.dbColumns << QStringLiteral("order_no") << QStringLiteral("line_no")
                          << QStringLiteral("sku");
        payload.binds << QVariant(QStringLiteral("001")) << QVariant(1LL)
                      << QVariant(QStringLiteral("SKU-001"));
        payload.conflictKey << QStringLiteral("order_no") << QStringLiteral("line_no");

        SqlBuilder builder;
        UpsertSql us = builder.buildUpsert(payload);

        QVERIFY(us.sql.contains(QStringLiteral("ON CONFLICT(order_no, line_no)")));
        QVERIFY(us.sql.contains(QStringLiteral("sku = excluded.sku")));
    }

    void testBuildAutoJoinSelectSingleTable() {
        RouteSpec route;
        route.table = QStringLiteral("customer");
        ColumnSpec c1;
        c1.dbColumn = QStringLiteral("customer_no");
        c1.source = QStringLiteral("CustomerNo");
        ColumnSpec c2;
        c2.dbColumn = QStringLiteral("name");
        c2.source = QStringLiteral("Name");
        route.columns << c1 << c2;

        ExportSpec exp;
        exp.orderBy << QStringLiteral("customer_no");

        SqlBuilder builder;
        QVector<RouteSpec> routeVec;
        routeVec << route;
        QString sql = builder.buildAutoJoinSelect(routeVec, exp);

        QVERIFY(sql.contains(QStringLiteral("FROM customer")));
        QVERIFY(sql.contains(QStringLiteral("customer.customer_no AS CustomerNo")));
        QVERIFY(sql.contains(QStringLiteral("ORDER BY customer_no")));
    }
};

QTEST_MAIN(TstSqlBuilder)
#include "tst_sql_builder.moc"
