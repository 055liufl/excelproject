// tst_udp_reassembly.cpp
// Part A: 纯组件单测 (A1–A26)
// Part B: socket 集成单测 (B1–B13)
// 覆盖率目标：udp_transport.cpp 行覆盖率 ≥ 90%

#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QUdpSocket>

#include "udp_transport.h"

// ─────────────────────────────────────────────────────────────────────────────
// 辅助：构造一个合法的 DATA 数据报（大端）
// ─────────────────────────────────────────────────────────────────────────────
static QByteArray makeDataDg(quint32 msgId, quint16 fragIdx, quint16 fragCount, quint32 totalSize,
                             const QString& fname, const QByteArray& payload) {
    const QByteArray fnUtf8 = fname.toUtf8();
    QByteArray dg;
    QDataStream ds(&dg, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::BigEndian);
    ds << static_cast<quint32>(0xDB5ACED0u) << static_cast<quint8>(0x01u) << msgId << fragIdx
       << fragCount << totalSize << static_cast<quint8>(fnUtf8.size());
    dg.append(fnUtf8);
    dg.append(payload);
    return dg;
}

// 构造合法 ACK 包（9B）
static QByteArray makeAckDg(quint32 msgId) {
    QByteArray ack;
    QDataStream ds(&ack, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::BigEndian);
    ds << static_cast<quint32>(0xDB5ACED0u) << static_cast<quint8>(0x02u) << msgId;
    return ack;
}

// ─────────────────────────────────────────────────────────────────────────────
// 辅助：向指定端口发 UDP 数据报
// ─────────────────────────────────────────────────────────────────────────────
static void sendUdp(const QByteArray& dg, quint16 port) {
    QUdpSocket s;
    s.writeDatagram(dg, QHostAddress::LocalHost, port);
}

// ─────────────────────────────────────────────────────────────────────────────
// 辅助：轮询直到文件存在（最多 timeoutMs 毫秒）
// ─────────────────────────────────────────────────────────────────────────────
static bool waitForFile(const QString& path, int timeoutMs = 6000) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        if (QFile::exists(path))
            return true;
        QThread::msleep(20);
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// 辅助：等待任意一个文件出现
// ─────────────────────────────────────────────────────────────────────────────
static bool waitForAny(const QStringList& paths, int timeoutMs = 6000) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        for (const QString& p : paths)
            if (QFile::exists(p))
                return true;
        QThread::msleep(20);
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// 每个集成测试用不同端口（避免 TIME_WAIT 串扰）
// ─────────────────────────────────────────────────────────────────────────────
static quint16 s_nextPort = 19900;
static quint16 allocPort() {
    return s_nextPort++;
}

// ─────────────────────────────────────────────────────────────────────────────
// 测试类（全局命名空间，与 friend 声明一致）
// ─────────────────────────────────────────────────────────────────────────────
class TstUdpReassembly : public QObject {
    Q_OBJECT

   private slots:
    // ── Part A：纯组件单测 ──────────────────────────────────────────────────

    // A1：分片大小与数量
    void frag_sizes() {
        // 空文件 → 1 片
        {
            auto r = fragmentMessage(1, QStringLiteral("f.payload"), QByteArray(), 800);
            QVERIFY(r.ok);
            QCOMPARE(r.datagrams.size(), 1);
            QVERIFY(r.datagrams[0].size() <= 800);
        }
        // 单片恰好
        {
            const int fnLen = 9;             // "f.payload"
            const int M = 800 - 18 - fnLen;  // 773
            QByteArray data(M, 'x');
            auto r = fragmentMessage(2, QStringLiteral("f.payload"), data, 800);
            QVERIFY(r.ok);
            QCOMPARE(r.datagrams.size(), 1);
        }
        // 单片+1 → 2 片
        {
            const int fnLen = 9;
            const int M = 800 - 18 - fnLen;
            QByteArray data(M + 1, 'y');
            auto r = fragmentMessage(3, QStringLiteral("f.payload"), data, 800);
            QVERIFY(r.ok);
            QCOMPARE(r.datagrams.size(), 2);
            for (const auto& dg : r.datagrams)
                QVERIFY(dg.size() <= 800);
        }
        // maxBytes=274，fnLen=1，M=255：多片
        {
            QByteArray data(510, 'z');
            auto r = fragmentMessage(4, QStringLiteral("a"), data, 274);
            QVERIFY(r.ok);
            QCOMPARE(r.datagrams.size(), 2);
        }
        // maxBytes=60000
        {
            QByteArray data(120000, 'k');
            auto r = fragmentMessage(5, QStringLiteral("f.payload"), data, 60000);
            QVERIFY(r.ok);
            QVERIFY(r.datagrams.size() >= 2);
        }
        // fname=255字节
        {
            QString longName(255, QLatin1Char('a'));
            auto r = fragmentMessage(6, longName, QByteArray(1, 'x'), 800);
            QVERIFY(r.ok);
        }
    }

    // A2：wire 头逐字节校验
    void frag_wire_header() {
        const QString fname = QStringLiteral("test.payload");
        const QByteArray data = QByteArray("hello");
        auto r = fragmentMessage(0x12345678u, fname, data, 800);
        QVERIFY(r.ok);
        QCOMPARE(r.datagrams.size(), 1);
        const QByteArray& dg = r.datagrams[0];

        QDataStream ds(dg);
        ds.setByteOrder(QDataStream::BigEndian);
        quint32 magic;
        quint8 type;
        quint32 msgId;
        quint16 fragIdx, fragCount;
        quint32 totalSize;
        quint8 fnLen;
        ds >> magic >> type >> msgId >> fragIdx >> fragCount >> totalSize >> fnLen;

        QCOMPARE(magic, static_cast<quint32>(0xDB5ACED0u));
        QCOMPARE(type, static_cast<quint8>(0x01u));
        QCOMPARE(msgId, static_cast<quint32>(0x12345678u));
        QCOMPARE(fragIdx, static_cast<quint16>(0));
        QCOMPARE(fragCount, static_cast<quint16>(1));
        QCOMPARE(totalSize, static_cast<quint32>(5));
        QCOMPARE(fnLen, static_cast<quint8>(fname.toUtf8().size()));

        const QByteArray fnInDg = dg.mid(18, fnLen);
        QCOMPARE(fnInDg, fname.toUtf8());
        const QByteArray payload = dg.mid(18 + fnLen);
        QCOMPARE(payload, data);
    }

    // A3：文件名 UTF-8 > 255
    void frag_err_fname_too_long() {
        QString fname(256, QLatin1Char('a'));
        auto r = fragmentMessage(1, fname, QByteArray("x"), 800);
        QVERIFY(!r.ok);
        QVERIFY(r.error.contains(QStringLiteral("filename"), Qt::CaseInsensitive));
    }

    // A4：M < 1
    void frag_err_no_payload_room() {
        // fname="a"(1B), maxBytes=18 → M=18-18-1=-1 < 1
        auto r = fragmentMessage(1, QStringLiteral("a"), QByteArray("x"), 18);
        QVERIFY(!r.ok);
        QVERIFY(r.error.contains(QStringLiteral("room"), Qt::CaseInsensitive) ||
                r.error.contains(QStringLiteral("small"), Qt::CaseInsensitive));
    }

