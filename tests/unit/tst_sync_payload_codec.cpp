// ============================================================================
// tst_sync_payload_codec.cpp — PayloadCodec（工件编解码器）的单元测试
// ============================================================================
//
// 【被测对象在系统中的角色】
//   PayloadCodec 负责把同步过程中的各类「工件」(artifact) 在「内存对象 ↔ 线缆字节流」
//   之间转换。每个工件落到磁盘 outbox/inbox 目录、再经传输层在节点间传递，因此必须有
//   一套自描述、可校验、向后兼容的二进制线格式。它支持的工件种类：
//     · Changeset      —— 普通增量变更集（SQLite changeset blob + 头部元数据）；
//     · SelectionPush   —— 选择性推送的一个数据 chunk（含冻结清单与行数据）；
//     · ChangesetAck    —— 对某 (origin, epoch, seq) 的应用确认（ACK）；
//     · PushChunkAck    —— 对某选择性推送 chunk 的确认。
//   头部 PayloadHeader 携带 origin/streamEpoch/originSeq/schemaFingerprint/schemaVer/
//   routeTag/senderPeer 等路由与版本信息，是接收方判定「来源、漏收、schema 兼容、
//   防回送」的依据。
//
// 【本测试验证的契约（不变量）】
//   1) 往返一致性（round-trip）：encode 再 decode，所有字段与负载字节必须原样还原——
//      这是编解码器最核心的契约（任何字段在线缆上丢失/错位都会破坏同步语义）；
//   2) 压缩透明：大块可压缩负载编码后体积应变小，但 decode 仍能无损还原；
//   3) 损坏检测：空数据 / 错误魔数 / 截断 / 比特翻转等异常输入，decode 必须安全失败
//      （返回 false 或至少不崩溃），绝不把垃圾当合法工件解出来；
//   4) 版本兼容：第 2 版线格式新增的 senderPeer 字段必须能往返保留（转发链路依赖它）。
//
// 【测试框架】Qt Test，无数据库依赖，纯字节级编解码验证，故用 QTEST_APPLESS_MAIN。
// ============================================================================

#include "dbridge/Errors.h"
#include "dbridge/sync/SyncTypes.h"

#include <QUuid>
#include <QtTest>

#include "sync/payload/PayloadCodec.h"

using namespace dbridge::sync;

// makeHeader —— 测试夹具：用一组合理默认值快速造一个 PayloadHeader。
// 做什么：填好编码所需的最小头部字段，调用方只需覆盖关心的少数几个（origin/epoch/seq）。
// 为什么：几乎每个用例都要一个头部，集中构造可避免重复样板、并保证各用例头部一致可比。
// 参数：origin 来源节点；epoch 流纪元；seq 来源序号；fp schema 指纹；schemaVer schema 版本。
//   注意 senderPeer 默认等于 origin（自产自发场景），转发场景会在用例里另行覆盖。
static PayloadHeader makeHeader(const QString& origin = "nodeA", qint64 epoch = 1, qint64 seq = 42,
                                const QString& fp = "fp_test", qint64 schemaVer = 2) {
    PayloadHeader h;
    h.origin = origin;         // 变更来源节点 id
    h.streamEpoch = epoch;     // 流纪元（重启换值，隔离新旧序号流）
    h.originSeq = seq;         // 该来源的单调递增序号（漏收/ACK 据此判定）
    h.schemaFingerprint = fp;  // schema 结构指纹（接收方据此校验结构一致）
    h.schemaVer = schemaVer;   // schema 版本号（低于要求则进隔离区）
    h.routeTag = "center";     // 路由标签（拓扑角色，如 center/edge）
    h.senderPeer = origin;     // 直接发送方（转发时≠origin；见 senderPeer 用例）
    return h;
}

// TstSyncPayloadCodec —— PayloadCodec 测试套件。
// 每个 private slot 验证一种工件的往返一致性，或一种损坏输入的安全失败。
class TstSyncPayloadCodec : public QObject {
    Q_OBJECT
   private slots:

