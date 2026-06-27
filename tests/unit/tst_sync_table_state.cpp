#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "sync/SyncDDL.h"
#include "sync/schema/TableStateStore.h"

using namespace dbridge::sync;

// Helper: build a TableMutation for INSERT
static TableMutation makeInsert(const QString& table, const QString& pkHash,
                                const QByteArray& afterHash) {
    TableMutation m;
    m.table = table;
    m.pkHash = pkHash;
    m.afterHash = afterHash;
    m.isInsert = true;
    m.isDelete = false;
    return m;
}
static TableMutation makeDelete(const QString& table, const QString& pkHash,
                                const QByteArray& beforeHash) {
    TableMutation m;
    m.table = table;
    m.pkHash = pkHash;
    m.beforeHash = beforeHash;
    m.isInsert = false;
    m.isDelete = true;
    return m;
}
static TableMutation makeUpdate(const QString& table, const QString& pkHash,
                                const QByteArray& before, const QByteArray& after) {
    TableMutation m;
    m.table = table;
    m.pkHash = pkHash;
    m.beforeHash = before;
    m.afterHash = after;
    m.isInsert = false;
    m.isDelete = false;
    return m;
}

class TstSyncTableState : public QObject {
    Q_OBJECT
    QString conn_;
    QSqlDatabase db_;

    void readState(const QString& table, qint64 epoch, QString* fp, QString* checksum,
                   qint64* rowCount) {
        TableStateStore ts;
        QString err;
        ts.init(db_, &err);
        bool found = false;
        ts.readState(db_, table, epoch, fp, checksum, rowCount, &found, &err);
    }