    // A5：fragCount > 65535
    void frag_err_too_many_frags() {
        // fname="a"(1B), maxBytes=274 → M=274-18-1=255
        // 需要 data > 65535*255 = 16711425 字节
        // 用 274，M=255，data=65536*255+1
        QByteArray data(65535 * 255 + 256, 'x');
        auto r = fragmentMessage(1, QStringLiteral("a"), data, 274);
        QVERIFY(!r.ok);
        QVERIFY(r.error.contains(QStringLiteral("65535"), Qt::CaseInsensitive) ||
                r.error.contains(QStringLiteral("fragCount"), Qt::CaseInsensitive));
    }

    // A6：顺序喂全部片 → Completed，内容一致
    void roundtrip_inorder() {
        const QByteArray data = QByteArray("Hello, UDP reassembly!");
        const QString fname = QStringLiteral("msg.payload");
        auto fr = fragmentMessage(100u, fname, data, 800);
        QVERIFY(fr.ok);

        FragmentReassembler reasm(5000, 5000);
        RxEvent ev;
        for (const auto& dg : fr.datagrams)
            ev = reasm.feed(QStringLiteral("127.0.0.1:9999"), dg, 0);
        QCOMPARE(ev.kind, RxEvent::Completed);
        QCOMPARE(ev.bytes, data);
        QCOMPARE(ev.filename, fname);
    }

    // A7：乱序 + 重复片
    void roundtrip_reorder_dup() {
        const QString fname = QStringLiteral("big.payload");
        QByteArray data;
        for (int i = 0; i < 3000; ++i)
            data.append(static_cast<char>(i & 0xFF));
        auto fr = fragmentMessage(200u, fname, data, 274);
        QVERIFY(fr.ok);
        QVERIFY(fr.datagrams.size() > 1);

        FragmentReassembler reasm(5000, 5000);
        const QString key = QStringLiteral("127.0.0.1:9998");
        RxEvent ev;
        // 逆序喂
        for (int i = fr.datagrams.size() - 1; i >= 0; --i)
            ev = reasm.feed(key, fr.datagrams[i], 0);
        QCOMPARE(ev.kind, RxEvent::Completed);
        QCOMPARE(ev.bytes, data);

        // 重复喂第一片（已完成，不进入 pending，Completed 后尚未 markDelivered → 重建新消息不可，但
        // pending 已删，不再 complete） 重复片在 complete 前: 再喂不改变状态 这里已
        // Completed，pending 已清，再喂同 msgId → 短表未记录（markDelivered 未调）→ 重建 pending
        // 只需验证幂等性在 complete 前有效：下面 A7 补充乱序重复
    }

    // A8：长度 < 18 → None
    void reasm_reject_short() {
        FragmentReassembler reasm(5000, 5000);
        QByteArray dg(17, 'x');
        auto ev = reasm.feed(QStringLiteral("k"), dg, 0);
        QCOMPARE(ev.kind, RxEvent::None);
        QCOMPARE(reasm.pendingCount(), 0);
    }

    // A9：magic 错 → None
    void reasm_reject_bad_magic() {
        FragmentReassembler reasm(5000, 5000);
        QByteArray dg = makeDataDg(1, 0, 1, 5, QStringLiteral("f.payload"), QByteArray("hello"));
        // 破坏 magic
        dg[0] = 0x00;
        auto ev = reasm.feed(QStringLiteral("k"), dg, 0);
        QCOMPARE(ev.kind, RxEvent::None);
    }

    // A10：type 不是 0x01 → None
    void reasm_reject_bad_type() {
        FragmentReassembler reasm(5000, 5000);
        QByteArray dg = makeDataDg(1, 0, 1, 5, QStringLiteral("f.payload"), QByteArray("hello"));
        dg[4] = 0x02;  // ACK type
        auto ev = reasm.feed(QStringLiteral("k"), dg, 0);
        QCOMPARE(ev.kind, RxEvent::None);
    }

    // A11：fragIdx >= fragCount 或 fragCount == 0 → None
    void reasm_reject_out_of_range() {
        FragmentReassembler reasm(5000, 5000);
        const QString key = QStringLiteral("k");

        // fragCount == 0
        {
            QByteArray dg = makeDataDg(1, 0, 0, 5, QStringLiteral("f.payload"), QByteArray("hi"));
            auto ev = reasm.feed(key, dg, 0);
            QCOMPARE(ev.kind, RxEvent::None);
        }
        // fragIdx == fragCount（越界）
        {
            QByteArray dg = makeDataDg(2, 2, 2, 5, QStringLiteral("f.payload"), QByteArray("hi"));
            auto ev = reasm.feed(key, dg, 0);
            QCOMPARE(ev.kind, RxEvent::None);
        }
        QCOMPARE(reasm.pendingCount(), 0);
    }

    // A12：文件名长度越界（dg.size() < 18 + fnLen）→ None
    void reasm_reject_fnamelen_overflow() {
        FragmentReassembler reasm(5000, 5000);
        // 手工构造：fnLen=50，但实际只有 5 字节文件名
        QByteArray dg;
        QDataStream ds(&dg, QIODevice::WriteOnly);
        ds.setByteOrder(QDataStream::BigEndian);
        ds << static_cast<quint32>(0xDB5ACED0u) << static_cast<quint8>(0x01u)
           << static_cast<quint32>(1u) << static_cast<quint16>(0u) << static_cast<quint16>(1u)
           << static_cast<quint32>(3u) << static_cast<quint8>(50u);  // fnLen=50 but只有 "abc" 跟随
        dg.append("abc");                                            // 仅 3 字节，< 50
        auto ev = reasm.feed(QStringLiteral("k"), dg, 0);
        QCOMPARE(ev.kind, RxEvent::None);
    }

    // A13：不安全文件名 → None
    void reasm_reject_unsafe_filename() {
        FragmentReassembler reasm(5000, 5000);
        const QString key = QStringLiteral("k");

        // 空文件名
        {
            QByteArray dg = makeDataDg(1, 0, 1, 1, QStringLiteral(""), QByteArray("x"));
            auto ev = reasm.feed(key, dg, 0);
            QCOMPARE(ev.kind, RxEvent::None);
        }
        // 含 '/'
        {
            QByteArray dg = makeDataDg(2, 0, 1, 3, QStringLiteral("a/b"), QByteArray("abc"));
            auto ev = reasm.feed(key, dg, 0);
            QCOMPARE(ev.kind, RxEvent::None);
        }
        // 含 '\'
        {
            QByteArray dg = makeDataDg(3, 0, 1, 3, QStringLiteral("a\\b"), QByteArray("abc"));
            auto ev = reasm.feed(key, dg, 0);
            QCOMPARE(ev.kind, RxEvent::None);
        }
        // ".."
        {
            QByteArray dg = makeDataDg(4, 0, 1, 2, QStringLiteral(".."), QByteArray("xy"));
            auto ev = reasm.feed(key, dg, 0);
            QCOMPARE(ev.kind, RxEvent::None);
        }
        QCOMPARE(reasm.pendingCount(), 0);
    }

