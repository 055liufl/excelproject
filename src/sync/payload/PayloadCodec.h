#pragma once
#include "dbridge/sync/SyncTypes.h"

#include <QByteArray>
#include <QString>

// ============================================================================
// PayloadCodec.h — 同步「载荷编解码器」：内存变更 ↔ 可传输字节 的桥
// ============================================================================
//
// 【这个类是什么】
//   节点之间不直接传内存对象，而是传「artifact（工件）」二进制文件。本类就是这两个
//   世界的双向翻译器：
//     编码(encode)：把 PayloadHeader（信封头）+ 各类负载体（changeset / 选择性推送 /
//                   基线请求 / 基线应答）打包成一段自描述的字节流，写进 outbox。
//     解码(decode)：从 inbox 读到字节流后还原成 DecodeResult（含 header 与按 kind 区分
//                   的负载体），交给 apply 管线。
//   它是同步管线最底层的「序列化层」，上游不关心字节布局，只递结构体；下游不关心来源，
//   只拿结构体。所有跨节点的二进制约定（魔数/版本/字节序/字段序）都集中在这里。
//
// 【线格式（wire format）—— 与 .cpp 的 writeHeader/readHeader 严格对应】
//   每个 payload artifact 的整体布局（QDataStream，Qt_5_12 版本、默认大端）：
//     magic   : quint32  固定 0x44425359 = ASCII "DBSY"，首先校验，不匹配即判损坏
//     version : quint16  线格式版本（见 kVersion / kVersionMin）
//     kind    : quint8   负载种类（见 KindByte 枚举）
//     header  : 见 writeHeader 写出的字段序（origin/originSeq/.../senderPeer）
//     body    : 经 qCompress 压缩的负载体（changeset 是原始 blob；其余是 JSON 文本）
//   ACK 工件格式更简单（无 header、无 body 压缩），见 encode/decodeChangesetAck/ChunkAck。
//
// 【关于压缩与 JSON】
//   · changeset 体：SQLite 原始变更集 blob，直接 qCompress；解码端 qUncompress 还原。
//   · 选择性推送 / 基线请求 / 基线应答体：先编成 JSON（便于演进与排错），再 qCompress。
//     其中二进制字段（fingerprint、baselineData）在 JSON 内以 Base64 文本承载。
//
// 【在管线中的位置 / 协作者】
//   PayloadHeader、各 *Body/*Payload、DecodeResult、ChangesetAck/PushChunkAck 等结构均
//   定义于 include/dbridge/sync/SyncTypes.h（已注释）；基线数据由 BaselineManager 生产/消费。
//   解码失败对应错误码 E_SYNC_PAYLOAD_CORRUPT（见 Errors.h）；本类只返回 false+文本，
//   错误码归类由调用方完成。
//
// 命名空间 dbridge::sync；注释风格对齐 Errors.h / SyncTypes.h / RowPayload.h。
// ============================================================================

namespace dbridge::sync {

// PayloadCodec —— 所有同步工件的二进制编解码器。
// 线格式：magic(4) "DBSY" + version(quint16) + kind(quint8) + header + body。
// changeset 负载体在写出前会先 qCompress 压缩。本类无状态，可自由复用同一实例。
class PayloadCodec {
   public:
    // 编码一个 changeset 负载。changeset 是 SQLite 的原始变更集 blob（会被压缩后写入）。
    QByteArray encodeChangeset(const PayloadHeader& h, const QByteArray& changeset);

    // 编码一个「选择性推送」负载（把冻结清单 frozenEntries + 行数据 rows 编成 JSON）。
    QByteArray encodeSelectionPush(const PayloadHeader& h, const SelectionPushBody& body);

    // 编码「基线请求」负载（请求方告诉源端：我要哪些表、从哪个 seq 起补齐）。
    QByteArray encodeBaselineRequest(const PayloadHeader& h, const BaselineRequestPayload& body);
    // 编码「基线应答」负载（源端回带全量 baselineData + 各 origin 水位切点 originCuts）。
    QByteArray encodeBaselineResponse(const PayloadHeader& h, const BaselineResponsePayload& body);

    // 解码任意 payload 工件：校验魔数/版本，读头，按 kind 还原对应负载体写入 *out。
    // 返回 false 并填 *err 表示失败（魔数错/版本不支持/头损坏/解压失败/未知 kind 等）。
    bool decode(const QByteArray& data, DecodeResult* out, QString* err);

    // 带类型的 ACK（回执）工件编解码。ACK 不含 header、不压缩，格式极简（见 .cpp）。
    QByteArray encodeChangesetAck(const ChangesetAck& ack);  // 对增量变更集的确认
    QByteArray encodeChunkAck(const PushChunkAck& ack);  // 对选择性推送单个分片的确认
    bool decodeChangesetAck(const QByteArray& data, ChangesetAck* out);  // 还原 ChangesetAck
    bool decodeChunkAck(const QByteArray& data, PushChunkAck* out);      // 还原 PushChunkAck

   private:
    static constexpr quint32 kMagic =
        0x44425359u;  // 魔数 "DBSY"（'D'=0x44 'B'=0x42 'S'=0x53 'Y'=0x59）
    // M-03 fix：把线格式版本提升到 2，用以标记「头里带 senderPeer」。
    // version=1 的旧负载（老节点）头里【没有】senderPeer；readHeader 仅在 v>=2 时才去读它。
    // 新节点写 version=2，但仍能解码 version=1（向后兼容，可与老节点混跑）。
    static constexpr quint16 kVersion = 2;
    static constexpr quint16 kVersionMin = 1;  // 可接受的最低版本（向后兼容下限）

    // 负载/工件种类的单字节标识（写在线格式第 7 字节处）。
    enum KindByte : quint8 {
        KChangeset = 0,        // 普通增量变更集
        KSelectionPush = 1,    // 选择性推送
        KChangesetAck = 2,     // 变更集 ACK
        KChunkAck = 3,         // 分片 ACK
        KBaselineRequest = 4,  // 基线请求
        KBaselineResponse = 5  // 基线应答
    };

    // 把 PayloadHeader 各字段按固定顺序写入流（顺序必须与 readHeader 完全一致）。
    void writeHeader(QDataStream& ds, const PayloadHeader& h);
    // 从流中读回 PayloadHeader。
    // M-03 fix：version 参数决定流里是否存在「可选字段」——v>=2 才读 senderPeer，v=1 不读。
    bool readHeader(QDataStream& ds, PayloadHeader* h, quint16 version, QString* err);
};

}  // namespace dbridge::sync
