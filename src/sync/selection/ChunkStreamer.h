#pragma once
#include "dbridge/sync/SyncTypes.h"

#include <QList>
#include <QString>
#include <QUuid>

#include "../payload/PayloadCodec.h"
#include "FkClosureBuilder.h"

// ============================================================================
// ChunkStreamer.h — 选择性推送第 4 阶段：把「冻结闭包清单」切片成多个传输分片
// ============================================================================
//
// 【职责一句话】
//   输入是一份「已补好外键闭包、已拓扑排序」的清单（FkClosureBuilder::Entry 列表，
//   父先子后）；输出是若干个 Chunk（分片），使「单个分片的字节体积」控制在调用方
//   给定的预算（chunkBudgetBytes / pushChunkBudgetBytes）之内。每个分片随后会被
//   PayloadCodec 编码成一个独立的 SelectionPush artifact 发出去。
//   一言以蔽之：把一份「可能很大」的选择集，拆成「一口能咽下」的多块，逐块传输。
//
// 【在选择性推送链路中的位置】
//   SyncSelection（用户的选择意图：要推哪些表的哪些主键）
//     → SelectionResolver（把 表+主键 → 真实整行 QVariantMap，物化数据）
//     → FkClosureBuilder（补外键父行闭包 + 拓扑排序 → 闭包清单 manifest，父先子后）
//     → FrozenManifest（冻结清单并记指纹，用于检测发送窗口内行是否漂移）
//     → ★ChunkStreamer★（本文件：按字节预算把 manifest 切成多个分片 Chunk）
//     → PayloadCodec::encodeSelectionPush（把每个 Chunk 编码成 SelectionPush 工件）
//     → transport（写 outbox → 投递到对端 inbox）
//
// 【为什么要分片（chunking）】
//   ① 传输/内存上限：一次推送可能涉及成千上万行（尤其闭包放大后），整包一次性传输
//      会撑爆单个 artifact 的体积约定、占用过多内存、也不利于失败重传（重传整包代价高）。
//      切成多片后，单片可独立编码、独立 ACK、独立重传（见 PushChunkAck）。
//   ② 进度与背压：分片让接收端可逐片应用、逐片回 ACK（PushChunkAck），发送端据此
//      判断「是否收齐 totalChunks」并推进进度；某片失败也只需重发那一片。
//
// 【分片如何与下游衔接（计数语义，务必吃透）】
//   · 同一次推送的所有分片共享同一个 pushId（UUID）——它把散落的分片归属到「同一批」。
//   · chunkSeq：本片在批次内的序号，从 0 开始连续递增（0,1,2,…）。
//   · totalChunks：该批次的总片数。接收端据「已收到的 chunkSeq 集合是否凑满
//     [0, totalChunks) 」判断是否收齐（见 SyncTypes.h::PushChunkAck::totalChunks）。
//   · 这三个字段最终会被搬进 PayloadHeader（pushId/chunkSeq/totalChunks）与
//     SelectionPushBody（pushId/chunkSeq/totalChunks），由 PayloadCodec 写进工件。
//
// 【与冻结/指纹的关系】
//   本类不做指纹运算的「业务决策」，但在切片时顺手把每个 Entry 转成 FrozenEntry
//   （含 pkHash 与行内容 fingerprint，见 SyncTypes.h::FrozenEntry），使下游拿到的
//   分片即「自带冻结快照」，可直接交给编解码与漂移检测。
//
// 【协作者】
//   · FkClosureBuilder::Entry（FkClosureBuilder.h）：本类的输入元素（闭包清单的一行）。
//   · FrozenEntry / SelectionPushBody（include/dbridge/sync/SyncTypes.h，已注释）：分片的
//     输出结构与最终负载体。
//   · PayloadCodec（src/sync/payload/PayloadCodec.h，已注释）：把分片编码成工件；本类
//     签名里已预留 codec 形参，便于将来在此就地做线编码（当前未用，见 .cpp 的 Q_UNUSED）。
//   · 错误码见 include/dbridge/Errors.h：单行超预算 → E_SYNC_SELECTION_TOO_LARGE。
//
// 命名空间 dbridge::sync；注释风格对齐 Errors.h / SyncTypes.h / FkClosureBuilder.h
// （`// ──` 分节、中文、信息密集）。
// ============================================================================