    // A14：元数据不一致 → 丢弃后续片
    void reasm_reject_meta_mismatch() {
        FragmentReassembler reasm(5000, 5000);
        const QString key = QStringLiteral("k");
        const QString fname = QStringLiteral("f.payload");
        const QByteArray payload0("hello");
        const QByteArray payload1("world");

        // 建立 pending：fragCount=2，totalSize=10
        {
            QByteArray dg = makeDataDg(1, 0, 2, 10, fname, payload0);
            reasm.feed(key, dg, 0);
        }
        QCOMPARE(reasm.pendingCount(), 1);

        // fragCount 不一致
        {
            QByteArray dg = makeDataDg(1, 1, 3, 10, fname, payload1);
            auto ev = reasm.feed(key, dg, 0);
            QCOMPARE(ev.kind, RxEvent::None);
        }
        // totalSize 不一致
        {
            QByteArray dg = makeDataDg(1, 1, 2, 99, fname, payload1);
            auto ev = reasm.feed(key, dg, 0);
            QCOMPARE(ev.kind, RxEvent::None);
        }
        // fname 不一致
        {
            QByteArray dg = makeDataDg(1, 1, 2, 10, QStringLiteral("other.payload"), payload1);
            auto ev = reasm.feed(key, dg, 0);
            QCOMPARE(ev.kind, RxEvent::None);
        }
        // 还未 complete
        QCOMPARE(reasm.pendingCount(), 1);
    }

    // A15：assembled.size() != totalSize → None（总长校验失败）
    void reasm_reject_size_mismatch() {
        FragmentReassembler reasm(5000, 5000);
        const QString key = QStringLiteral("k");
        const QString fname = QStringLiteral("f.payload");

        // 声明 totalSize=20，但实际组装 = 5+5=10
        QByteArray p0("hello"), p1("world");
        {
            QByteArray dg = makeDataDg(1, 0, 2, 20, fname, p0);  // totalSize 声称 20
            reasm.feed(key, dg, 0);
        }
        {
            QByteArray dg = makeDataDg(1, 1, 2, 20, fname, p1);
            auto ev = reasm.feed(key, dg, 0);
            // assembled=10 != totalSize=20 → None，pending 清除
            QCOMPARE(ev.kind, RxEvent::None);
        }
        QCOMPARE(reasm.pendingCount(), 0);
    }

    // A16：扣留某片 → 未 complete；补喂后 Completed
    void retransmit_recovery() {
        const QString fname = QStringLiteral("big.payload");
        QByteArray data(3000, 'R');
        auto fr = fragmentMessage(300u, fname, data, 400);
        QVERIFY(fr.ok);
        QVERIFY(fr.datagrams.size() > 2);

        FragmentReassembler reasm(5000, 5000);
        const QString key = QStringLiteral("k");

        // 扣留第 1 片（index 1）
        for (int i = 0; i < fr.datagrams.size(); ++i) {
            if (i == 1)
                continue;
            auto ev = reasm.feed(key, fr.datagrams[i], 0);
            QCOMPARE(ev.kind, RxEvent::None);
        }
        QCOMPARE(reasm.pendingCount(), 1);

        // 喂第 1 片 → Completed
        RxEvent ev = reasm.feed(key, fr.datagrams[1], 0);
        QCOMPARE(ev.kind, RxEvent::Completed);
        QCOMPARE(ev.bytes, data);
    }

    // A17：Completed 后不 markDelivered → 可重投；markDelivered 后重喂 → NeedAck
    void durable_delivery() {
        const QString fname = QStringLiteral("d.payload");
        const QByteArray data("durable test data");
        auto fr = fragmentMessage(400u, fname, data, 800);
        QVERIFY(fr.ok);

        FragmentReassembler reasm(5000, 5000);
        const QString key = QStringLiteral("k");
        const qint64 t0 = 0;

        // 第一次 → Completed
        RxEvent ev;
        for (const auto& dg : fr.datagrams)
            ev = reasm.feed(key, dg, t0);
        QCOMPARE(ev.kind, RxEvent::Completed);

        // 不 markDelivered；再喂 → 重建 pending → Completed（可重投）
        for (const auto& dg : fr.datagrams)
            ev = reasm.feed(key, dg, t0);
        QCOMPARE(ev.kind, RxEvent::Completed);

        // markDelivered 后再喂 → NeedAck
        reasm.markDelivered(key, 400u, t0);
        for (const auto& dg : fr.datagrams)
            ev = reasm.feed(key, dg, t0);
        QCOMPARE(ev.kind, RxEvent::NeedAck);
    }

    // A18：markDelivered 后重喂同消息 → NeedAck，不重复交付
    void reasm_dup_after_complete() {
        const QString fname = QStringLiteral("dup.payload");
        const QByteArray data("dup test");
        auto fr = fragmentMessage(500u, fname, data, 800);
        QVERIFY(fr.ok);

        FragmentReassembler reasm(5000, 5000);
        const QString key = QStringLiteral("k");

        // 第一次 Completed + markDelivered
        RxEvent ev;
        for (const auto& dg : fr.datagrams)
            ev = reasm.feed(key, dg, 0);
        QCOMPARE(ev.kind, RxEvent::Completed);
        reasm.markDelivered(key, 500u, 0);

        // 再喂 → NeedAck
        for (const auto& dg : fr.datagrams)
            ev = reasm.feed(key, dg, 100);
        QCOMPARE(ev.kind, RxEvent::NeedAck);
        QCOMPARE(ev.msgId, static_cast<quint32>(500u));
    }

    // A19：pending 超时淘汰；未超时者保留
    void eviction() {
        FragmentReassembler reasm(1000, 5000);  // reassemblyTimeout=1000ms
        const QString key = QStringLiteral("k");

        // 喂一个 2 片消息的首片，nowMs=0
        {
            QByteArray dg =
                makeDataDg(1, 0, 2, 10, QStringLiteral("a.payload"), QByteArray("hello"));
            reasm.feed(key, dg, 0);
        }
        // 喂另一个消息（nowMs=500，未超时）
        {
            QByteArray dg =
                makeDataDg(2, 0, 2, 10, QStringLiteral("b.payload"), QByteArray("world"));
            reasm.feed(key, dg, 500);
        }
        QCOMPARE(reasm.pendingCount(), 2);

        // nowMs=1500：消息1 超时（lastProgress=0，1500-0>1000），消息2
        // 未超时（lastProgress=500，1500-500=1000，不 > 1000）
        reasm.evictStale(1500);
        QCOMPARE(reasm.pendingCount(), 1);

        // nowMs=1600：消息2 超时
        reasm.evictStale(1600);
        QCOMPARE(reasm.pendingCount(), 0);
    }

    // A20：重复片不刷新 lastProgressAt → 超时后淘汰
    void dup_frag_no_progress() {
        FragmentReassembler reasm(1000, 5000);
        const QString key = QStringLiteral("k");
        QByteArray dg0 = makeDataDg(1, 0, 2, 10, QStringLiteral("f.payload"), QByteArray("hello"));

        reasm.feed(key, dg0, 0);    // 首次喂片，lastProgress=0
        reasm.feed(key, dg0, 500);  // 重复片，不刷新 lastProgress
        reasm.feed(key, dg0, 900);  // 再次重复

        // nowMs=1001：0+1001>1000 → 超时淘汰
        reasm.evictStale(1001);
        QCOMPARE(reasm.pendingCount(), 0);
    }

