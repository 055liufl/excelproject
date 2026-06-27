#include <QSet>
#include <QtTest>

#include "sync/diff/InboundTableGate.h"

using namespace dbridge::sync;

class TstSyncInboundTableGate : public QObject {
    Q_OBJECT
   private slots:
    void initialState_notOpen() {
        InboundTableGate g;
        QVERIFY(!g.isOpen());
    }

    void open_setsWatched_isOpenTrue() {
        InboundTableGate g;
        g.open({"t_a", "t_b"});
        QVERIFY(g.isOpen());
    }

    void shouldDefer_anyHit_true() {
        InboundTableGate g;
        g.open({"t_a", "t_b"});
        QSet<QString> payload = {"t_x", "t_a"};  // t_a is watched
        QVERIFY(g.shouldDefer(payload));
    }

    void shouldDefer_noHit_false() {
        InboundTableGate g;
        g.open({"t_a", "t_b"});
        QSet<QString> payload = {"t_x", "t_y"};
        QVERIFY(!g.shouldDefer(payload));
    }

    void shouldDefer_singleHit_true() {
        InboundTableGate g;
        g.open({"only_watched"});
        QVERIFY(g.shouldDefer({"only_watched"}));
    }

    void shouldDefer_emptyPayload_false() {
        InboundTableGate g;
        g.open({"t_a"});
        QVERIFY(!g.shouldDefer({}));
    }

    void releaseAll_closesGate() {
        InboundTableGate g;
        g.open({"t_a"});
        g.releaseAll();
        QVERIFY(!g.isOpen());
        QVERIFY(!g.shouldDefer({"t_a"}));
    }

    void reopen_replacesWatched() {
        InboundTableGate g;
        g.open({"t_a"});
        g.open({"t_b"});  // replaces
        QVERIFY(!g.shouldDefer({"t_a"}));
        QVERIFY(g.shouldDefer({"t_b"}));
    }
};

QTEST_APPLESS_MAIN(TstSyncInboundTableGate)
#include "tst_sync_inbound_table_gate.moc"
