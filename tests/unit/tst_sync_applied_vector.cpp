#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "sync/SyncDDL.h"
#include "sync/apply/AppliedVectorStore.h"

using namespace dbridge::sync;

class TstSyncAppliedVector : public QObject {
    Q_OBJECT
    QString conn_;
    QSqlDatabase db_;

    void setupDb() {
        conn_ =
            QStringLiteral("tst_av_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
        db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn_);
        db_.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db_.open());
        QSqlQuery q(db_);
        for (const QString& s : dbridge::sync::ddl::allCreateStatements())
            q.exec(s);
    }

   private slots:
    void init() {
        setupDb();
    }
    void cleanup() {
        db_ = QSqlDatabase();  // release handle before removeDatabase
        if (QSqlDatabase::contains(conn_)) {
            QSqlDatabase::database(conn_).close();
            QSqlDatabase::removeDatabase(conn_);
        }
    }

    // init() only checks table exists
    void init_ok() {
        AppliedVectorStore av;
        QString err;
        QVERIFY2(av.init(db_, &err), qPrintable(err));
    }

    // First ever seq==1 for unknown origin → Apply
    void check_freshOrigin_seq1_Apply() {
        AppliedVectorStore av;
        QString err;
        av.init(db_, &err);
        QCOMPARE(av.check(db_, "A", 1, 1, &err), SeqCheckResult::Apply);
    }

    // First ever seq>1 for unknown origin → Gap
    void check_freshOrigin_seqGt1_Gap() {
        AppliedVectorStore av;
        QString err;
        av.init(db_, &err);
        QCOMPARE(av.check(db_, "A", 1, 2, &err), SeqCheckResult::Gap);
        QCOMPARE(av.check(db_, "A", 1, 99, &err), SeqCheckResult::Gap);
    }

    // seq == applied+1 → Apply; advance; seq <= applied → NoOp
    void check_strictSequential() {
        AppliedVectorStore av;
        QString err;
        av.init(db_, &err);

        // apply seq=1
        QCOMPARE(av.check(db_, "A", 1, 1, &err), SeqCheckResult::Apply);
        QVERIFY(av.advance(db_, "A", 1, 1, &err));

        // seq=2 is next → Apply
        QCOMPARE(av.check(db_, "A", 1, 2, &err), SeqCheckResult::Apply);
        QVERIFY(av.advance(db_, "A", 1, 2, &err));

        // seq=1 again → NoOp (already applied)
        QCOMPARE(av.check(db_, "A", 1, 1, &err), SeqCheckResult::NoOp);
        QCOMPARE(av.check(db_, "A", 1, 2, &err), SeqCheckResult::NoOp);
    }

    // seq > applied+1 → Gap (缺口不推进水位)
    void check_gap() {
        AppliedVectorStore av;
        QString err;
        av.init(db_, &err);
        av.check(db_, "A", 1, 1, &err);
        av.advance(db_, "A", 1, 1, &err);
        // skip seq=2, deliver seq=3 → Gap
        QCOMPARE(av.check(db_, "A", 1, 3, &err), SeqCheckResult::Gap);
        // seq=2 is still Apply (water mark did NOT advance to 3)
        QCOMPARE(av.check(db_, "A", 1, 2, &err), SeqCheckResult::Apply);
    }

    // multiple origins are independent
    void check_multipleOrigins_independent() {
        AppliedVectorStore av;
        QString err;
        av.init(db_, &err);
        // origin A at seq=1
        QCOMPARE(av.check(db_, "A", 1, 1, &err), SeqCheckResult::Apply);
        av.advance(db_, "A", 1, 1, &err);
        // origin B still fresh, seq=1 → Apply
        QCOMPARE(av.check(db_, "B", 1, 1, &err), SeqCheckResult::Apply);
    }

    // current() returns -1 if no row, or applied_seq otherwise
    void current_noRow_returnsMinusOne() {
        AppliedVectorStore av;
        QString err;
        av.init(db_, &err);
        QCOMPARE(av.current(db_, "X", 1), qint64(-1));
    }

    void current_afterAdvance() {
        AppliedVectorStore av;
        QString err;
        av.init(db_, &err);
        av.check(db_, "A", 1, 1, &err);
        av.advance(db_, "A", 1, 1, &err);
        QCOMPARE(av.current(db_, "A", 1), qint64(1));
        av.check(db_, "A", 1, 2, &err);
        av.advance(db_, "A", 1, 2, &err);
        QCOMPARE(av.current(db_, "A", 1), qint64(2));
    }

    // reset() zeroes applied_seq and bumps baseline_generation
    void reset_zeroes() {
        AppliedVectorStore av;
        QString err;
        av.init(db_, &err);
        av.check(db_, "A", 1, 1, &err);
        av.advance(db_, "A", 1, 1, &err);
        QVERIFY(av.reset(db_, "A", 1, /*baselineGen=*/2, &err));
        QCOMPARE(av.current(db_, "A", 1), qint64(0));
        // after reset, seq=1 is Apply again
        QCOMPARE(av.check(db_, "A", 1, 1, &err), SeqCheckResult::Apply);
    }

    // resetTo() truncates to a specific seq
    void resetTo_specificSeq() {
        AppliedVectorStore av;
        QString err;
        av.init(db_, &err);
        for (int i = 1; i <= 5; ++i) {
            av.check(db_, "A", 1, i, &err);
            av.advance(db_, "A", 1, i, &err);
        }
        QCOMPARE(av.current(db_, "A", 1), qint64(5));
        QVERIFY(av.resetTo(db_, "A", 1, /*originSeq=*/3, /*baselineGen=*/1, &err));
        QCOMPARE(av.current(db_, "A", 1), qint64(3));
    }
};

QTEST_APPLESS_MAIN(TstSyncAppliedVector)
#include "tst_sync_applied_vector.moc"