    // A21：短表过期 → 再喂按新消息重建
    void completed_retention_expiry() {
        FragmentReassembler reasm(5000, 500);  // completedRetention=500ms
        const QString key = QStringLiteral("k");
        const QString fname = QStringLiteral("exp.payload");
        const QByteArray data("expiry test");
        auto fr = fragmentMessage(600u, fname, data, 800);
        QVERIFY(fr.ok);

        // Completed + markDelivered at t=0
        RxEvent ev;
        for (const auto& dg : fr.datagrams)
            ev = reasm.feed(key, dg, 0);
        reasm.markDelivered(key, 600u, 0);

        // t=600：短表项过期
        reasm.evictStale(600);

        // 再喂 → 按新消息重建 → Completed（非 NeedAck）
        for (const auto& dg : fr.datagrams)
            ev = reasm.feed(key, dg, 600);
        QCOMPARE(ev.kind, RxEvent::Completed);
    }

    // A22：两个不同 senderKey、相同 msgId → 各自独立
    void collision_two_senders() {
        const QString fname = QStringLiteral("c.payload");
        const QByteArray dataA("SENDER_A_DATA");
        const QByteArray dataB("SENDER_B_DATA");
        auto frA = fragmentMessage(700u, fname, dataA, 800);
        auto frB = fragmentMessage(700u, fname, dataB, 800);
        QVERIFY(frA.ok && frB.ok);

        FragmentReassembler reasm(5000, 5000);
        const QString keyA = QStringLiteral("10.0.0.1:1111");
        const QString keyB = QStringLiteral("10.0.0.2:2222");

        RxEvent evA, evB;
        for (const auto& dg : frA.datagrams)
            evA = reasm.feed(keyA, dg, 0);
        for (const auto& dg : frB.datagrams)
            evB = reasm.feed(keyB, dg, 0);

        QCOMPARE(evA.kind, RxEvent::Completed);
        QCOMPARE(evB.kind, RxEvent::Completed);
        QCOMPARE(evA.bytes, dataA);
        QCOMPARE(evB.bytes, dataB);
    }

    // A23：pendingCount 动态变化
    void pending_count_tracking() {
        FragmentReassembler reasm(5000, 5000);
        const QString key = QStringLiteral("k");
        QCOMPARE(reasm.pendingCount(), 0);

        // 建立 3 个 pending（各不同 msgId，都 2 片）
        for (int i = 1; i <= 3; ++i) {
            QByteArray dg = makeDataDg(i, 0, 2, 10, QString::fromLatin1("f%1.payload").arg(i),
                                       QByteArray("12345"));
            reasm.feed(key, dg, 0);
        }
        QCOMPARE(reasm.pendingCount(), 3);

        // 补齐消息 2 → Completed → pending 减 1
        {
            QByteArray dg =
                makeDataDg(2, 1, 2, 10, QStringLiteral("f2.payload"), QByteArray("67890"));
            auto ev = reasm.feed(key, dg, 0);
            QCOMPARE(ev.kind, RxEvent::Completed);
        }
        QCOMPARE(reasm.pendingCount(), 2);

        // 淘汰剩余
        reasm.evictStale(10000);
        QCOMPARE(reasm.pendingCount(), 0);
    }

    // A24：setMaxTransmitBytes 全分支（白盒 friend）
    void set_max_transmit_bytes() {
        QTemporaryDir d;
        UdpFileTransport t(19000, QHostAddress::LocalHost, 19001, d.path(), d.path());

        // 默认 60000
        QCOMPARE(t.maxTransmitBytes_, 60000);

        // < 274 → false，值不变
        QVERIFY(!t.setMaxTransmitBytes(273));
        QCOMPARE(t.maxTransmitBytes_, 60000);

        // == 274 → true，夹取 min(274,65507)=274
        QVERIFY(t.setMaxTransmitBytes(274));
        QCOMPARE(t.maxTransmitBytes_, 274);

        // 中间值
        QVERIFY(t.setMaxTransmitBytes(1000));
        QCOMPARE(t.maxTransmitBytes_, 1000);

        // == 65507 → true
        QVERIFY(t.setMaxTransmitBytes(65507));
        QCOMPARE(t.maxTransmitBytes_, 65507);

        // > 65507 → true，被夹到 65507
        QVERIFY(t.setMaxTransmitBytes(65508));
        QCOMPARE(t.maxTransmitBytes_, 65507);
    }

    // A25：extractTargetPeer 全分支 + .sending 预处理（白盒 friend）
    void extract_peer_all() {
        using F = UdpFileTransport;

        // ack，size>=3 → parts[2]
        QCOMPARE(F::extractTargetPeer(QStringLiteral("ack__from__nodeX__ms__uuid.ack")),
                 QStringLiteral("nodeX"));
        // ack，size<3 → ""
        QCOMPARE(F::extractTargetPeer(QStringLiteral("ack__nodeX")), QString());
        // blreq，size>=3
        QCOMPARE(F::extractTargetPeer(QStringLiteral("blreq__from__nodeY__ms.payload")),
                 QStringLiteral("nodeY"));
        // baselineresponse
        QCOMPARE(F::extractTargetPeer(QStringLiteral("a__nodeZ__x__y__baselineresponse.payload")),
                 QStringLiteral("nodeZ"));
        // baselinerequest
        QCOMPARE(F::extractTargetPeer(QStringLiteral("a__nodeW__x__y__baselinerequest.payload")),
                 QStringLiteral("nodeW"));
        // changeset，seg 有 '-'
        QCOMPARE(
            F::extractTargetPeer(QStringLiteral("origin__ep__changeset__seq__peerA-uuid.payload")),
            QStringLiteral("peerA"));
        // changeset，seg 多个 '-'，取最后一个左侧
        QCOMPARE(
            F::extractTargetPeer(QStringLiteral("origin__ep__changeset__seq__peer-A-uuid.payload")),
            QStringLiteral("peer-A"));
        // changeset，seg 无 '-' → ""
        QCOMPARE(F::extractTargetPeer(QStringLiteral("origin__ep__changeset__seq__nodash.payload")),
                 QString());
        // selectionpush，去 "peer-" 后 lastIndexOf('-') 取左侧
        QCOMPARE(F::extractTargetPeer(
                     QStringLiteral("origin__ep__selectionpush__id__peer-nodeB-uuid.payload")),
                 QStringLiteral("nodeB"));
        // selectionpush，含多 '-'
        QCOMPARE(F::extractTargetPeer(
                     QStringLiteral("origin__ep__selectionpush__id__peer-node-B-uuid.payload")),
                 QStringLiteral("node-B"));
        // selectionpush，不以 "peer-" 开头 → ""
        QCOMPARE(F::extractTargetPeer(
                     QStringLiteral("origin__ep__selectionpush__id__nopeerprefix.payload")),
                 QString());
        // .sending 预处理 → .payload
        QCOMPARE(F::extractTargetPeer(QStringLiteral("ack__from__nodeC__ms__uuid.payload.sending")),
                 QStringLiteral("nodeC"));
        // .sending 预处理 → .ack
        QCOMPARE(F::extractTargetPeer(QStringLiteral("ack__from__nodeD__ms__uuid.ack.sending")),
                 QStringLiteral("nodeD"));
        // 无匹配 → ""
        QCOMPARE(F::extractTargetPeer(QStringLiteral("unknown__format.payload")), QString());
    }

