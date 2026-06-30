#pragma once
#include "dbridge/sync/SyncTypes.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QVector>

#include "../apply/AppliedVectorStore.h"
#include "../apply/RowWinnerStore.h"
#include "../schema/TableStateStore.h"
#include "../selection/ConsistencyCache.h"
#include <sqlite3.h>

// ============================================================================
// BaselineManager.h — 「基线（baseline）全量传输」的导出/导入引擎
// ============================================================================
//
// 【这个类是什么】
//   增量同步平时只传「增量变更集（changeset）」：A 写了一行，就把这一行的差异打包发
//   给 B。但有两种情况增量追不上，必须改用「全量基线」一次性对齐：
//     ① 冷启动：B 是张空白库，没有任何历史，逐条补增量太慢、也未必拿得到最早的增量；
//     ② 落后过多：B 掉线太久，源端早已把它需要的那些老 changeset「压实/清掉」
//        （compact）了——增量已经不在了（见 shouldFallbackToBaseline）。
//   这时由源端导出一份「当前所有同步表的完整快照」= 基线工件（BaselineArtifact），
//   B 收到后把本地表整张替换成这份快照，再从快照的「水位」处接着追增量。
//
// 【在同步管线里的位置（协作者）】
//   · 触发：边缘节点检测到序号空洞(E_SYNC_GAP)或刚入网 → 发 BaselineRequest（编解码
//     由 PayloadCodec 负责）；源端（通常是中心节点）收到后调用本类 exportBaseline()。
//   · 传输：导出的 BaselineArtifact 被装进 BaselineResponsePayload，由 PayloadCodec
//     编码成 artifact 二进制经 outbox→inbox 投递（见 SyncTypes.h 的负载结构）。
//   · 落地：接收端调用本类 applyBaseline()，把快照灌进本地库，并「重置」所有追踪状态
//     存储（applied-vector / table-state / row-winner / consistency-cache），使本地
//     像「刚从这份快照长出来」一样，再无缝衔接后续增量。
//
// 【关键概念（详见 SyncTypes.h 与 AppliedVectorStore.h 的类头）】
//   · origin / seq / stream_epoch：每条变更由 (origin, stream_epoch, seq) 唯一定位；
//     基线必须把「截至导出时各 origin 各自追到的 seq」如实带走，否则接收端的 gap 判定
//     会从错误的起点开始。承载它的就是 BaselineOriginCut（见 SyncTypes.h）。
//   · originCuts（各 origin 的 applied 水位快照，含 stream_epoch）：基线不是「空白起
//     点」而是「截至某些 seq 的全量」；接收端要按每个 cut 自带的 epoch 调用
//     av.resetTo()，把水位设到真实位置（而非一律清零）。这是 C-03 fix 的核心。
//   · 基线体积告警 W_SYNC_BASELINE_LARGE：基线是全量、可能很大；上层据体积发此告警
//     （本类只负责导出/导入，体积阈值判断在调用方）。
//
// 【命名空间】dbridge::sync　【错误码】失败统一冠以 E_SYNC_BASELINE_FAILED（见 .cpp）。
// 注释风格对齐 Errors.h / SyncTypes.h / RowPayload.h：POD 信封 + 逐字段中文详注。
// ============================================================================

namespace dbridge::sync {

// BaselineManager —— 全表基线的导出与应用。
//   导出（export）：把所有同步表的全部行序列化成一段扁平二进制（再压缩）。
//   应用（apply） ：把行导回本地库，并重置所有「向量/状态」追踪存储，使本地与基线对齐。
// 本类无成员状态（不缓存任何东西）：所有上下文都通过参数传入，可在持有写连接的线程上
// 直接复用同一个实例。
class BaselineManager {
   public:
    // BaselineArtifact —— 一次导出的「基线工件」：自包含的全量快照 + 水位元数据。
    // 它是 exportBaseline 的产物、applyBaseline 的输入；序列化后塞进
    // BaselineResponsePayload::baselineData 在节点间传输。
    struct BaselineArtifact {
        QByteArray data;  // 序列化后的基线数据（已 qCompress 压缩；格式见 .cpp serializeTables）
        qint64 sourceMaxSeq =
            0;  // 导出时刻 __sync_changelog 里的最大 local_seq（仅供诊断/作为基线行的种子 seq）
        // C-03 fix：导出时刻「逐 origin 的 applied-vector 快照」（含 stream_epoch）。
        // 取代旧版缺少 stream_epoch 的 QHash<QString,qint64>——旧版会导致 applyBaseline()
        // 用「本地 epoch」去调 av.resetTo()，而非各 origin 自己的 epoch，从而错置远端水位。
        QVector<BaselineOriginCut> originCuts;
    };