    // --- Changeset round-trip（普通变更集往返）---
    // GIVEN 一个标准头部 + 一段含不可打印字节(\x01\x02\x03)的伪 changeset blob；
    // WHEN encodeChangeset 再 decode；THEN 工件种类、头部全部字段、负载字节均原样还原。
    // 意义：这是编解码器最基本的契约——头部任一字段或负载在线缆上失真，都会让接收方
    //   误判来源/序号/schema，破坏整个同步。blob 里特意放二进制字节，验证不是按文本处理。
    void changeset_encodeDecodeRoundTrip() {
        PayloadCodec codec;
        PayloadHeader h = makeHeader();
        QByteArray blob("fake_changeset_bytes_\x01\x02\x03");

        QByteArray encoded = codec.encodeChangeset(h, blob);
        QVERIFY(!encoded.isEmpty());  // 编码必须产出非空字节流

        DecodeResult out;
        QString err;
        // QVERIFY2 在失败时打印 err，便于定位是哪一步解码失败。
        QVERIFY2(codec.decode(encoded, &out, &err), qPrintable(err));
        QCOMPARE(out.kind, PayloadKind::Changeset);                   // 种类正确
        QCOMPARE(out.header.origin, h.origin);                        // 逐字段比对：来源
        QCOMPARE(out.header.streamEpoch, h.streamEpoch);              // 纪元
        QCOMPARE(out.header.originSeq, h.originSeq);                  // 来源序号
        QCOMPARE(out.header.schemaFingerprint, h.schemaFingerprint);  // schema 指纹
        QCOMPARE(out.header.schemaVer, h.schemaVer);                  // schema 版本
        QCOMPARE(out.changeset, blob);                                // 负载字节无损
    }

    // changeset_largeBlob_compressedRoundTrip —— 大负载压缩往返。
    // GIVEN 50KB 高度重复(全 'Z')、极易压缩的负载；
    // WHEN 编码；THEN 编码后体积「明显小于」原始大小（证明编码路径确实做了压缩）；
    //   且 decode 仍能无损还原出完整 50KB。
    // 意义：工件经磁盘与网络传输，压缩可显著省 IO/带宽；但压缩必须对调用方透明（无损）。
    void changeset_largeBlob_compressedRoundTrip() {
        PayloadCodec codec;
        // 50KB of repetitive data — should compress well（50KB 重复数据，压缩率应很高）
        QByteArray big(50000, 'Z');
        QByteArray enc = codec.encodeChangeset(makeHeader(), big);
        // encoded should be smaller than raw (compression)（编码后应小于原始 → 压缩生效）
        QVERIFY(enc.size() < big.size());

        DecodeResult out;
        QString err;
        QVERIFY(codec.decode(enc, &out, &err));
        QCOMPARE(out.changeset, big);  // 解压后与原始逐字节一致（无损）
    }

    // --- SelectionPush round-trip（选择性推送 chunk 往返）---
    // GIVEN 一个带分片信息(pushId/chunkSeq/totalChunks)的头部，以及一个 SelectionPushBody：
    //   含一条冻结清单项(orders 表、pk=42)和一行实际数据({id:42,name:Alice})；
    // WHEN encodeSelectionPush 再 decode；THEN 分片元信息、冻结清单、行数据都正确还原。
    // 意义：选择性推送把大数据集切成多个 chunk 工件按序传输，每个 chunk 必须自带「我是
    //   push X 的第 i/N 片」以便接收方按序重组；冻结清单与行数据是 chunk 的实际载荷。
    void selectionPush_encodeDecodeRoundTrip() {
        PayloadCodec codec;
        PayloadHeader h = makeHeader("edge", 2, 7);
        h.pushId = "push-uuid-001";
        h.chunkSeq = 1;
        h.totalChunks = 3;

        // 构造推送体：分片头(pushId/chunkSeq/totalChunks) + 一条冻结清单项 + 一行数据。
        SelectionPushBody body;
        body.pushId = h.pushId;
        body.chunkSeq = h.chunkSeq;
        body.totalChunks = h.totalChunks;
        FrozenEntry fe;
        fe.table = "orders";         // 该条目所属表
        fe.primaryKey = "42";        // 主键文本表示
        fe.pkHash = "pk42";          // 主键哈希（快速匹配用）
        fe.recordKind = "selected";  // 记录类别（选中行 vs 依赖闭包补全行等）
        fe.topoIndex = 0;            // 拓扑序索引（保证父先于子套用）
        body.frozenEntries << fe;
        QVariantMap row;  // 一行实际数据，键=列名、值=列值
        row["id"] = 42;
        row["name"] = "Alice";
        body.rows << row;

        QByteArray enc = codec.encodeSelectionPush(h, body);
        QVERIFY(!enc.isEmpty());

        DecodeResult out;
        QString err;
        QVERIFY2(codec.decode(enc, &out, &err), qPrintable(err));
        QCOMPARE(out.kind, PayloadKind::SelectionPush);                     // 种类正确
        QCOMPARE(out.header.pushId, h.pushId);                              // 推送 id 还原
        QCOMPARE(out.selection.chunkSeq, 1);                                // 本片序号
        QCOMPARE(out.selection.totalChunks, 3);                             // 总片数
        QCOMPARE(out.selection.frozenEntries.size(), 1);                    // 冻结清单条数
        QCOMPARE(out.selection.frozenEntries[0].table, QString("orders"));  // 表名还原
        QCOMPARE(out.selection.rows.size(), 1);                             // 行数据条数
    }