    // A26：构造与 requestStop（白盒 friend）
    void ctor_and_stop() {
        QTemporaryDir d;
        // 单 peer 构造
        {
            UdpFileTransport t(19050, QHostAddress::LocalHost, 19051, d.path(), d.path());
            QCOMPARE(t.maxTransmitBytes_, 60000);
            QCOMPARE(t.peers_.size(), 1);
            QVERIFY(t.peers_.contains(QStringLiteral("__single__")));
        }
        // 多 peer 构造
        {
            QHash<QString, UdpPeerEndpoint> peers;
            peers.insert(QStringLiteral("nodeA"), {QHostAddress::LocalHost, 19052});
            peers.insert(QStringLiteral("nodeB"), {QHostAddress::LocalHost, 19053});
            UdpFileTransport t(19054, peers, d.path(), d.path());
            QCOMPARE(t.peers_.size(), 2);
        }
        // 空 peers 构造
        {
            QHash<QString, UdpPeerEndpoint> peers;
            UdpFileTransport t(19055, peers, d.path(), d.path());
            QCOMPARE(t.peers_.size(), 0);
        }
        // requestStop 设置 stop_ 标志
        {
            UdpFileTransport t(19056, QHostAddress::LocalHost, 19057, d.path(), d.path());
            QCOMPARE(t.stop_.loadAcquire(), 0);
            t.requestStop();
            QCOMPARE(t.stop_.loadAcquire(), 1);
            // 多次调用不崩溃
            t.requestStop();
            QCOMPARE(t.stop_.loadAcquire(), 1);
        }
    }

    // ── Part B：socket 集成单测 ─────────────────────────────────────────────

    // B1：happy path，单片端到端
    void e2e_happy_single() {
        QTemporaryDir outboxA, inboxA, outboxB, inboxB;
        QVERIFY(outboxA.isValid() && inboxB.isValid());

        quint16 portA = allocPort();
        quint16 portB = allocPort();

        // A → B 发送端；B → A 接收端（只需 B 绑定接收）
        UdpFileTransport sender(portA, QHostAddress::LocalHost, portB, outboxA.path(),
                                inboxA.path());
        UdpFileTransport receiver(portB, QHostAddress::LocalHost, portA, outboxB.path(),
                                  inboxB.path());
        sender.start();
        receiver.start();

        // 放工件
        const QString artifactName = QStringLiteral("hello.payload");
        const QByteArray content = "single fragment happy path";
        {
            QFile f(outboxA.path() + QLatin1Char('/') + artifactName);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(content);
            f.close();
        }
        {
            QFile r(outboxA.path() + QLatin1Char('/') + artifactName + QLatin1String(".ready"));
            QVERIFY(r.open(QIODevice::WriteOnly));
            r.close();
        }

        // 等待 .sent
        QVERIFY2(
            waitForFile(outboxA.path() + QLatin1Char('/') + artifactName + QLatin1String(".sent"),
                        5000),
            "sender: .sent not appeared");
        // inbox 出现工件
        QVERIFY2(waitForFile(inboxB.path() + QLatin1Char('/') + artifactName, 5000),
                 "receiver: artifact not appeared in inbox");
        // inbox .ready
        QVERIFY2(
            waitForFile(inboxB.path() + QLatin1Char('/') + artifactName + QLatin1String(".ready"),
                        5000),
            "receiver: .ready not appeared");
        // 内容一致
        QFile inF(inboxB.path() + QLatin1Char('/') + artifactName);
        QVERIFY(inF.open(QIODevice::ReadOnly));
        QCOMPARE(inF.readAll(), content);

        sender.requestStop();
        receiver.requestStop();
        sender.wait(3000);
        receiver.wait(3000);
    }

    // B2：多分片端到端，内容字节一致
    void e2e_happy_multifrag_800() {
        QTemporaryDir outboxA, inboxA, outboxB, inboxB;
        quint16 portA = allocPort();
        quint16 portB = allocPort();

        UdpFileTransport sender(portA, QHostAddress::LocalHost, portB, outboxA.path(),
                                inboxA.path());
        UdpFileTransport receiver(portB, QHostAddress::LocalHost, portA, outboxB.path(),
                                  inboxB.path());
        QVERIFY(sender.setMaxTransmitBytes(800));
        QVERIFY(receiver.setMaxTransmitBytes(800));
        sender.start();
        receiver.start();

        const QString name = QStringLiteral("big.payload");
        QByteArray content;
        content.reserve(2048);
        for (int i = 0; i < 2048; ++i)
            content.append(static_cast<char>(i & 0xFF));

        {
            QFile f(outboxA.path() + QLatin1Char('/') + name);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(content);
        }
        {
            QFile r(outboxA.path() + QLatin1Char('/') + name + QLatin1String(".ready"));
            QVERIFY(r.open(QIODevice::WriteOnly));
        }

        QVERIFY2(
            waitForFile(outboxA.path() + QLatin1Char('/') + name + QLatin1String(".sent"), 6000),
            "multifrag: .sent not appeared");
        QVERIFY2(waitForFile(inboxB.path() + QLatin1Char('/') + name, 6000),
                 "multifrag: inbox file not appeared");

        QFile inF(inboxB.path() + QLatin1Char('/') + name);
        QVERIFY(inF.open(QIODevice::ReadOnly));
        QCOMPARE(inF.readAll(), content);

        sender.requestStop();
        receiver.requestStop();
        sender.wait(3000);
        receiver.wait(3000);
    }

    // B3：多 peer 路由
    void e2e_multipeer_routing() {
        QTemporaryDir outboxC, inboxC, inboxA, inboxB;
        quint16 portCenter = allocPort();
        quint16 portA = allocPort();
        quint16 portB = allocPort();

        QHash<QString, UdpPeerEndpoint> peers;
        peers.insert(QStringLiteral("nodeA"), {QHostAddress::LocalHost, portA});
        peers.insert(QStringLiteral("nodeB"), {QHostAddress::LocalHost, portB});

        UdpFileTransport center(portCenter, peers, outboxC.path(), inboxC.path());
        UdpFileTransport recvA(portA, QHostAddress::LocalHost, portCenter,
                               inboxA.path() + QLatin1String("_out"), inboxA.path());
        UdpFileTransport recvB(portB, QHostAddress::LocalHost, portCenter,
                               inboxB.path() + QLatin1String("_out"), inboxB.path());

        // 确保 recvA/recvB 的 outbox 目录存在
        QDir().mkpath(inboxA.path() + QLatin1String("_out"));
        QDir().mkpath(inboxB.path() + QLatin1String("_out"));

        center.start();
        recvA.start();
        recvB.start();

        // 投给 nodeA：ack 类型文件
        const QString nameA = QStringLiteral("ack__from__nodeA__ms__001.ack");
        {
            QFile f(outboxC.path() + QLatin1Char('/') + nameA);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("data_for_A");
        }
        {
            QFile r(outboxC.path() + QLatin1Char('/') + nameA + QLatin1String(".ready"));
            QVERIFY(r.open(QIODevice::WriteOnly));
        }

        // 投给 nodeB：ack 类型文件
        const QString nameB = QStringLiteral("ack__from__nodeB__ms__002.ack");
        {
            QFile f(outboxC.path() + QLatin1Char('/') + nameB);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("data_for_B");
        }
        {
            QFile r(outboxC.path() + QLatin1Char('/') + nameB + QLatin1String(".ready"));
            QVERIFY(r.open(QIODevice::WriteOnly));
        }

        QVERIFY2(
            waitForFile(outboxC.path() + QLatin1Char('/') + nameA + QLatin1String(".sent"), 5000),
            "routing: nameA not .sent");
        QVERIFY2(
            waitForFile(outboxC.path() + QLatin1Char('/') + nameB + QLatin1String(".sent"), 5000),
            "routing: nameB not .sent");

        center.requestStop();
        recvA.requestStop();
        recvB.requestStop();
        center.wait(3000);
        recvA.wait(3000);
        recvB.wait(3000);
    }

