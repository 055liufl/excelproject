#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "sync/SyncDDL.h"
#include "sync/capture/ChangelogStore.h"

using namespace dbridge::sync;

class TstSyncChangelogStore : public QObject {
    Q_OBJECT
    QString conn_;
    QSqlDatabase db_;

   private slots:
    void init() {
        conn_ =
            QStringLiteral("tst_cl_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
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
        ChangelogStore cs;
        QString err;
        QVERIFY(cs.init(db_, &err));
    }

    void append_basic() {
        ChangelogStore cs;
        QString err;
        cs.init(db_, &err);
        qint64 localSeq = 0;
        QVERIFY(cs.append(db_, "changeset", "nodeA", "nodeA",
                          /*originSeq=*/1, /*parentSeq=*/0,
                          /*epoch=*/100, /*schemaVer=*/1, /*schemaFp=*/"fp1", QByteArray("blob1"),
                          /*authoritative=*/true, &localSeq, &err));
        QVERIFY(localSeq > 0);
    }

    void append_uniqueConstraint() {
        ChangelogStore cs;
        QString err;
        cs.init(db_, &err);
        qint64 seq1 = 0, seq2 = 0;
        cs.append(db_, "changeset", "A", "A", 1, 0, 1, 1, "fp", QByteArray("b1"), true, &seq1,
                  &err);
        // same (origin, epoch, originSeq) must fail
        bool ok = cs.append(db_, "changeset", "A", "A", 1, 0, 1, 1, "fp", QByteArray("b2"), true,
                            &seq2, &err);
        QVERIFY(!ok);  // UNIQUE(origin, stream_epoch, origin_seq) violation
    }

    void appendForward_preservesOrigin() {
        ChangelogStore cs;
        QString err;
        cs.init(db_, &err);
        qint64 localSeq = 0;
        // appendForward = inbound changeset with remote origin
        QVERIFY(cs.appendForward(db_, "remoteNodeB", "centerNode",
                                 /*originSeq=*/7, /*epoch=*/1, /*schemaVer=*/1, /*schemaFp=*/"fp",
                                 QByteArray("remote_blob"), &localSeq, &err));
        QSqlQuery q(db_);
        q.prepare(
            QStringLiteral("SELECT origin, source_peer FROM __sync_changelog WHERE local_seq=?"));
        q.addBindValue(localSeq);
        QVERIFY(q.exec() && q.next());
        QCOMPARE(q.value(0).toString(), QString("remoteNodeB"));  // origin NOT recast
        QCOMPARE(q.value(1).toString(), QString("centerNode"));
    }

    void readRange_filtersCorrectly() {
        ChangelogStore cs;
        QString err;
        cs.init(db_, &err);
        qint64 s = 0;
        cs.append(db_, "changeset", "A", "A", 1, 0, 1, 1, "fp", QByteArray("b1"), true, &s, &err);
        cs.append(db_, "changeset", "A", "A", 2, 1, 1, 1, "fp", QByteArray("b2"), true, &s, &err);
        cs.append(db_, "changeset", "A", "A", 3, 2, 1, 1, "fp", QByteArray("b3"), true, &s, &err);
        // afterOriginSeq=1 should return seq 2 and 3
        auto entries = cs.readRange(db_, "A", 1);
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries[0].originSeq, qint64(2));
        QCOMPARE(entries[1].originSeq, qint64(3));
    }

    void readRange_emptyWhenNoneMatch() {
        ChangelogStore cs;
        QString err;
        cs.init(db_, &err);
        auto entries = cs.readRange(db_, "unknown", 0);
        QVERIFY(entries.isEmpty());
    }

    void readRangeAll_excludesOrigin() {
        ChangelogStore cs;
        QString err;
        cs.init(db_, &err);
        qint64 s = 0;
        cs.append(db_, "changeset", "A", "A", 1, 0, 1, 1, "fp", QByteArray("ba"), true, &s, &err);
        cs.append(db_, "changeset", "B", "B", 1, 0, 1, 1, "fp", QByteArray("bb"), true, &s, &err);
        cs.append(db_, "changeset", "C", "C", 1, 0, 1, 1, "fp", QByteArray("bc"), true, &s, &err);
        // exclude origin="A" → only B and C
        auto entries = cs.readRangeAll(db_, "A", /*afterLocalSeq=*/-1);
        QCOMPARE(entries.size(), 2);
        for (const auto& e : entries)
            QVERIFY(e.origin != "A");
    }

    void maxLocalSeq_returnsHighestSeq() {
        ChangelogStore cs;
        QString err;
        cs.init(db_, &err);
        QCOMPARE(cs.maxLocalSeq(db_), qint64(-1));  // empty → -1
        qint64 s = 0;
        cs.append(db_, "changeset", "A", "A", 1, 0, 1, 1, "fp", QByteArray("b1"), true, &s, &err);
        cs.append(db_, "changeset", "A", "A", 2, 1, 1, 1, "fp", QByteArray("b2"), true, &s, &err);
        QCOMPARE(cs.maxLocalSeq(db_), qint64(2));
    }
};

QTEST_APPLESS_MAIN(TstSyncChangelogStore)
#include "tst_sync_changelog_store.moc"
