#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "profile/AutoProfileBuilder.h"
#include "schema/SchemaCatalog.h"
#include "schema/SchemaIntrospector.h"

using namespace dbridge::detail;

class TstAutoProfileBuilder : public QObject {
    Q_OBJECT

    QString connName_;

    void execSql(QSqlDatabase& db, const QString& sql) {
        QSqlQuery q(db);
        QVERIFY2(q.exec(sql), q.lastError().text().toUtf8());
    }

    SchemaCatalog loadCatalog(QSqlDatabase& db) {
        SchemaIntrospector si;
        SchemaCatalog catalog;
        QString err;
        bool ok = si.load(db, &catalog, &err);
        Q_ASSERT_X(ok, "loadCatalog", err.toUtf8());
        return catalog;
    }

   private slots:
    void init() {
        connName_ = QStringLiteral("tst_auto_") + QUuid::createUuid().toString();
    }

    void cleanup() {
        if (QSqlDatabase::contains(connName_)) {
            QSqlDatabase::database(connName_).close();
            QSqlDatabase::removeDatabase(connName_);
        }
    }

    void testAutoincrementPrimaryKey() {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName_);
        db.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db.open());
        execSql(db, R"(CREATE TABLE items (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL UNIQUE
        ))");

        SchemaCatalog catalog = loadCatalog(db);
        AutoProfileBuilder builder;
        ProfileSpec spec;
        QString err;
        QVERIFY2(builder.build(*catalog.table("items"), &spec, &err), err.toUtf8());

        // autoincrement column should not be in conflict or writable cols
        QCOMPARE(spec.routes.size(), 1);
        const RouteSpec& route = spec.routes[0];
        QVERIFY(route.conflict.columns.contains(QStringLiteral("name")));
        for (const auto& col : route.columns) {
            QVERIFY(col.dbColumn != QStringLiteral("id"));  // skip autoincrement
        }
    }

    void testCompositePrimaryKey() {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName_);
        db.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db.open());
        execSql(db, R"(CREATE TABLE pairs (
            a TEXT NOT NULL,
            b TEXT NOT NULL,
            val TEXT,
            PRIMARY KEY (a, b)
        ))");

        SchemaCatalog catalog = loadCatalog(db);
        AutoProfileBuilder builder;
        ProfileSpec spec;
        QString err;
        QVERIFY2(builder.build(*catalog.table("pairs"), &spec, &err), err.toUtf8());

        const RouteSpec& route = spec.routes[0];
        QVERIFY(route.conflict.columns.contains(QStringLiteral("a")));
        QVERIFY(route.conflict.columns.contains(QStringLiteral("b")));
    }

    void testNoUniqueKey() {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName_);
        db.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db.open());
        execSql(db, R"(CREATE TABLE nopk (
            col1 TEXT,
            col2 INTEGER
        ))");

        SchemaCatalog catalog = loadCatalog(db);
        AutoProfileBuilder builder;
        ProfileSpec spec;
        QString err;
        // M-03 fix: build() now returns true for draft profiles (executable=false + issues).
        QVERIFY(builder.build(*catalog.table("nopk"), &spec, &err));
        QVERIFY(!spec.executable);
        QVERIFY(!spec.issues.isEmpty());
        QVERIFY(spec.issues.first().contains("E_PROFILE_NO_CONFLICT_KEY"));
    }

    void testJsonOutput() {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName_);
        db.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db.open());
        execSql(db, R"(CREATE TABLE customer (
            customer_no TEXT PRIMARY KEY,
            name TEXT
        ))");

        SchemaCatalog catalog = loadCatalog(db);
        AutoProfileBuilder builder;
        ProfileSpec spec;
        QString err;
        QVERIFY(builder.build(*catalog.table("customer"), &spec, &err));

        QString json = builder.toJson(spec);
        QVERIFY(!json.isEmpty());
        QVERIFY(json.contains(QStringLiteral("profileName")));
        QVERIFY(json.contains(QStringLiteral("auto_customer")));
        QVERIFY(json.contains(QStringLiteral("singleTable")));
    }
};

QTEST_MAIN(TstAutoProfileBuilder)
#include "tst_auto_profile_builder.moc"
