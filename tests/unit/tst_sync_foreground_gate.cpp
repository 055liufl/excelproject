#include "dbridge/Errors.h"

#include <QtTest>

#include "sync/ForegroundGate.h"

using namespace dbridge::sync;

class TstSyncForegroundGate : public QObject {
    Q_OBJECT
   private slots:
    void initialState() {
        ForegroundGate g;
        QVERIFY(!g.isHeld());
    }

    void tryAcquire_firstSucceeds() {
        ForegroundGate g;
        QString err;
        QVERIFY(g.tryAcquire(&err));
        QVERIFY(g.isHeld());
        QVERIFY(err.isEmpty());
    }

    void tryAcquire_reentrant_E_BUSY() {
        ForegroundGate g;
        QString err1, err2;
        QVERIFY(g.tryAcquire(&err1));
        QVERIFY(!g.tryAcquire(&err2));                                // second fails
        QVERIFY(err2.contains(QLatin1String(dbridge::err::E_BUSY)));  // E_BUSY
        QVERIFY(g.isHeld());                                          // still held
    }

    void release_clearsHeld() {
        ForegroundGate g;
        QString err;
        g.tryAcquire(&err);
        g.release();
        QVERIFY(!g.isHeld());
    }

    void reacquireAfterRelease() {
        ForegroundGate g;
        QString err;
        g.tryAcquire(&err);
        g.release();
        QVERIFY(g.tryAcquire(&err));
        QVERIFY(g.isHeld());
    }

    void release_whenNotHeld_noop() {
        ForegroundGate g;
        g.release();  // must not crash
        QVERIFY(!g.isHeld());
    }
};

QTEST_APPLESS_MAIN(TstSyncForegroundGate)
#include "tst_sync_foreground_gate.moc"