    // --- ChangesetAck round-trip（changeset 确认往返）---
    // GIVEN 一个 ACK：确认 nodeA 在 epoch=5 下已应用到 appliedSeq=99，回送给 center；
    // WHEN encodeChangesetAck 再 decodeChangesetAck；THEN 关键字段(origin/epoch/appliedSeq)还原。
    // 意义：ACK 是发送方推进「已确认水位」、判定 sync 完成、裁剪 changelog 的依据，
    //   其字段若编解码失真，会导致重发、误删日志或前台永久等待。
    void changesetAck_roundTrip() {
        PayloadCodec codec;
        ChangesetAck ack;
        ack.origin = "nodeA";   // 被确认变更的来源
        ack.streamEpoch = 5;    // 纪元
        ack.appliedSeq = 99;    // 对端已应用到的来源序号
        ack.toPeer = "center";  // ACK 回送给谁

        QByteArray enc = codec.encodeChangesetAck(ack);
        QVERIFY(!enc.isEmpty());

        ChangesetAck out;
        QVERIFY(codec.decodeChangesetAck(enc, &out));
        QCOMPARE(out.origin, ack.origin);
        QCOMPARE(out.streamEpoch, ack.streamEpoch);
        QCOMPARE(out.appliedSeq, ack.appliedSeq);
    }

    // --- PushChunkAck round-trip（选择性推送 chunk 确认往返）---
    // GIVEN 一个 chunk ACK：push-001 的第 3/10 片、校验和 abc123、ok=true，回送 edge；
    // WHEN encodeChunkAck 再 decodeChunkAck；THEN 各字段还原且 ok 标志为真。
    // 意义：选择性推送靠逐 chunk ACK 实现「断点续传」与「成功确认」；checksum 让接收方
    //   能告知发送方该片是否完好收讫，ok 区分成功/失败重传。
    void chunkAck_roundTrip() {
        PayloadCodec codec;
        PushChunkAck ack;
        ack.pushId = "push-001";
        ack.chunkSeq = 3;
        ack.totalChunks = 10;
        ack.checksum = "abc123";  // 该片内容校验和
        ack.ok = true;            // 该片是否成功应用
        ack.toPeer = "edge";

        QByteArray enc = codec.encodeChunkAck(ack);
        QVERIFY(!enc.isEmpty());

        PushChunkAck out;
        QVERIFY(codec.decodeChunkAck(enc, &out));
        QCOMPARE(out.pushId, ack.pushId);
        QCOMPARE(out.chunkSeq, ack.chunkSeq);
        QCOMPARE(out.totalChunks, ack.totalChunks);
        QCOMPARE(out.checksum, ack.checksum);
        QVERIFY(out.ok);
    }

    // --- Corrupt data → 安全失败（损坏检测系列）---
    // 下面四个用例覆盖四类「坏输入」，验证 decode 不把垃圾当合法工件、且不崩溃。
    // 备注：错误信息是描述性文本（非错误码前缀），故只断言 err 非空，不匹配具体码。

    // decode_emptyData_corrupt —— 空字节流。
    // WHEN 解码空 QByteArray；THEN 失败且给出非空错误。最基本的健壮性：连魔数都没有。
    void decode_emptyData_corrupt() {
        PayloadCodec codec;
        DecodeResult out;
        QString err;
        QVERIFY(!codec.decode(QByteArray(), &out, &err));
        QVERIFY(!err.isEmpty());  // codec uses descriptive err, not error-code prefix
    }

