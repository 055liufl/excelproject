// ============================================================================
// PayloadCodec.cpp — 载荷编解码器的实现（线格式的「字节真理来源」）
// ============================================================================
//
// 【本文件实现什么】
//   PayloadCodec.h 声明的全部 encode*/decode* 方法。这里是同步系统里唯一规定「字节
//   到底怎么排」的地方：魔数、版本闸、字段写入/读出顺序、压缩、JSON 内嵌等全在此处。
//
// 【全局不变量（读代码前先记住）】
//   · 所有流统一 ds.setVersion(QDataStream::Qt_5_12)：跨版本节点必须用同一 QDataStream
//     版本，否则 QString/QVariant 等的内部编码会不一致，导致解码错位。
//   · 写入顺序 == 读出顺序：writeHeader 与 readHeader 的字段序必须逐字段对齐；任何一处
//     增删字段都要同时改两端，并提升 kVersion + 在 readHeader 里按版本分支。
//   · 头之后的 body 一律是「先 qCompress 的一段 QByteArray」：解码端先 >> 出压缩块，再
//     qUncompress；changeset 体解出来即原始 blob，其余三类解出来是 JSON 文本。
// ============================================================================

#include "PayloadCodec.h"

#include <QBuffer>
#include <QDataStream>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace dbridge::sync {

// ---------------------------------------------------------------------------
// 私有辅助：信封头的写入 / 读出（writeHeader 与 readHeader 必须字段序严格镜像）
// ---------------------------------------------------------------------------

// writeHeader —— 把 PayloadHeader 各字段顺序写入流。
// 字段序（即线格式中头部的物理布局，新增字段只能往末尾追加）：
//   origin → originSeq → parentSeq → schemaFingerprint → schemaVer → streamEpoch
//   → routeTag → pushId → chunkSeq(qint32) → totalChunks(qint32) → senderPeer
// 注意：chunkSeq/totalChunks 在结构里是 int，这里显式转 qint32 写出，保证宽度跨平台稳定。
void PayloadCodec::writeHeader(QDataStream& ds, const PayloadHeader& h) {
    ds << h.origin << h.originSeq << h.parentSeq << h.schemaFingerprint << h.schemaVer
       << h.streamEpoch << h.routeTag << h.pushId << static_cast<qint32>(h.chunkSeq)
       << static_cast<qint32>(h.totalChunks)
       << h.senderPeer;  // C-05 fix：物理发送者，让接收端能把 ACK 回执到正确的节点（≠ origin）
}

// readHeader —— 从流中按 writeHeader 的顺序读回 PayloadHeader。
// 参数 version 决定是否读「可选末尾字段」senderPeer（v>=2 才有）。
// 返回 false 并填 *err 表示头损坏（流状态非 Ok）。
bool PayloadCodec::readHeader(QDataStream& ds, PayloadHeader* h, quint16 version, QString* err) {
    qint32 cs = 0, tc = 0;  // 先读进 qint32 临时变量，再回填到 int 成员（与写出宽度对齐）
    ds >> h->origin >> h->originSeq >> h->parentSeq >> h->schemaFingerprint >> h->schemaVer >>
        h->streamEpoch >> h->routeTag >> h->pushId >> cs >> tc;
    h->chunkSeq = cs;
    h->totalChunks = tc;
    if (ds.status() != QDataStream::Ok) {
        if (err)
            *err = QStringLiteral("payload header read error");
        return false;
    }
    // M-03 fix：senderPeer 只在线格式 version >= 2 时存在。
    // 老负载（version=1）头里没有 senderPeer；若硬读就会「吃掉」紧随其后的 body 字节，
    // 把 changeset 读坏。因此显式按 version 判断，而不是依赖 ReadPastEnd（那样会误判损坏）。
    if (version >= 2) {
        ds >> h->senderPeer;
        if (ds.status() != QDataStream::Ok) {
            if (err)
                *err = QStringLiteral("payload header senderPeer read error");
            return false;
        }
    } else {
        // version=1：线格式里没有 senderPeer，默认为空（语义即「发送者 == origin」）。
        h->senderPeer.clear();
    }
    return true;
}

// ---------------------------------------------------------------------------
// public：编码（encode）
//   四个 encode* 方法共享同一骨架：写 magic+version+kind → writeHeader → 写压缩后的 body。
//   差别只在 body 的构造方式（changeset 是 blob，其余三类是 JSON）。
// ---------------------------------------------------------------------------