    // B4：giveUp（黑洞端口），断言 .failed + 重传次数 == 6
    void e2e_giveup_no_ack() {
        QTemporaryDir outbox, inbox;
        quint16 portSend = allocPort();
        quint16 portBlackhole = allocPort();

        // 哑 socket：只接收，不回 ACK
        QUdpSocket dumb;
        QVERIFY(dumb.bind(QHostAddress::LocalHost, portBlackhole));

        UdpFileTransport sender(portSend, QHostAddress::LocalHost, portBlackhole, outbox.path(),
                                inbox.path());
        sender.start();

        const QString name = QStringLiteral("giveup.payload");
        const QByteArray content(100, 'G');
        {
            QFile f(outbox.path() + QLatin1Char('/') + name);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(content);
        }
        {
            QFile r(outbox.path() + QLatin1Char('/') + name + QLatin1String(".ready"));
            QVERIFY(r.open(QIODevice::WriteOnly));
        }

        // 等待 .failed（giveUp ≈ 5*500ms + 余量）
        QVERIFY2(
            waitForFile(outbox.path() + QLatin1Char('/') + name + QLatin1String(".failed"), 5000),
            "giveup: .failed not appeared");

        // 统计哑 socket 收到的 DATA 包数（首发1 + 重传5 = 6）
        int recvCount = 0;
        while (dumb.hasPendingDatagrams()) {
            QByteArray dg(static_cast<int>(dumb.pendingDatagramSize()), Qt::Uninitialized);
            dumb.readDatagram(dg.data(), dg.size());
            if (dg.size() >= 5) {
                const auto* raw = reinterpret_cast<const unsigned char*>(dg.constData());
                const quint32 magic = (quint32(raw[0]) << 24) | (quint32(raw[1]) << 16) |
                                      (quint32(raw[2]) << 8) | quint32(raw[3]);
                if (magic == 0xDB5ACED0u && raw[4] == 0x01u)
                    recvCount++;
            }
        }
        // 单片：首发1 + 重传5 = 6；给一点缓冲（可能有延迟到达的包）
        QVERIFY2(recvCount == 6,
                 qPrintable(QString::fromLatin1("expected 6 DATA, got %1").arg(recvCount)));

        sender.requestStop();
        sender.wait(3000);
        dumb.close();
    }

    // B5：durable 落盘失败 → 不 ACK → 发送端最终 .failed
    void e2e_durable_write_fail() {
        QTemporaryDir outboxSend, inboxSend;
        quint16 portSend = allocPort();
        quint16 portRecv = allocPort();

        // 让接收端的 inboxDir_ 是一个普通文件（非目录）→ open(WriteOnly) 失败
        QTemporaryDir tmpBase;
        const QString fakeInbox = tmpBase.path() + QLatin1String("/fake_inbox_file");
        {
            QFile f(fakeInbox);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("I am a file, not a directory");
            f.close();
        }

        UdpFileTransport sender(portSend, QHostAddress::LocalHost, portRecv, outboxSend.path(),
                                inboxSend.path());
        UdpFileTransport receiver(portRecv, QHostAddress::LocalHost, portSend,
                                  tmpBase.path() + QLatin1String("/recv_out"), fakeInbox);

        QDir().mkpath(tmpBase.path() + QLatin1String("/recv_out"));

        sender.start();
        receiver.start();

        const QString name = QStringLiteral("durable_fail.payload");
        const QByteArray content(50, 'D');
        {
            QFile f(outboxSend.path() + QLatin1Char('/') + name);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(content);
        }
        {
            QFile r(outboxSend.path() + QLatin1Char('/') + name + QLatin1String(".ready"));
            QVERIFY(r.open(QIODevice::WriteOnly));
        }

        // 接收端 inbox 侧无 .ready 产出
        // 发送端最终 .failed
        QVERIFY2(waitForFile(outboxSend.path() + QLatin1Char('/') + name + QLatin1String(".failed"),
                             5000),
                 "durable_fail: .failed not appeared at sender");
        // fakeInbox 是文件，不会有 .ready
        QVERIFY(!QFile::exists(fakeInbox + QLatin1Char('/') + name + QLatin1String(".ready")));

        sender.requestStop();
        receiver.requestStop();
        sender.wait(3000);
        receiver.wait(3000);
    }

    // B6：不可路由文件名 → 直接 .failed，不发送
    // 必须 2+ peers 才进入 extractTargetPeer 多 peer 路由分支
    void e2e_unroutable_failed() {
        QTemporaryDir outbox, inbox;
        quint16 portC = allocPort();
        quint16 portX = allocPort();
        quint16 portY = allocPort();

        QHash<QString, UdpPeerEndpoint> peers;
        peers.insert(QStringLiteral("nodeX"), {QHostAddress::LocalHost, portX});
        peers.insert(QStringLiteral("nodeY"), {QHostAddress::LocalHost, portY});

        UdpFileTransport center(portC, peers, outbox.path(), inbox.path());
        center.start();

        // 哑 socket 监听 portX
        QUdpSocket dumb;
        dumb.bind(QHostAddress::LocalHost, portX);

        // 不可路由的文件名（extractTargetPeer 返回 ""）
        const QString name = QStringLiteral("unknown__format.payload");
        {
            QFile f(outbox.path() + QLatin1Char('/') + name);
            f.open(QIODevice::WriteOnly);
            f.write("data");
        }
        {
            QFile r(outbox.path() + QLatin1Char('/') + name + QLatin1String(".ready"));
            r.open(QIODevice::WriteOnly);
        }

        const bool failed =
            waitForFile(outbox.path() + QLatin1Char('/') + name + QLatin1String(".failed"), 3000);

        // 先停线程再断言，避免 QVERIFY 失败后线程仍在运行
        center.requestStop();
        center.wait(3000);
        dumb.close();

        QVERIFY2(failed, "unroutable: .failed not appeared");
    }

