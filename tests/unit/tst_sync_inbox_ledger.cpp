#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "sync/SyncDDL.h"
#include "sync/transport/InboxLedger.h"

using namespace dbridge::sync;

class TstSyncInboxLedger : public QObject {
    Q_OBJECT
    QString conn_;
    QSqlDatabase db_;

   private slots:
    void init() {
        conn_ =
            QStringLiteral("tst_il_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
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
        InboxLedger ledger;
        QString err;
        QVERIFY(ledger.init(db_, &err));
    }

    void status_unknown_whenAbsent() {
        InboxLedger ledger;
        QString err;
        ledger.init(db_, &err);
        QCOMPARE(ledger.status(db_, "missing_artifact"), LedgerStatus::Unknown);
    }

    void markSeen_setsSeenStatus() {
        InboxLedger ledger;
        QString err;
        ledger.init(db_, &err);
        QVERIFY(ledger.markSeen(db_, "art1.payload", &err));
        QCOMPARE(ledger.status(db_, "art1.payload"), LedgerStatus::Seen);
    }

    void markSeen_idempotent() {
        InboxLedger ledger;
        QString err;
        ledger.init(db_, &err);
        ledger.markSeen(db_, "art1.payload", &err);
        QVERIFY(ledger.markSeen(db_, "art1.payload", &err));  // second call is no-op
        QCOMPARE(ledger.status(db_, "art1.payload"), LedgerStatus::Seen);
    }

    void markConsumed_transition() {
        InboxLedger ledger;
        QString err;
        ledger.init(db_, &err);
        ledger.markSeen(db_, "art2.payload", &err);
        QVERIFY(ledger.markConsumed(db_, "art2.payload", &err));
        QCOMPARE(ledger.status(db_, "art2.payload"), LedgerStatus::Consumed);
    }

    void markCorrupt_transition() {
        InboxLedger ledger;
        QString err;
        ledger.init(db_, &err);
        ledger.markSeen(db_, "art3.payload", &err);
        QVERIFY(ledger.markCorrupt(db_, "art3.payload", &err));
        QCOMPARE(ledger.status(db_, "art3.payload"), LedgerStatus::Corrupt);
    }

    void consumed_idempotent_skipped() {
        InboxLedger ledger;
        QString err;
        ledger.init(db_, &err);
        ledger.markSeen(db_, "art4.payload", &err);
        ledger.markConsumed(db_, "art4.payload", &err);
        // re-marking seen should not change Consumed status
        ledger.markSeen(db_, "art4.payload", &err);
        QCOMPARE(ledger.status(db_, "art4.payload"), LedgerStatus::Consumed);
    }

    void pendingSeen_onlyReturnsSeen() {
        InboxLedger ledger;
        QString err;
        ledger.init(db_, &err);
        ledger.markSeen(db_, "seen1.payload", &err);
        ledger.markSeen(db_, "seen2.payload", &err);
        ledger.markSeen(db_, "consumed.payload", &err);
        ledger.markConsumed(db_, "consumed.payload", &err);
        ledger.markSeen(db_, "corrupt.payload", &err);
        ledger.markCorrupt(db_, "corrupt.payload", &err);

        QStringList pending = ledger.pendingSeen(db_);
        QCOMPARE(pending.size(), 2);
        QVERIFY(pending.contains("seen1.payload"));
        QVERIFY(pending.contains("seen2.payload"));
        QVERIFY(!pending.contains("consumed.payload"));
        QVERIFY(!pending.contains("corrupt.payload"));
    }

    void pendingSeen_emptyWhenNone() {
        InboxLedger ledger;
        QString err;
        ledger.init(db_, &err);
        QVERIFY(ledger.pendingSeen(db_).isEmpty());
    }

    void stalePending_returnsOldSeen() {
        InboxLedger ledger;
        QString err;
        ledger.init(db_, &err);

        // Directly insert a row with a very old first_seen_ms
        QSqlQuery q(db_);
        q.prepare(
            QStringLiteral("INSERT INTO __sync_inbox_ledger(artifact_name,status,first_seen_ms)"
                           " VALUES(?,?,?)"));
        q.addBindValue("old.payload");
        q.addBindValue("seen");
        q.addBindValue(qint64(1000));  // epoch ms = 1000 (very old)
        QVERIFY(q.exec());

        // fresh artifact
        ledger.markSeen(db_, "fresh.payload", &err);

        // gapTimeout = 500ms; now - 1000 >> 500ms so "old" is stale
        // fresh was just inserted so first_seen_ms is ~now, not stale
        qint64 gapTimeout = 1000LL * 60 * 60;  // 1 hour — "fresh" won't be stale
        QStringList stale = ledger.stalePending(db_, gapTimeout);
        QVERIFY(stale.contains("old.payload"));
        QVERIFY(!stale.contains("fresh.payload"));
    }

    void stalePending_excludesConsumed() {
        InboxLedger ledger;
        QString err;
        ledger.init(db_, &err);
        // old consumed
        QSqlQuery q(db_);
        q.prepare(QStringLiteral(
            "INSERT INTO __sync_inbox_ledger(artifact_name,status,first_seen_ms,consumed_ms)"
            " VALUES(?,?,?,?)"));
        q.addBindValue("old_consumed.payload");
        q.addBindValue("consumed");
        q.addBindValue(qint64(1000));
        q.addBindValue(qint64(2000));
        q.exec();
        QStringList stale = ledger.stalePending(db_, 1000LL * 60 * 60);
        QVERIFY(!stale.contains("old_consumed.payload"));
    }
};

QTEST_APPLESS_MAIN(TstSyncInboxLedger)
#include "tst_sync_inbox_ledger.moc"