// encodeChangeset —— 把 SQLite 原始变更集打包成 Changeset 工件。
// body = qCompress(changeset)（changeset 已是二进制 blob，直接压缩即可）。
QByteArray PayloadCodec::encodeChangeset(const PayloadHeader& h, const QByteArray& changeset) {
    QByteArray buf;
    QDataStream ds(&buf, QIODevice::WriteOnly);
    ds.setVersion(QDataStream::Qt_5_12);
    ds << kMagic << kVersion << static_cast<quint8>(KChangeset);  // 前 7 字节：魔数+版本+kind
    writeHeader(ds, h);
    ds << qCompress(changeset);  // body：压缩后的变更集 blob
    return buf;
}

// encodeSelectionPush —— 把「选择性推送」打包成 SelectionPush 工件。
// body 是 JSON（便于阅读/演进）再压缩：frozen[] 描述被冻结的行、rows[] 是对应行数据。
QByteArray PayloadCodec::encodeSelectionPush(const PayloadHeader& h,
                                             const SelectionPushBody& body) {
    // 为简洁/可读，把 rows 序列化为 JSON 数组（每行一个对象）。
    QJsonArray rowsArr;
    for (const QVariantMap& row : body.rows) {
        rowsArr.append(QJsonObject::fromVariantMap(row));
    }
    // 冻结清单：每项记录该行的定位信息与内容指纹（用于接收端比对/漂移检测）。
    QJsonArray frozenArr;
    for (const FrozenEntry& fe : body.frozenEntries) {
        QJsonObject o;
        o[QStringLiteral("table")] = fe.table;
        o[QStringLiteral("primaryKey")] = fe.primaryKey;
        o[QStringLiteral("pkHash")] = fe.pkHash;
        o[QStringLiteral("recordKind")] = fe.recordKind;
        o[QStringLiteral("topoIndex")] = fe.topoIndex;
        // fingerprint 是二进制，JSON 容不下原始字节 → 用 Base64 文本承载（解码端再还原）。
        o[QStringLiteral("fingerprint")] = QString::fromLatin1(fe.fingerprint.toBase64());
        frozenArr.append(o);
    }
    QJsonObject root;
    root[QStringLiteral("frozen")] = frozenArr;
    root[QStringLiteral("rows")] = rowsArr;
    root[QStringLiteral("pushId")] = body.pushId;
    root[QStringLiteral("chunkSeq")] = body.chunkSeq;
    root[QStringLiteral("totalChunks")] = body.totalChunks;
    QByteArray json =
        QJsonDocument(root).toJson(QJsonDocument::Compact);  // 紧凑 JSON（无多余空白）

    QByteArray buf;
    QDataStream ds(&buf, QIODevice::WriteOnly);
    ds.setVersion(QDataStream::Qt_5_12);
    ds << kMagic << kVersion << static_cast<quint8>(KSelectionPush);
    writeHeader(ds, h);
    ds << qCompress(json);
    return buf;
}

// encodeBaselineRequest —— 把「基线请求」打包成 BaselineRequest 工件。
// 注意：streamEpoch/fromSeq 是 64 位整数，这里以「字符串」存进 JSON——因为 JSON number
// 的安全整数范围只有 53 位，超过会丢精度；用字符串无损（解码端再 toLongLong）。
QByteArray PayloadCodec::encodeBaselineRequest(const PayloadHeader& h,
                                               const BaselineRequestPayload& body) {
    QJsonArray tablesArr;
    for (const QString& table : body.requestedTables)
        tablesArr.append(table);

    QJsonObject root;
    root[QStringLiteral("origin")] = body.origin;
    root[QStringLiteral("streamEpoch")] =
        QString::number(body.streamEpoch);  // 64 位→字符串，防丢精度
    root[QStringLiteral("requestedTables")] = tablesArr;
    root[QStringLiteral("fromSeq")] = QString::number(body.fromSeq);  // 同上
    root[QStringLiteral("pendingArtifactName")] = body.pendingArtifactName;
    QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);

    QByteArray buf;
    QDataStream ds(&buf, QIODevice::WriteOnly);
    ds.setVersion(QDataStream::Qt_5_12);
    ds << kMagic << kVersion << static_cast<quint8>(KBaselineRequest);
    writeHeader(ds, h);
    ds << qCompress(json);
    return buf;
}