   private slots:
    void init() {
        conn_ =
            QStringLiteral("tst_ts_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
        db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn_);
        db_.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db_.open());
        QSqlQuery q(db_);
        for (const QString& s : dbridge::sync::ddl::allCreateStatements())
            q.exec(s);
    }
    void cleanup() {
        db_ = QSqlDatabase();  // release handle before removeDatabase
        if (QSqlDatabase::contains(conn_)) {
            QSqlDatabase::database(conn_).close();
            QSqlDatabase::removeDatabase(conn_);
        }
    }

    void init_ok() {
        TableStateStore ts;
        QString err;
        QVERIFY(ts.init(db_, &err));
    }

    void rowHash_stable() {
        QVariantMap row;
        row["id"] = 1;
        row["name"] = "Alice";
        QByteArray h1 = TableStateStore::rowHash(row);
        QByteArray h2 = TableStateStore::rowHash(row);
        QCOMPARE(h1, h2);
        QVERIFY(!h1.isEmpty());
    }

    void rowHash_differentValues_different() {
        QVariantMap r1, r2;
        r1["id"] = 1;
        r2["id"] = 2;
        QVERIFY(TableStateStore::rowHash(r1) != TableStateStore::rowHash(r2));
    }

    // INSERT: row_count +1, checksum += H(new)
    void applyMutations_insert_incrementsRowCount() {
        TableStateStore ts;
        QString err;
        ts.init(db_, &err);
        QByteArray h = QByteArray("row_hash_1");
        QList<TableMutation> muts = {makeInsert("orders", "pk1", h)};
        QVERIFY(ts.applyMutations(db_, muts, 1, "fp1", 1, &err));

        QString fp, cs;
        qint64 rc = 0;
        bool found = false;
        QVERIFY(ts.readState(db_, "orders", 1, &fp, &cs, &rc, &found, &err));
        QVERIFY(found);
        QCOMPARE(rc, qint64(1));
    }

    // DELETE: row_count -1
    void applyMutations_delete_decrementsRowCount() {
        TableStateStore ts;
        QString err;
        ts.init(db_, &err);
        QByteArray h = QByteArray("row_hash_1");
        // insert first
        ts.applyMutations(db_, {makeInsert("t", "pk1", h)}, 1, "fp", 1, &err);
        // then delete
        QVERIFY(ts.applyMutations(db_, {makeDelete("t", "pk1", h)}, 1, "fp", 2, &err));

        QString fp, cs;
        qint64 rc = 0;
        bool found = false;
        ts.readState(db_, "t", 1, &fp, &cs, &rc, &found, &err);
        QCOMPARE(rc, qint64(0));
    }

    // INSERT then DELETE with same hash => checksum returns to zero (modular add/sub)
    void applyMutations_insertDelete_checksumCancels() {
        TableStateStore ts;
        QString err;
        ts.init(db_, &err);
        QByteArray h = QByteArray("same_hash");
        ts.applyMutations(db_, {makeInsert("t", "pk1", h)}, 1, "fp", 1, &err);
        ts.applyMutations(db_, {makeDelete("t", "pk1", h)}, 1, "fp", 2, &err);

        QString fp, cs;
        qint64 rc = 0;
        bool found = false;
        ts.readState(db_, "t", 1, &fp, &cs, &rc, &found, &err);
        // After insert+delete of same row, checksum should be 0 (hex "0000000000000000")
        // and row_count=0
        QCOMPARE(rc, qint64(0));
        QVERIFY(cs == "0000000000000000" || cs == "0");
    }

    // G-06: two nodes with same content but different high_water => both read as "identical"
    //        because content_checksum + schema_fp + row_count determine identity, NOT high_water
    void g06_highWaterDoesNotAffectIdentity() {
        TableStateStore ts;
        QString err;
        ts.init(db_, &err);
        QByteArray h = QByteArray("row_hash_X");
        ts.applyMutations(db_, {makeInsert("t", "pk1", h)}, 1, "fp", /*originSeq=*/1, &err);

        QString fp1, cs1;
        qint64 rc1 = 0;
        bool f1 = false;
        ts.readState(db_, "t", 1, &fp1, &cs1, &rc1, &f1, &err);

        // simulate second node: same content but high_water advanced via a different origin_seq
        ts.applyMutations(db_, {makeInsert("t", "pk2", h)}, 1, "fp", /*originSeq=*/99, &err);
        ts.applyMutations(db_, {makeDelete("t", "pk2", h)}, 1, "fp", /*originSeq=*/100, &err);

        QString fp2, cs2;
        qint64 rc2 = 0;
        bool f2 = false;
        ts.readState(db_, "t", 1, &fp2, &cs2, &rc2, &f2, &err);

        // content_checksum and row_count are same (one row), fp same
        QCOMPARE(cs1, cs2);
        QCOMPARE(rc1, rc2);
        QCOMPARE(fp1, fp2);
        // (high_water_seq differs but not compared here — it's only informational per G-06)
    }

    // ORDER-INSENSITIVE: applying mutations in different order yields same checksum
    // Verified via rowHash: H(row1)+H(row2) == H(row2)+H(row1) (modular add is commutative)
    void applyMutations_orderInsensitive() {
        TableStateStore ts;
        QString err;
        ts.init(db_, &err);

        QByteArray h1("hash_row_1"), h2("hash_row_2");
        // Apply h1 then h2 on table "ta"
        ts.applyMutations(db_, {makeInsert("ta", "pk1", h1)}, 1, "fp", 1, &err);
        ts.applyMutations(db_, {makeInsert("ta", "pk2", h2)}, 1, "fp", 2, &err);

        // Apply h2 then h1 on table "tb"
        ts.applyMutations(db_, {makeInsert("tb", "pk2", h2)}, 1, "fp", 3, &err);
        ts.applyMutations(db_, {makeInsert("tb", "pk1", h1)}, 1, "fp", 4, &err);

        QString fp_a, cs_a, fp_b, cs_b;
        qint64 rc_a = 0, rc_b = 0;
        bool fa = false, fb = false;
        ts.readState(db_, "ta", 1, &fp_a, &cs_a, &rc_a, &fa, &err);
        ts.readState(db_, "tb", 1, &fp_b, &cs_b, &rc_b, &fb, &err);
        QVERIFY(fa && fb);
        QCOMPARE(rc_a, rc_b);  // same row count
        QCOMPARE(cs_a, cs_b);  // same checksum (modular add is commutative)
    }

    void readState_notFound_returnsFalse() {
        TableStateStore ts;
        QString err;
        ts.init(db_, &err);
        QString fp, cs;
        qint64 rc = 0;
        bool found = false;
        ts.readState(db_, "nonexistent", 1, &fp, &cs, &rc, &found, &err);
        QVERIFY(!found);
    }
};

QTEST_APPLESS_MAIN(TstSyncTableState)
#include "tst_sync_table_state.moc"
