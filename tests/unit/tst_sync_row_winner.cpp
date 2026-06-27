#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "sync/SyncDDL.h"
#include "sync/apply/RowWinnerStore.h"

using namespace dbridge::sync;

static RowWinner makeWinner(const QString& origin, int rank, qint64 seq,
                            const QByteArray& hash = QByteArray("h")) {
    RowWinner w;
    w.origin = origin;
    w.rank = rank;
    w.originSeq = seq;
    w.contentHash = hash;
    w.winningContent = QStringLiteral("");  // must be non-null (NOT NULL DEFAULT '')
    return w;
}

static QString winnerOrigin(QSqlDatabase& db, const QString& table, const QString& pkHash) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT winning_origin FROM __sync_row_winner WHERE table_name=? AND pk_hash=?"));
    q.addBindValue(table);
    q.addBindValue(pkHash);
    if (q.exec() && q.next())
        return q.value(0).toString();
    return {};
}

class TstSyncRowWinner : public QObject {
    Q_OBJECT
    QString conn_;
    QSqlDatabase db_;

   private slots:
    void init() {
        conn_ =
            QStringLiteral("tst_rw_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
        db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn_);
        db_.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db_.open());
        QSqlQuery q(db_);
        for (const QString& s : dbridge::sync::ddl::allCreateStatements())
            q.exec(s);
    }
    void cleanup() {
        db_ = QSqlDatabase();
        if (QSqlDatabase::contains(conn_)) {
            QSqlDatabase::database(conn_).close();
            QSqlDatabase::removeDatabase(conn_);
        }
    }

    void init_ok() {
        RowWinnerStore rws;
        QString err;
        QVERIFY(rws.init(db_, &err));
    }

    // put stores winner; verify via SELECT
    void put_storesWinner() {
        RowWinnerStore rws;
        QString err;
        rws.init(db_, &err);
        RowWinner w = makeWinner("nodeA", 10, 42, QByteArray("abc"));
        QVERIFY(rws.put(db_, "orders", "pk001", w, &err));
        QCOMPARE(winnerOrigin(db_, "orders", "pk001"), QString("nodeA"));
    }

    // higher rank overwrites lower rank (put always writes; caller decides ordering)
    void put_higherRankOverwrites() {
        RowWinnerStore rws;
        QString err;
        rws.init(db_, &err);
        rws.put(db_, "t", "pk", makeWinner("B", 3, 1), &err);
        rws.put(db_, "t", "pk", makeWinner("A", 10, 2), &err);  // higher rank
        QCOMPARE(winnerOrigin(db_, "t", "pk"), QString("A"));
    }

    // putOrRefill: when challenger loses (lower rank), incumbent stays
    void putOrRefill_lowerRankKeepsIncumbent() {
        RowWinnerStore rws;
        QString err;
        rws.init(db_, &err);
        rws.put(db_, "t", "pk", makeWinner("A", 10, 1), &err);
        // low-rank challenger
        rws.putOrRefill(db_, "t", "pk", makeWinner("B", 3, 99), &err);
        QCOMPARE(winnerOrigin(db_, "t", "pk"), QString("A"));  // A stays
    }

    // putOrRefill: when challenger wins (higher rank), it replaces incumbent
    void putOrRefill_higherRankReplaces() {
        RowWinnerStore rws;
        QString err;
        rws.init(db_, &err);
        rws.put(db_, "t", "pk", makeWinner("B", 3, 1), &err);
        rws.putOrRefill(db_, "t", "pk", makeWinner("A", 10, 2), &err);
        QCOMPARE(winnerOrigin(db_, "t", "pk"), QString("A"));
    }

    // resetAll clears all rows
    void resetAll_clearsTable() {
        RowWinnerStore rws;
        QString err;
        rws.init(db_, &err);
        rws.put(db_, "t1", "p1", makeWinner("A", 1, 1), &err);
        rws.put(db_, "t2", "p2", makeWinner("B", 2, 1), &err);
        QVERIFY(rws.resetAll(db_, &err));
        QSqlQuery q(db_);
        q.exec(QStringLiteral("SELECT COUNT(*) FROM __sync_row_winner"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 0);
    }

    // clear removes specific row, leaves others
    void clear_specificRow() {
        RowWinnerStore rws;
        QString err;
        rws.init(db_, &err);
        rws.put(db_, "t", "pk1", makeWinner("A", 1, 1), &err);
        rws.put(db_, "t", "pk2", makeWinner("B", 2, 1), &err);
        QVERIFY(rws.clear(db_, "t", "pk1", &err));
        QVERIFY(winnerOrigin(db_, "t", "pk1").isEmpty());
        QCOMPARE(winnerOrigin(db_, "t", "pk2"), QString("B"));
    }

    // pkHash is stable and non-empty for same input
    void pkHash_stableAndNonEmpty() {
        QVariantMap m;
        m["id"] = 42;
        m["name"] = "Alice";
        QString h1 = RowWinnerStore::pkHash(m);
        QString h2 = RowWinnerStore::pkHash(m);
        QCOMPARE(h1, h2);
        QVERIFY(!h1.isEmpty());
    }

    // pkHash differs for different values
    void pkHash_differsForDifferentValues() {
        QVariantMap m1, m2;
        m1["id"] = 1;
        m2["id"] = 2;
        QVERIFY(RowWinnerStore::pkHash(m1) != RowWinnerStore::pkHash(m2));
    }

    // sentinel rank (INT_MIN) treated as "no winner" – any real rank beats it
    void putOrRefill_sentinelIsLowest() {
        RowWinnerStore rws;
        QString err;
        rws.init(db_, &err);
        RowWinner sentinel = makeWinner("none", INT_MIN, 0);
        RowWinner real = makeWinner("A", 1, 1);
        rws.put(db_, "t", "pk", sentinel, &err);
        rws.putOrRefill(db_, "t", "pk", real, &err);
        QCOMPARE(winnerOrigin(db_, "t", "pk"), QString("A"));
    }

    // Multiple pk_hash values are isolated within same table
    void put_multipleRows_independent() {
        RowWinnerStore rws;
        QString err;
        rws.init(db_, &err);
        rws.put(db_, "t", "pk1", makeWinner("A", 5, 1), &err);
        rws.put(db_, "t", "pk2", makeWinner("B", 3, 1), &err);
        QCOMPARE(winnerOrigin(db_, "t", "pk1"), QString("A"));
        QCOMPARE(winnerOrigin(db_, "t", "pk2"), QString("B"));
    }
};

QTEST_APPLESS_MAIN(TstSyncRowWinner)
#include "tst_sync_row_winner.moc"