// encodeBaselineResponse —— 把「基线应答」打包成 BaselineResponse 工件（同步里最大的负载）。
// 携带全量 baselineData（Base64）与各 origin 的水位切点 originCuts，让接收端能精确对齐。
QByteArray PayloadCodec::encodeBaselineResponse(const PayloadHeader& h,
                                                const BaselineResponsePayload& body) {
    QJsonArray tablesArr;
    for (const QString& table : body.tables)
        tablesArr.append(table);

    // C-03 fix：序列化「逐 origin 的 applied-vector 切点」(origin + stream_epoch + appliedSeq)。
    // 格式：由 {"o":origin, "e":epoch, "s":appliedSeq} 组成的 JSON 数组。
    // 旧版的 "originMaxSeq" 键（缺少 epoch 的 QHash）已不再写出（彻底废弃）。
    // 字段名刻意取单字母 o/e/s，是为压缩前减小 JSON 体积（基线本就大，逐行水位条目可能很多）。
    QJsonArray originCutsArr;
    for (const BaselineOriginCut& cut : body.originCuts) {
        QJsonObject obj;
        obj[QStringLiteral("o")] = cut.origin;
        obj[QStringLiteral("e")] = QString::number(cut.streamEpoch);  // 64 位→字符串
        obj[QStringLiteral("s")] = QString::number(cut.appliedSeq);   // 64 位→字符串
        originCutsArr.append(obj);
    }

    QJsonObject root;
    root[QStringLiteral("origin")] = body.origin;
    root[QStringLiteral("requestOrigin")] = body.requestOrigin;
    root[QStringLiteral("streamEpoch")] = QString::number(body.streamEpoch);
    root[QStringLiteral("tables")] = tablesArr;
    root[QStringLiteral("fromSeq")] = QString::number(body.fromSeq);
    root[QStringLiteral("pendingArtifactName")] = body.pendingArtifactName;
    // baselineData 是基线全量二进制 → Base64 文本内嵌 JSON（注意：此处会被 JSON 放大约 4/3，
    // 之后整段 JSON 又会被 qCompress 压回一部分）。
    root[QStringLiteral("baselineData")] = QString::fromLatin1(body.baselineData.toBase64());
    root[QStringLiteral("sourceMaxSeq")] = QString::number(body.sourceMaxSeq);
    root[QStringLiteral("originCuts")] = originCutsArr;
    QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);

    QByteArray buf;
    QDataStream ds(&buf, QIODevice::WriteOnly);
    ds.setVersion(QDataStream::Qt_5_12);
    ds << kMagic << kVersion << static_cast<quint8>(KBaselineResponse);
    writeHeader(ds, h);
    ds << qCompress(json);
    return buf;
}

// ---------------------------------------------------------------------------
// public：解码（decode）
//   单一入口处理全部 4 种 payload：先做「魔数→版本→读头→解压 body」的公共前段，再按
//   kind 分派到各自的还原分支。任一步失败均返回 false 并填 *err（上层归类为
//   E_SYNC_PAYLOAD_CORRUPT）。
// ---------------------------------------------------------------------------