    // B7：拆包失败 → .failed（单 peer，241 字节文件名，983026 字节数据）
    void e2e_fragment_fail_failed() {
        QTemporaryDir outbox, inbox;
        quint16 portSend = allocPort();
        quint16 portRecv = allocPort();

        // 哑 socket 监听接收端口
        QUdpSocket dumb;
        QVERIFY(dumb.bind(QHostAddress::LocalHost, portRecv));

        UdpFileTransport sender(portSend, QHostAddress::LocalHost, portRecv, outbox.path(),
                                inbox.path());
        QVERIFY(sender.setMaxTransmitBytes(274));  // M = 274-18-241 = 15
        sender.start();

        // 文件名 241 字节（.ready.sending = 241+14=255 = NAME_MAX，可创建）
        const QString name = QString(241, QLatin1Char('a'));
        // 数据 983026B > 65535*15 = 983025 → fragCount=65536 > 65535
        const QByteArray content(983026, 'F');

        // 创建文件（本测试中数据较大，写到磁盘）
        {
            QFile f(outbox.path() + QLatin1Char('/') + name);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(content);
        }
        {
            QFile r(outbox.path() + QLatin1Char('/') + name + QLatin1String(".ready"));
            QVERIFY(r.open(QIODevice::WriteOnly));
        }

        const bool fragFailed =
            waitForFile(outbox.path() + QLatin1Char('/') + name + QLatin1String(".failed"), 4000);
        QThread::msleep(200);
        const bool noDumData = !dumb.hasPendingDatagrams();

        sender.requestStop();
        sender.wait(3000);
        dumb.close();

        QVERIFY2(fragFailed, "fragment_fail: .failed not appeared");
        QVERIFY(noDumData);
    }

    // B8：bind 失败 → 线程速退
    void e2e_bind_fail_thread_exits() {
        QTemporaryDir d;
        quint16 port = allocPort();

        // 预占端口
        QUdpSocket occupier;
        QVERIFY(occupier.bind(QHostAddress::LocalHost, port));

        UdpFileTransport t(port, QHostAddress::LocalHost, 19999, d.path(), d.path());
        t.start();

        // 线程应快速退出（bind 失败 → return）
        QVERIFY2(t.wait(2000), "bind_fail: thread did not exit");

        occupier.close();
    }

    // B9：坏包丢弃不崩溃，后续正常工件仍能送达
    void e2e_garbage_datagrams() {
        QTemporaryDir outboxA, inboxA, outboxB, inboxB;
        quint16 portA = allocPort();
        quint16 portB = allocPort();

        UdpFileTransport sender(portA, QHostAddress::LocalHost, portB, outboxA.path(),
                                inboxA.path());
        UdpFileTransport receiver(portB, QHostAddress::LocalHost, portA, outboxB.path(),
                                  inboxB.path());
        sender.start();
        receiver.start();

        // 发 4 类坏包给接收端
        // 1. < 5B 短包
        sendUdp(QByteArray(3, 'x'), portB);
        // 2. 错 magic（5B，magic=0xDEADBEEF）
        {
            QByteArray dg(5, 0);
            dg[0] = 0xDE;
            dg[1] = 0xAD;
            dg[2] = 0xBE;
            dg[3] = 0xEF;
            dg[4] = 0x01;
            sendUdp(dg, portB);
        }
        // 3. 正确 magic + type=0x03（未知）
        {
            QByteArray dg(5, 0);
            dg[0] = 0xDB;
            dg[1] = 0x5A;
            dg[2] = 0xCE;
            dg[3] = 0xD0;
            dg[4] = 0x03;
            sendUdp(dg, portB);
        }
        // 4. type=0x02 (ACK) 但长度 != 9（坏长 ACK，发给 sender）
        {
            QByteArray dg(8, 0);
            dg[0] = 0xDB;
            dg[1] = 0x5A;
            dg[2] = 0xCE;
            dg[3] = 0xD0;
            dg[4] = 0x02;
            sendUdp(dg, portA);
        }

        // 随后放正常工件
        const QString name = QStringLiteral("garbage_after.payload");
        const QByteArray content = "normal after garbage";
        {
            QFile f(outboxA.path() + QLatin1Char('/') + name);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(content);
        }
        {
            QFile r(outboxA.path() + QLatin1Char('/') + name + QLatin1String(".ready"));
            QVERIFY(r.open(QIODevice::WriteOnly));
        }

        QVERIFY2(
            waitForFile(outboxA.path() + QLatin1Char('/') + name + QLatin1String(".sent"), 5000),
            "garbage: .sent not appeared");
        QVERIFY2(waitForFile(inboxB.path() + QLatin1Char('/') + name, 5000),
                 "garbage: inbox not appeared");

        sender.requestStop();
        receiver.requestStop();
        sender.wait(3000);
        receiver.wait(3000);
    }

    // B10：ACK msgId 不在 outbound → 忽略，在途工件不受影响
    void e2e_ack_msgid_not_in_outbound() {
        QTemporaryDir outboxA, inboxA, outboxB, inboxB;
        quint16 portA = allocPort();
        quint16 portB = allocPort();

        UdpFileTransport sender(portA, QHostAddress::LocalHost, portB, outboxA.path(),
                                inboxA.path());
        UdpFileTransport receiver(portB, QHostAddress::LocalHost, portA, outboxB.path(),
                                  inboxB.path());
        sender.start();
        receiver.start();

        // 向发送端发一个 msgId 不存在于 outbound 的合法 ACK
        sendUdp(makeAckDg(0xDEADBEEFu), portA);

        // 放正常工件，验证发送端存活、在途工件不受影响
        const QString name = QStringLiteral("normal_after_badack.payload");
        const QByteArray content = "still alive";
        {
            QFile f(outboxA.path() + QLatin1Char('/') + name);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(content);
        }
        {
            QFile r(outboxA.path() + QLatin1Char('/') + name + QLatin1String(".ready"));
            QVERIFY(r.open(QIODevice::WriteOnly));
        }

        QVERIFY2(
            waitForFile(outboxA.path() + QLatin1Char('/') + name + QLatin1String(".sent"), 5000),
            "bad_ack_msgid: .sent not appeared");

        sender.requestStop();
        receiver.requestStop();
        sender.wait(3000);
        receiver.wait(3000);
    }

