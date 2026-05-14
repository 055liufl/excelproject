#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "schema/SchemaCatalog.h"
#include "schema/SchemaIntrospector.h"

using namespace dbridge::detail;

class TstSchemaIntrospector : public QObject {
    Q_OBJECT

    QString connName_;

    void execSql(QSqlDatabase& db, const QString& sql) {
        QSqlQuery q(db);
        QVERIFY2(q.exec(sql), q.lastError().text().toUtf8());
    }

   private slots:
    void init() {
        connName_ = QStringLiteral("tst_schema_") + QUuid::createUuid().toString();
    }

    void cleanup() {
        if (QSqlDatabase::contains(connName_)) {
            QSqlDatabase::database(connName_).close();
            QSqlDatabase::removeDatabase(connName_);
        }
    }

    void testSimpleTable() {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName_);
        db.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db.open());
        execSql(db, R"(CREATE TABLE customer (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            phone TEXT
        ))");

        SchemaIntrospector si;
        SchemaCatalog catalog;
        QString err;
        QVERIFY2(si.load(db, &catalog, &err), err.toUtf8());

        QVERIFY(catalog.hasTable(QStringLiteral("customer")));
        const TableInfo* t = catalog.table(QStringLiteral("customer"));
        QVERIFY(t);
        QCOMPARE(t->columns.size(), 3);

        const ColumnInfo* idCol = t->column(QStringLiteral("id"));
        QVERIFY(idCol);
        QVERIFY(idCol->primaryKey);
        QVERIFY(idCol->autoIncrement);

        const ColumnInfo* nameCol = t->column(QStringLiteral("name"));
        QVERIFY(nameCol);
        QVERIFY(nameCol->notNull);
    }

    void testCompositePrimaryKey() {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName_);
        db.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db.open());
        execSql(db, R"(CREATE TABLE order_items (
            order_no TEXT NOT NULL,
            line_no INTEGER NOT NULL,
            sku TEXT,
            PRIMARY KEY (order_no, line_no)
        ))");

        SchemaIntrospector si;
        SchemaCatalog catalog;
        QString err;
        QVERIFY2(si.load(db, &catalog, &err), err.toUtf8());

        const TableInfo* t = catalog.table(QStringLiteral("order_items"));
        QVERIFY(t);

        // Both columns should be primary key
        const ColumnInfo* oCol = t->column(QStringLiteral("order_no"));
        QVERIFY(oCol && oCol->primaryKey);
        const ColumnInfo* lCol = t->column(QStringLiteral("line_no"));
        QVERIFY(lCol && lCol->primaryKey);
    }

    void testUniqueIndex() {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName_);
        db.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db.open());
        execSql(db, R"(CREATE TABLE products (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            sku TEXT NOT NULL UNIQUE
        ))");

        SchemaIntrospector si;
        SchemaCatalog catalog;
        QString err;
        QVERIFY2(si.load(db, &catalog, &err), err.toUtf8());

        const TableInfo* t = catalog.table(QStringLiteral("products"));
        QVERIFY(t);

        bool foundUniqueIdx = false;
        for (const auto& idx : t->indexes) {
            if (idx.unique && idx.columns.contains(QStringLiteral("sku"))) {
                foundUniqueIdx = true;
            }
        }
        QVERIFY(foundUniqueIdx);
    }

    void testForeignKey() {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName_);
        db.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db.open());
        execSql(db, R"(CREATE TABLE orders (order_no TEXT PRIMARY KEY))");
        execSql(db, R"(CREATE TABLE items (
            id INTEGER PRIMARY KEY,
            order_no TEXT REFERENCES orders(order_no)
        ))");

        SchemaIntrospector si;
        SchemaCatalog catalog;
        QString err;
        QVERIFY2(si.load(db, &catalog, &err), err.toUtf8());

        const TableInfo* t = catalog.table(QStringLiteral("items"));
        QVERIFY(t);
        QVERIFY(!t->foreignKeys.isEmpty());
        QCOMPARE(t->foreignKeys[0].refTable, QStringLiteral("orders"));
        QCOMPARE(t->foreignKeys[0].fromColumn, QStringLiteral("order_no"));
    }
};

QTEST_MAIN(TstSchemaIntrospector)
#include "tst_schema_introspector.moc"