    // exportBaseline —— 把 tables 列出的所有表导出为一个 BaselineArtifact（导出方调用）。
    // 做什么：序列化全部行 + 采集各 origin 水位切点，组装成 *out。
    // 参数  ：rconn 只读连接；tables 同步表清单；out 出参（工件）；err 出参（失败原因）。
    //         localOrigin/localEpoch/localOriginSeq —— 导出节点「自己」的身份与当前序号。
    // 返回  ：true=成功；false=失败且 *err 以 E_SYNC_BASELINE_FAILED 开头。
    // 副作用：只读数据库，不修改任何表。
    //
    // M-01 fix：导出节点「自己产生」的写入从不会推进它自己的 __sync_applied_vector
    //   （applied-vector 只记「我应用了别人的第几号」，不记自己的输出），所以
    //   (localOrigin, localEpoch) 这一切点在 queryOriginCuts() 里是缺失的。本函数据
    //   localOriginSeq 把这条「自身切点」按取最大值并入 out->originCuts，避免接收端把
    //   导出方的序号误重置为 0（否则导出方后续增量会全被当成 Gap）。
    bool exportBaseline(QSqlDatabase& rconn, const QStringList& tables, BaselineArtifact* out,
                        QString* err, const QString& localOrigin = QString(), qint64 localEpoch = 0,
                        qint64 localOriginSeq = 0);

    // applyBaseline —— 应用一个基线工件（接收方调用）：把行灌入 wconn，再重置全部追踪存储。
    // 做什么：① 关外键 → 开写事务 → 清表并按快照重灌行；② 用 originCuts 把 applied-vector
    //   逐 (origin,epoch) 重置到权威水位；③ 重置 table-state（写入 schemaFp）；④ 清空并
    //   重新「播种」row-winner（每行一条胜者记录）；⑤ 提交、复原外键、失效一致性缓存。
    // 参数  ：wconn 写连接；h 原生 sqlite3 句柄（当前实现未使用，预留）；art 基线工件；
    //         av/ts/rw/cache 四个待重置的追踪存储；epoch/origin「主」来源的纪元与 id；
    //         schemaFp 远端表结构指纹；newAnchorSeq 出参（新的锚点序号）；err 出参；
    //         baselineRank 基线来源的 rank（用于播种胜者）。
    // 返回  ：true=全部成功并已提交；false=任一步失败，已回滚 + 复原外键，*err 冠
    //         E_SYNC_BASELINE_FAILED。
    // 副作用：清空并重写各同步表与多张 __sync_* 元数据表；改动 PRAGMA foreign_keys（最终复原）。
    // 事务  ：核心步骤全在一个 WriteTxn 内，保证「全量替换 + 状态重置」原子完成。
    //
    // H-05 fix：把调用方给的 schemaFp 写进 table_state，使基线应用后 DiffEngine 能正确
    //   比对指纹，避免误判为「Different」而触发无谓的全表 diff。
    // M-02 fix：baselineRank 是基线来源的 rank；为每个导入行播种 RowWinner 记录时用它，
    //   这样后续「低 rank 的挑战者」无法覆盖基线确立的事实（见 RowWinnerStore.h 仲裁规则）。
    bool applyBaseline(QSqlDatabase& wconn, sqlite3* h, const BaselineArtifact& art,
                       AppliedVectorStore& av, TableStateStore& ts, RowWinnerStore& rw,
                       ConsistencyCache& cache, qint64 epoch, const QString& origin,
                       const QString& schemaFp, qint64* newAnchorSeq, QString* err,
                       int baselineRank = 0);

    // shouldFallbackToBaseline —— 判断「该不该改用基线而非继续追增量」。
    // 做什么：当本地已应用到 appliedSeq，而源端能提供的最早序号 sourceMinSeq 已经比它还
    //   大时（appliedSeq < sourceMinSeq），说明本地缺的那段 changeset 已被源端压实/清除、
    //   永远拿不到了——只能退化为全量基线对齐。
    // 返回  ：true=应回退到基线；false=增量仍可衔接。 副作用：无（纯比较）。
    bool shouldFallbackToBaseline(qint64 appliedSeq, qint64 sourceMinSeq) const;

   private:
    // serializeTables —— 把 tables 全部行序列化进 *out（QDataStream 布局，末尾 qCompress）。
    // 同时把 __sync_changelog 的 MAX(local_seq) 写入 *maxSeq（诊断用）。失败置 *err 返回 false。
    bool serializeTables(QSqlDatabase& rconn, const QStringList& tables, QByteArray* out,
                         qint64* maxSeq, QString* err);

    // deserializeAndApply —— serializeTables 的逆操作：解压并逐表「先清空、后逐行 INSERT」。
    // 解析出的表名列表写入 *tables（供 applyBaseline 后续重置 table-state/胜者用）。
    // 失败（解压失败/数据损坏/SQL 失败）置 *err 返回 false。注意：本函数须在调用方已开启的
    // 写事务内运行，使「清表 + 重灌」可被一起回滚。
    bool deserializeAndApply(QSqlDatabase& wconn, const QByteArray& data, QStringList* tables,
                             QString* err);
};

}  // namespace dbridge::sync