    // B11：ACK 端点校验（可选）—— 从错误端点发合法 msgId ACK → 不变 .sent
    void e2e_ack_endpoint_mismatch() {
        QTemporaryDir outboxA, inboxA, outboxB, inboxB;
        quint16 portA = allocPort();
        quint16 portB = allocPort();
        quint16 portWrong = allocPort();

        // 哑 socket 抓发送端的第一个 DATA 包
        QUdpSocket interceptor;
        QVERIFY(interceptor.bind(QHostAddress::LocalHost, portB));

        UdpFileTransport sender(portA, QHostAddress::LocalHost, portB, outboxA.path(),
                                inboxA.path());
        sender.start();

        // 放工件
        const QString name = QStringLiteral("ep_mismatch.payload");
        const QByteArray content = "endpoint mismatch test";
        {
            QFile f(outboxA.path() + QLatin1Char('/') + name);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(content);
        }
        {
            QFile r(outboxA.path() + QLatin1Char('/') + name + QLatin1String(".ready"));
            QVERIFY(r.open(QIODevice::WriteOnly));
        }

        // 等待 DATA 包到达
        interceptor.waitForReadyRead(2000);
        QByteArray dgData;
        quint32 ackMsgId = 0;
        while (interceptor.hasPendingDatagrams()) {
            QByteArray dg(static_cast<int>(interceptor.pendingDatagramSize()), Qt::Uninitialized);
            QHostAddress src;
            quint16 srcPort = 0;
            interceptor.readDatagram(dg.data(), dg.size(), &src, &srcPort);
            if (dg.size() >= 18) {
                const auto* raw = reinterpret_cast<const unsigned char*>(dg.constData());
                const quint32 magic = (quint32(raw[0]) << 24) | (quint32(raw[1]) << 16) |
                                      (quint32(raw[2]) << 8) | quint32(raw[3]);
                if (magic == 0xDB5ACED0u && raw[4] == 0x01u) {
                    ackMsgId = (quint32(raw[5]) << 24) | (quint32(raw[6]) << 16) |
                               (quint32(raw[7]) << 8) | quint32(raw[8]);
                    dgData = dg;
                    break;
                }
            }
        }
        QVERIFY(ackMsgId != 0);

        // 从错误端点（portWrong）发合法 ACK
        {
            QUdpSocket wrongSock;
            QVERIFY(wrongSock.bind(QHostAddress::LocalHost, portWrong));
            wrongSock.writeDatagram(makeAckDg(ackMsgId), QHostAddress::LocalHost, portA);
        }

        // 工件不应变 .sent（端点不匹配）
        QThread::msleep(300);
        QVERIFY(!QFile::exists(outboxA.path() + QLatin1Char('/') + name + QLatin1String(".sent")));

        // 从正确端点（interceptor 本身绑在 portB）发 ACK → 变 .sent
        interceptor.writeDatagram(makeAckDg(ackMsgId), QHostAddress::LocalHost, portA);
        QVERIFY2(
            waitForFile(outboxA.path() + QLatin1Char('/') + name + QLatin1String(".sent"), 2000),
            "ep_mismatch: correct ACK did not result in .sent");

        sender.requestStop();
        sender.wait(3000);
        interceptor.close();
    }

    // B12：pollOutbox 边界（outbox 不存在 + 主文件缺失）
    void e2e_poll_edge_cases() {
        QTemporaryDir inbox;
        quint16 port = allocPort();

        // ① outbox 目录不存在
        {
            const QString nonexist = inbox.path() + QLatin1String("/nonexist_outbox");
            UdpFileTransport t(port, QHostAddress::LocalHost, quint16(port + 1), nonexist,
                               inbox.path());
            t.start();
            QThread::msleep(200);  // 让线程跑几圈 pollOutbox
            t.requestStop();
            QVERIFY(t.wait(2000));
            // 无任何 .failed/.sent 产出（outbox 不存在直接 return）
        }

        // ② outbox 存在，但 *.ready 对应主文件不存在 → continue（不发送，不 .failed）
        {
            quint16 p2 = allocPort();
            QTemporaryDir outbox2;
            const QString name = QStringLiteral("ghost.payload");
            // 只放 .ready，不放主文件
            {
                QFile r(outbox2.path() + QLatin1Char('/') + name + QLatin1String(".ready"));
                QVERIFY(r.open(QIODevice::WriteOnly));
            }

            UdpFileTransport t(p2, QHostAddress::LocalHost, quint16(p2 + 1), outbox2.path(),
                               inbox.path());
            t.start();
            QThread::msleep(300);
            t.requestStop();
            QVERIFY(t.wait(2000));

            // .ready 可能被认领（.ready.sending），但无 .failed
            QVERIFY(!QFile::exists(outbox2.path() + QLatin1Char('/') + name +
                                   QLatin1String(".failed")));
            QVERIFY(
                !QFile::exists(outbox2.path() + QLatin1Char('/') + name + QLatin1String(".sent")));
        }
    }

    // B13：认领/主文件 rename 失败（预建目标文件，单线程确定性触发）
    void e2e_rename_fail_deterministic() {
        QTemporaryDir inbox;
        quint16 p = allocPort();

        // ① 认领失败：预建 {name}.ready.sending
        {
            QTemporaryDir outbox;
            const QString name = QStringLiteral("art1.payload");

            // 放主文件
            {
                QFile f(outbox.path() + QLatin1Char('/') + name);
                QVERIFY(f.open(QIODevice::WriteOnly));
                f.write("data1");
            }
            // 放 .ready
            {
                QFile r(outbox.path() + QLatin1Char('/') + name + QLatin1String(".ready"));
                QVERIFY(r.open(QIODevice::WriteOnly));
            }
            // 预建 .ready.sending → QFile::rename(.ready→.ready.sending) 因目标存在而失败
            {
                QFile block(outbox.path() + QLatin1Char('/') + name +
                            QLatin1String(".ready.sending"));
                QVERIFY(block.open(QIODevice::WriteOnly));
                block.write("block");
            }

            UdpFileTransport t(p, QHostAddress::LocalHost, quint16(p + 100), outbox.path(),
                               inbox.path());
            t.start();
            QThread::msleep(300);
            t.requestStop();
            QVERIFY(t.wait(2000));

            // 认领失败 → continue → .ready 仍在（或 .ready.sending 是我们预建的）
            // 无 .failed，无 .sent
            QVERIFY(
                !QFile::exists(outbox.path() + QLatin1Char('/') + name + QLatin1String(".failed")));
            QVERIFY(
                !QFile::exists(outbox.path() + QLatin1Char('/') + name + QLatin1String(".sent")));
        }

        // ② 主文件 rename 失败：认领成功(.ready.sending 建好)，但预建 {name}.sending
        {
            quint16 p2 = allocPort();
            QTemporaryDir outbox2;
            const QString name = QStringLiteral("art2.payload");

            // 放主文件
            {
                QFile f(outbox2.path() + QLatin1Char('/') + name);
                QVERIFY(f.open(QIODevice::WriteOnly));
                f.write("data2");
            }
            // 放 .ready
            {
                QFile r(outbox2.path() + QLatin1Char('/') + name + QLatin1String(".ready"));
                QVERIFY(r.open(QIODevice::WriteOnly));
            }
            // 预建 .sending → 认领 .ready→.ready.sending 成功；但主文件 rename(→.sending)
            // 因目标存在而失败
            {
                QFile block(outbox2.path() + QLatin1Char('/') + name + QLatin1String(".sending"));
                QVERIFY(block.open(QIODevice::WriteOnly));
                block.write("block_sending");
            }

            UdpFileTransport t2(p2, QHostAddress::LocalHost, quint16(p2 + 100), outbox2.path(),
                                inbox.path());
            t2.start();

            // 等待 .ready.failed 出现（主文件 rename 失败 → .ready.sending 改为 .ready.failed）
            const QString readyFailed =
                outbox2.path() + QLatin1Char('/') + name + QLatin1String(".ready.failed");
            QVERIFY2(waitForFile(readyFailed, 2000), "rename_fail: .ready.failed not appeared");

            // 主文件未变为 .sending（预建的 .sending 仍是 block 文件）
            // 原主文件应仍存在（rename 失败）
            QVERIFY(QFile::exists(outbox2.path() + QLatin1Char('/') + name));
            // 无 .sent
            QVERIFY(
                !QFile::exists(outbox2.path() + QLatin1Char('/') + name + QLatin1String(".sent")));

            t2.requestStop();
            QVERIFY(t2.wait(2000));
        }
    }
};

QTEST_GUILESS_MAIN(TstUdpReassembly)
#include "tst_udp_reassembly.moc"
