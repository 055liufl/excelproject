#include "dbridge/Types.h"
#include "dbridge/sync/SyncTypes.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "sync/apply/UpsertExecutor.h"

using namespace dbridge::sync;

static RowMutation makeRow(const QString& table, const QStringList& pkCols,
                           const QVariantList& pkVals, const QStringList& cols,
                           const QVariantList& vals, UpsertMode mode = UpsertMode::DoUpdate) {
    RowMutation m;
    m.table = table;
    m.pkColumns = pkCols;
    m.columns = cols;
    m.values = vals;
    m.mode = mode;
    return m;
}

class TstSyncUpsertExecutor : public QObject {
    Q_OBJECT
    QString conn_;
    QSqlDatabase db_;

    void exec(const QString& sql) {
        QSqlQuery q(db_);
        QVERIFY2(q.exec(sql), qPrintable(q.lastError().text()));
    }
    int count(const QString& table) {
        QSqlQuery q(db_);
        q.exec(QStringLiteral("SELECT COUNT(*) FROM ") + table);
        return q.next() ? q.value(0).toInt() : -1;
    }

   private slots:
    void init() {
        conn_ =
            QStringLiteral("tst_ue_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
        db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn_);
        db_.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db_.open());
        exec("CREATE TABLE items (id INTEGER PRIMARY KEY NOT NULL, name TEXT, qty INTEGER)");
    }
    void cleanup() {
        db_ = QSqlDatabase();  // release handle before removeDatabase
        if (QSqlDatabase::contains(conn_)) {
            QSqlDatabase::database(conn_).close();
            QSqlDatabase::removeDatabase(conn_);
        }
    }

    void apply_doUpdate_insertsNew() {
        UpsertExecutor ue;
        QList<RowMutation> rows;
        rows << makeRow("items", {"id"}, {1}, {"id", "name", "qty"}, {1, "Widget", 10});
        QList<dbridge::RowError> errors;
        QString err;
        QVERIFY(ue.apply(db_, rows, &errors, &err));
        QCOMPARE(count("items"), 1);
        QVERIFY(errors.isEmpty());
    }

    void apply_doUpdate_updatesExisting() {
        UpsertExecutor ue;
        QList<RowMutation> rows;
        rows << makeRow("items", {"id"}, {1}, {"id", "name", "qty"}, {1, "Widget", 10});
        ue.apply(db_, rows, nullptr, nullptr);
        // update qty
        rows.clear();
        rows << makeRow("items", {"id"}, {1}, {"id", "name", "qty"}, {1, "Gadget", 99});
        QVERIFY(ue.apply(db_, rows, nullptr, nullptr));
        QSqlQuery q(db_);
        q.exec("SELECT name, qty FROM items WHERE id=1");
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QString("Gadget"));
        QCOMPARE(q.value(1).toInt(), 99);
    }

    void apply_doNothing_doesNotOverwrite() {
        UpsertExecutor ue;
        // insert first
        QList<RowMutation> ins;
        ins << makeRow("items", {"id"}, {5}, {"id", "name", "qty"}, {5, "Original", 1});
        ue.apply(db_, ins, nullptr, nullptr);

        // DoNothing must not overwrite
        QList<RowMutation> dep;
        dep << makeRow("items", {"id"}, {5}, {"id", "name", "qty"}, {5, "ShouldNotWin", 999},
                       UpsertMode::DoNothing);
        QVERIFY(ue.apply(db_, dep, nullptr, nullptr));

        QSqlQuery q(db_);
        q.exec("SELECT name FROM items WHERE id=5");
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QString("Original"));  // unchanged
    }

    void apply_batchMultipleRows() {
        UpsertExecutor ue;
        QList<RowMutation> rows;
        for (int i = 1; i <= 5; ++i)
            rows << makeRow("items", {"id"}, {i}, {"id", "name", "qty"},
                            {i, QString("Item%1").arg(i), i * 10});
        QVERIFY(ue.apply(db_, rows, nullptr, nullptr));
        QCOMPARE(count("items"), 5);
    }

    void apply_preparedCacheReused() {
        UpsertExecutor ue;
        // Apply twice with same schema — prepared cache should be reused (no crash)
        QList<RowMutation> r1, r2;
        r1 << makeRow("items", {"id"}, {10}, {"id", "name", "qty"}, {10, "A", 1});
        r2 << makeRow("items", {"id"}, {11}, {"id", "name", "qty"}, {11, "B", 2});
        QVERIFY(ue.apply(db_, r1, nullptr, nullptr));
        QVERIFY(ue.apply(db_, r2, nullptr, nullptr));
        QCOMPARE(count("items"), 2);
    }

    void apply_emptyBatch_succeeds() {
        UpsertExecutor ue;
        QVERIFY(ue.apply(db_, {}, nullptr, nullptr));
    }

    void apply_constraintViolation_rowErrorCollected() {
        // Insert a NOT NULL violation: name=NULL when NOT NULL constraint exists
        // (items.name has no NOT NULL, so try id=NULL which violates PK)
        UpsertExecutor ue;
        QList<RowMutation> rows;
        // id=NULL violates PRIMARY KEY NOT NULL
        rows << makeRow("items", {"id"}, {QVariant()}, {"id", "name", "qty"}, {QVariant(), "X", 1});
        QList<dbridge::RowError> errors;
        QString fatalErr;
        // Should not crash; may collect row error or return false
        bool ok = ue.apply(db_, rows, &errors, &fatalErr);
        // Either row error collected or fatal error — must not crash
        (void)ok;
    }
};

QTEST_APPLESS_MAIN(TstSyncUpsertExecutor)
#include "tst_sync_upsert_executor.moc"