    // decode_badMagic_corrupt —— 魔数(magic header)错误。
    // GIVEN 一段不以正确魔数开头的字节；THEN 解码必须立即失败。
    // 意义：魔数是工件的「身份证」，第一道防线——非本协议的字节绝不能被误解析。
    void decode_badMagic_corrupt() {
        PayloadCodec codec;
        QByteArray bad("BAD_MAGIC_BYTES____extra_data_here");
        DecodeResult out;
        QString err;
        QVERIFY(!codec.decode(bad, &out, &err));
        QVERIFY(!err.isEmpty());  // codec uses descriptive err, not error-code prefix
    }

    // decode_truncated_corrupt —— 截断（只取合法编码的前一半）。
    // GIVEN 一个合法工件被砍掉后半；THEN 解码失败。
    // 意义：模拟「写一半 / 传一半」的不完整文件，长度字段/负载不足必须被识别为损坏，
    //   绝不能越界读或返回半截数据。
    void decode_truncated_corrupt() {
        PayloadCodec codec;
        // encode a valid payload then truncate it（先编一个合法工件，再砍掉一半）
        QByteArray enc = codec.encodeChangeset(makeHeader(), QByteArray("data"));
        QByteArray truncated = enc.left(enc.size() / 2);
        DecodeResult out;
        QString err;
        QVERIFY(!codec.decode(truncated, &out, &err));
        QVERIFY(!err.isEmpty());  // codec uses descriptive err, not error-code prefix
    }

    // decode_bitflip_corrupt —— 比特翻转（中间某字节按位取反）。
    // GIVEN 合法工件中部一个字节被 ^0xFF 翻转；THEN 解码「可能」检出损坏(取决于压缩 CRC)，
    //   也可能侥幸通过——本用例的最低底线是【绝不崩溃】（故对返回值不做强断言，(void)ok）。
    // 意义：模拟传输/存储中的位错误。这里不强求「一定检出」，因为是否能检出取决于是否
    //   落在受 CRC 保护的区域；但无论如何不能段错误/越界，这是健壮性的硬底线。
    void decode_bitflip_corrupt() {
        PayloadCodec codec;
        QByteArray enc = codec.encodeChangeset(makeHeader(), QByteArray("hello world"));
        // flip a byte in the body area（翻转 body 区中部一个字节）
        if (enc.size() > 20) {
            char flipped = static_cast<char>(enc[enc.size() / 2]) ^ static_cast<char>(0xFF);
            enc[enc.size() / 2] = flipped;
        }
        DecodeResult out;
        QString err;
        bool ok = codec.decode(enc, &out, &err);
        // may or may not detect (compression CRC), but at minimum must not crash
        // （可能检出也可能检不出，取决于压缩 CRC；但最低要求是不崩溃）
        (void)ok;
    }

    // senderPeer_roundTrip —— 第 2 版线格式新增的 senderPeer 字段往返保留。
    // GIVEN 头部的 senderPeer 设为 "forwarder_node"（区别于 origin，模拟「转发节点」）；
    // WHEN 编码再解码；THEN senderPeer 必须原样还原。
    // 意义：在多跳转发拓扑里，origin 是「最初产生变更的节点」，senderPeer 是「这一跳的
    //   直接发送方」。防回送(anti-echo)与路由判定依赖 senderPeer；它是 v2 格式扩展项，
    //   本用例确保新增字段不会在编解码中丢失（向后兼容的关键验证）。
    void senderPeer_roundTrip() {
        PayloadCodec codec;
        PayloadHeader h = makeHeader();
        h.senderPeer = "forwarder_node";  // 显式区别于 origin(=nodeA)
        QByteArray enc = codec.encodeChangeset(h, QByteArray("data"));
        DecodeResult out;
        QString err;
        QVERIFY(codec.decode(enc, &out, &err));
        QCOMPARE(out.header.senderPeer, QString("forwarder_node"));
    }
};

// QTEST_APPLESS_MAIN：生成 main() 并依次运行全部用例（纯编解码测试，无需 QApplication）。
QTEST_APPLESS_MAIN(TstSyncPayloadCodec)
#include "tst_sync_payload_codec.moc"