namespace dbridge::sync {

// Splits a topologically sorted manifest into chunks within budget.
//   （把一份已拓扑排序的清单切成「单片体积不超预算」的若干分片。）
//
// 本类无状态、可复用同一实例；不持有任何资源或连接。一次 stream() 调用是自包含的：
// 输入一份 manifest，输出一份分片列表，互不影响。
class ChunkStreamer {
   public:
    // ── Chunk —— 一个分片：一批「连续的若干行」+ 其批次/序号元数据 ────────────────
    // 一个 Chunk 直接对应将来一个 SelectionPush artifact 的负载内容：
    //   entries 与 rows 严格同序、一一对应（rows[i] 即 entries[i] 描述的那一行的列值），
    //   这一「平行数组」约定与 SelectionPushBody（frozenEntries/rows）完全一致，便于直接搬运。
    struct Chunk {
        QString pushId;    // 本次推送批次 id（UUID，同批所有分片相同）
        int chunkSeq = 0;  // 本片在批次内的序号（从 0 起连续递增）
        int totalChunks = 0;  // 该批次总片数（接收端据此判断是否收齐 [0,totalChunks)）
        QList<FrozenEntry> entries;  // 本片的冻结清单项（含 pkHash/指纹/拓扑序）
        QList<QVariantMap> rows;     // 本片的行数据；rows[i] ↔ entries[i] 一一对应
    };

    // ── stream —— 顶层入口：把闭包清单按字节预算切成多个分片 ─────────────────────
    // Stream manifest into chunks. pushId is generated (UUID) if empty.
    //   （把 manifest 切片为多个 Chunk；批次 id（pushId）在内部用 UUID 现生成。）
    // chunkBudgetBytes: max serialized bytes per chunk.
    //   （chunkBudgetBytes 是单片序列化字节体积的上限；切分时以「行体积估算」逼近此预算。）
    //
    // 做什么：顺序遍历 manifest（已是父先子后的拓扑序，本类不改变其相对顺序），用「行体积
    //   估算」做贪心装箱——往当前分片塞行，塞不下（会超预算）就另起一片；同时把每个
    //   Entry 转成 FrozenEntry（算 pkHash 与行指纹）。最终给所有分片回填 chunkSeq/totalChunks。
    // 为什么保持原序切分：manifest 的拓扑序（父先子后）是对端能逐行 UPSERT 的前提；分片只是
    //   把这条有序长链「按段截断」，绝不打乱顺序，否则会破坏外键可应用性不变量。
    // 参数：
    //   manifest        输入闭包清单（FkClosureBuilder 产出，已拓扑排序）；空则直接成功返回。
    //   origin          变更来源节点 id（透传给上层填进 PayloadHeader）；本函数当前未直接使用。
    //   targetPeer      目标对端（当前未用，见 .cpp Q_UNUSED；保留以备将来按对端定制分片）。
    //   chunkBudgetBytes 单片字节预算；切分边界判定的依据（>0 才生效；见 .cpp 装箱逻辑）。
    //   codec           载荷编解码器（当前未用，预留：将来可在此就地做线编码以更精确控体积）。
    //   chunks          出参：切好的分片列表（按 chunkSeq 升序追加；调用方负责非空且通常已空）。
    //   err             出参：失败时写入错误码文本（见下错误模式）。
    // 返回：true=切片成功（*chunks 已填好）；false=失败（*err 已写）。
    // 副作用：向 *chunks 追加元素；对每行做一次 SHA-1 指纹运算（entryToFrozen）。
    // 错误模式：单独一行的估算体积就超过 chunkBudgetBytes（无法再细分）→ 失败并写
    //   E_SYNC_SELECTION_TOO_LARGE（见 Errors.h；行级粒度无法切分到更小，只能上报）。
    // 线程：无内部同步，约定在单线程（worker 线程）内串行调用；本类无共享可变状态。
    // 复杂度：O(行数 × 平均每行列数)——每行一次体积估算 + 一次指纹运算，单趟线性扫描。
    bool stream(const QList<FkClosureBuilder::Entry>& manifest, const QString& origin,
                const QString& targetPeer, qint64 chunkBudgetBytes, PayloadCodec& codec,
                QList<Chunk>* chunks, QString* err);
};

}  // namespace dbridge::sync