bool PayloadCodec::decode(const QByteArray& data, DecodeResult* out, QString* err) {
    QDataStream ds(data);
    ds.setVersion(QDataStream::Qt_5_12);  // 必须与编码端同版本，否则字段解码错位

    // 读前 7 字节：魔数(4) + 版本(2) + kind(1)。
    quint32 magic = 0;
    quint16 ver = 0;
    quint8 kind = 0;
    ds >> magic >> ver >> kind;
    if (magic != kMagic) {
        // 魔数不符：根本不是本系统的工件（或字节流被截断/污染）。
        if (err)
            *err = QStringLiteral("bad magic number");
        return false;
    }
    // C-03 fix：刚校验完魔数就立刻保存「完整原始字节」到 rawPayload。
    // 这样无论后续如何分派，rawPayload 都有值，隔离区(quarantine)能用 codec->decode() 原样重放。
    // 旧版从不给 rawPayload 赋值，导致因 schema 不匹配进隔离区时存成空 BLOB，永久丢失负载。
    out->rawPayload = data;
    // M-03 fix：接受 version=1（不带 senderPeer 的老节点）与 version=2（带 senderPeer 的新节点）。
    // 落在 [kVersionMin, kVersion] 之外的一律视为真正不支持而拒绝（既防过旧、也防来自未来的新版）。
    if (ver < kVersionMin || ver > kVersion) {
        if (err)
            *err = QStringLiteral("unsupported codec version %1").arg(ver);
        return false;
    }
    // 读信封头（按 ver 决定是否读 senderPeer，见 readHeader）。
    if (!readHeader(ds, &out->header, ver, err))
        return false;

    // 读 body：先 >> 出「压缩块」，再 qUncompress 还原。
    QByteArray compressed;
    ds >> compressed;
    if (ds.status() != QDataStream::Ok) {
        if (err)
            *err = QStringLiteral("payload body read error");
        return false;
    }
    QByteArray body = qUncompress(compressed);
    // 关键判别：qUncompress 失败也返回空 QByteArray。只有「压缩块本身非空、解出来却空」才是
    // 真损坏；若压缩块本就为空（合法的空 body），解出空属正常，不报错。
    if (body.isEmpty() && !compressed.isEmpty()) {
        if (err)
            *err = QStringLiteral("payload decompression failed");
        return false;
    }

    // ── 按 kind 分派：还原对应负载体，并设定 out->kind ─────────────────────────

    if (kind == KChangeset) {
        // 变更集：body 即原始 changeset blob，直接交付。
        out->kind = PayloadKind::Changeset;
        out->changeset = body;
        return true;
    }
    if (kind == KSelectionPush) {
        // 选择性推送：body 是 JSON，逆 encodeSelectionPush 还原 rows 与 frozen[]。
        out->kind = PayloadKind::SelectionPush;
        QJsonParseError pe;
        QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
        if (doc.isNull()) {
            if (err)
                *err = QStringLiteral("selection JSON: %1").arg(pe.errorString());
            return false;
        }
        QJsonObject root = doc.object();
        SelectionPushBody& sb = out->selection;
        sb.pushId = root[QStringLiteral("pushId")].toString();
        sb.chunkSeq = root[QStringLiteral("chunkSeq")].toInt();
        sb.totalChunks = root[QStringLiteral("totalChunks")].toInt();
        for (const QJsonValue& rv : root[QStringLiteral("rows")].toArray())
            sb.rows.append(rv.toObject().toVariantMap());
        for (const QJsonValue& fv : root[QStringLiteral("frozen")].toArray()) {
            QJsonObject fo = fv.toObject();
            FrozenEntry fe;
            fe.table = fo[QStringLiteral("table")].toString();
            fe.primaryKey = fo[QStringLiteral("primaryKey")].toString();
            fe.pkHash = fo[QStringLiteral("pkHash")].toString();
            fe.recordKind = fo[QStringLiteral("recordKind")].toString();
            fe.topoIndex = fo[QStringLiteral("topoIndex")].toInt();
            // fingerprint 在编码端转过 Base64，这里 fromBase64 还原成原始字节。
            fe.fingerprint =
                QByteArray::fromBase64(fo[QStringLiteral("fingerprint")].toString().toLatin1());
            sb.frozenEntries.append(fe);
        }
        return true;
    }
    if (kind == KBaselineRequest) {
        // 基线请求：body 是 JSON。注意 64 位字段是以字符串存的，这里 toLongLong 还原。
        out->kind = PayloadKind::BaselineRequest;
        QJsonParseError pe;
        QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
        if (doc.isNull()) {
            if (err)
                *err = QStringLiteral("baseline request JSON: %1").arg(pe.errorString());
            return false;
        }
        QJsonObject root = doc.object();
        BaselineRequestPayload& br = out->baselineRequest;
        br.origin = root[QStringLiteral("origin")].toString();
        br.streamEpoch =
            root[QStringLiteral("streamEpoch")].toString().toLongLong();       // 字符串→64 位
        br.fromSeq = root[QStringLiteral("fromSeq")].toString().toLongLong();  // 同上
        br.pendingArtifactName = root[QStringLiteral("pendingArtifactName")].toString();
        for (const QJsonValue& tv : root[QStringLiteral("requestedTables")].toArray())
            br.requestedTables.append(tv.toString());
        return true;
    }
    if (kind == KBaselineResponse) {
        // 基线应答：body 是 JSON。还原全量 baselineData（Base64→字节）与 originCuts。
        out->kind = PayloadKind::BaselineResponse;
        QJsonParseError pe;
        QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
        if (doc.isNull()) {
            if (err)
                *err = QStringLiteral("baseline response JSON: %1").arg(pe.errorString());
            return false;
        }
        QJsonObject root = doc.object();
        BaselineResponsePayload& br = out->baselineResponse;
        br.origin = root[QStringLiteral("origin")].toString();
        br.requestOrigin = root[QStringLiteral("requestOrigin")].toString();
        br.streamEpoch = root[QStringLiteral("streamEpoch")].toString().toLongLong();
        br.fromSeq = root[QStringLiteral("fromSeq")].toString().toLongLong();
        br.pendingArtifactName = root[QStringLiteral("pendingArtifactName")].toString();
        br.baselineData =
            QByteArray::fromBase64(root[QStringLiteral("baselineData")].toString().toLatin1());
        br.sourceMaxSeq = root[QStringLiteral("sourceMaxSeq")].toString().toLongLong();
        for (const QJsonValue& tv : root[QStringLiteral("tables")].toArray())
            br.tables.append(tv.toString());
        // C-03 fix：逆序还原「逐 origin 的 applied-vector 切点」（含 stream_epoch）。
        // 字段名 o/e/s 与 encodeBaselineResponse 写出的紧凑键一一对应。
        for (const QJsonValue& cv : root[QStringLiteral("originCuts")].toArray()) {
            const QJsonObject obj = cv.toObject();
            BaselineOriginCut cut;
            cut.origin = obj[QStringLiteral("o")].toString();
            cut.streamEpoch = obj[QStringLiteral("e")].toString().toLongLong();
            cut.appliedSeq = obj[QStringLiteral("s")].toString().toLongLong();
            br.originCuts.append(cut);
        }
        return true;
    }
    // kind 不在已知枚举内：未知种类（可能是更新版本引入、或字节损坏）。
    if (err)
        *err = QStringLiteral("unknown payload kind %1").arg(kind);
    return false;
}

// ---------------------------------------------------------------------------
// ACK 编解码（回执工件）
//   ACK 是接收端「我已应用到第几号」的回执，结构极简：复用同样的 magic+version+kind 前缀，
//   随后直接平铺各字段——无 PayloadHeader、body 不压缩。decode 只校验 magic 与 kind，
//   再靠流末状态(ds.status())判断是否完整读出。
// ---------------------------------------------------------------------------

// encodeChangesetAck —— 编码「变更集 ACK」。字段序：origin → streamEpoch → appliedSeq → toPeer。
QByteArray PayloadCodec::encodeChangesetAck(const ChangesetAck& ack) {
    QByteArray buf;
    QDataStream ds(&buf, QIODevice::WriteOnly);
    ds.setVersion(QDataStream::Qt_5_12);
    // M-09 fix：把 toPeer 也写进去，让 ACK 工件「自描述」（路由不再仅靠文件名约定）。
    ds << kMagic << kVersion << static_cast<quint8>(KChangesetAck) << ack.origin << ack.streamEpoch
       << ack.appliedSeq << ack.toPeer;
    return buf;
}

// encodeChunkAck —— 编码「分片 ACK」。字段序：pushId → chunkSeq → totalChunks → checksum → ok →
// toPeer。 chunkSeq/totalChunks 显式转 qint32 写出（与 decodeChunkAck 的读回宽度对齐）。
QByteArray PayloadCodec::encodeChunkAck(const PushChunkAck& ack) {
    QByteArray buf;
    QDataStream ds(&buf, QIODevice::WriteOnly);
    ds.setVersion(QDataStream::Qt_5_12);
    // M-09 fix：把 toPeer 写进去，让分片 ACK 自描述。
    ds << kMagic << kVersion << static_cast<quint8>(KChunkAck) << ack.pushId
       << static_cast<qint32>(ack.chunkSeq) << static_cast<qint32>(ack.totalChunks) << ack.checksum
       << ack.ok << ack.toPeer;
    return buf;
}

// decodeChangesetAck —— 还原 ChangesetAck。返回 false 表示魔数/种类不符或读取不完整。
bool PayloadCodec::decodeChangesetAck(const QByteArray& data, ChangesetAck* out) {
    QDataStream ds(data);
    ds.setVersion(QDataStream::Qt_5_12);
    quint32 magic = 0;
    quint16 ver = 0;  // ACK 当前未做版本分支，读出仅为占位/跳过这 2 字节
    quint8 kind = 0;
    ds >> magic >> ver >> kind;
    if (magic != kMagic || kind != KChangesetAck)  // 既要是本系统工件，又必须正好是「变更集 ACK」
        return false;
    ds >> out->origin >> out->streamEpoch >> out->appliedSeq >> out->toPeer;
    return ds.status() == QDataStream::Ok;  // 字段全部完整读出才算成功
}

// decodeChunkAck —— 还原 PushChunkAck。返回 false 表示魔数/种类不符或读取不完整。
bool PayloadCodec::decodeChunkAck(const QByteArray& data, PushChunkAck* out) {
    QDataStream ds(data);
    ds.setVersion(QDataStream::Qt_5_12);
    quint32 magic = 0;
    quint16 ver = 0;
    quint8 kind = 0;
    ds >> magic >> ver >> kind;
    if (magic != kMagic || kind != KChunkAck)
        return false;
    qint32 cs = 0, tc = 0;  // 先读进 qint32 临时变量，再回填 int 成员
    ds >> out->pushId >> cs >> tc >> out->checksum >> out->ok >> out->toPeer;
    out->chunkSeq = cs;
    out->totalChunks = tc;
    return ds.status() == QDataStream::Ok;
}

}  // namespace dbridge::sync
