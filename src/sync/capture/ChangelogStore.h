#pragma once
#include <QByteArray>
#include <QList>
#include <QSqlDatabase>
#include <QString>

// ============================================================================
// ChangelogStore.h — __sync_changelog 变更日志表的读写层
// ============================================================================
//
// 【这个类是什么】
//   ChangelogStore 是 __sync_changelog 表的薄持久化封装。该表是同步子系统的「行级变更
//   日志」——把本节点被 session 捕获到的变更、以及从对端收到/转发的变更，逐条以二进制
//   changeset 的形式落盘存档，供后续「打包广播给各 peer」之用。它只做 SQL 读写，不建表
//   （DDL 由 schema 子系统负责）。
//
// 【在同步管线中的位置】
//   capture（session 捕获本地写）→〔本类 append：写入 changelog〕→ broadcast（readRangeAll
//   取出待发条目）→ transport（发给 peer）。对端发来的变更经 apply 后，若需继续向其它 peer
//   转发，也会经 appendForward 写进同一张日志。truncate 负责日志的 GC/压实。
//
// 【一条日志记录的关键字段（与下面 Entry/EntryFull 对应）】
//   · local_seq   —— 本地自增主键，全局 FIFO 发送序（广播按它升序遍历）。
//   · origin      —— 该变更最初产生于哪个节点（不是「谁转发给我」）。
//   · origin_seq  —— 该变更在其 origin 流中的序号（同一 origin 内严格递增，用于查空洞/去重）。
//   · stream_epoch—— origin 的流纪元；origin 重置基线后纪元 +1，区分「同序号不同代」。
//   · source_peer —— 直接把这条变更发给我的那个 peer（可为空，表示本地自产）。
//   · kind        —— 记录类型，如 "forward"（转发）/ 本地捕获等。
//   · authoritative — 是否权威（自产捕获通常 true）。
//   · push_id     —— 选择性推送(selection push)的批次标识；普通变更为 NULL。
//   · changeset / payload_checksum / byte_size —— 变更字节、其 SHA-256 校验、字节数。
//
// 【协作者】
//   · SyncWorker / broadcast 层 —— 调 append/appendForward 写入、readRangeAll 取出广播。
//   · PeerAnchorStore（水位）  —— 配合 maxLocalSeq 初始化 last_sent 水位线。
//   · ChangesetApplier         —— 应用对端变更后，转发路径再调 appendForward。
// 【线程】QSqlDatabase 连接不可跨线程并发使用；所有方法须在持有该连接的线程内调用。
// ============================================================================

namespace dbridge::sync {

// Persistence layer for __sync_changelog.
// __sync_changelog 表的持久化层（只读写，不建表）。
class ChangelogStore {
   public:
    // 初始化探测：用 `WHERE 0`（不取任何行，只验结构）确认 __sync_changelog 存在且可查。
    // 失败 → 返回 false 并把驱动错误文本写入 *err。副作用：执行一次 SELECT。
    bool init(QSqlDatabase& db, QString* err);

    // Branch B/C: write a locally-captured changeset (fresh capture or
    // re-encoded incoming). authoritative=true for own captures.
    // H-01 fix: accepts optional pushId so selection-push changesets record their push_id,
    // enabling the broadcast barrier to filter by this specific push rather than all pushes
    // from the same origin.
    // 分支 B/C：写入一条「本地产出」的 changeset——可能是本机刚捕获的新变更，或是把收到的
    //   变更重新编码后的产物。本机自产捕获时 authoritative 传 true。
    // 【H-01 修复】可选 pushId：选择性推送的 changeset 记录其 push_id，使广播屏障(barrier)能
    //   按「这一次具体推送」过滤，而不是粗暴地屏蔽同一 origin 的所有推送。
    // 【参数】kind 记录类型；origin/originSeq/epoch 来源三元组与纪元；parentSeq 父序号(0=无)；
    //   schemaVer/schemaFp 表结构版本与指纹；changeset 变更字节；localSeqOut 出参回填自增主键。
    // 【返回】成功 true；INSERT 失败 false 并置 *err。委托给私有 insertRow 实现。
    bool append(QSqlDatabase& db, const QString& kind, const QString& origin,
                const QString& sourcePeer, qint64 originSeq, qint64 parentSeq, qint64 epoch,
                qint64 schemaVer, const QString& schemaFp, const QByteArray& changeset,
                bool authoritative, qint64* localSeqOut, QString* err,
                const QString& pushId = QString());

    // Branch A: store incoming raw blob verbatim (forwarded changeset).
    // M-04 fix: accepts optional pushId (empty for plain changesets, non-empty for selection push
    // changesets) so the broadcast layer can filter entries by push_id barrier.
    // 分支 A：把收到的对端变更「原样」存档（转发用）。固定 kind="forward"、parentSeq=0、
    //   authoritative=false（转发的不是本机权威产出）。
    // 【M-04 修复】可选 pushId：普通变更为空、选择性推送非空，供广播层按 push_id 屏障过滤。
    bool appendForward(QSqlDatabase& db, const QString& origin, const QString& sourcePeer,
                       qint64 originSeq, qint64 epoch, qint64 schemaVer, const QString& schemaFp,
                       const QByteArray& changesetBlob, qint64* localSeqOut, QString* err,
                       const QString& pushId = QString());

    // Read entries after a peer anchor for broadcasting.
    // 广播读取的「精简条目」：按某 peer 锚点之后取该 origin 的变更。
    struct Entry {
        qint64 localSeq;       // 本地自增序
        QString origin;        // 变更来源节点
        qint64 originSeq;      // 在 origin 流中的序号
        QByteArray changeset;  // 变更字节
        qint64 byteSize;       // 字节数
    };
    // 按 (peer==origin) 锚点读取：返回该 origin 流中 origin_seq > afterOriginSeq 的条目，
    //   按 origin_seq 升序、最多 limit 条。调用方做「同一 origin」范围读取时用。
    QList<Entry> readRange(QSqlDatabase& db, const QString& peer, qint64 afterOriginSeq,
                           int limit = 1000);

    // Full entry including origin, used for broadcast fan-out (J-01 fix).
    // 广播扇出(fan-out)用的「完整条目」(J-01 修复)：比 Entry 多带 stream_epoch 与 push_id。
    struct EntryFull {
        qint64 localSeq = 0;
        QString origin;
        qint64 originSeq = 0;
        qint64 streamEpoch = 0;  // C-04 fix: preserve original stream_epoch per origin
                                 // 【C-04 修复】保留每个 origin 各自的原始 stream_epoch，
                                 // 转发时不会被本地节点的纪元覆盖。
        QByteArray changeset;
        qint64 byteSize = 0;
        QString pushId;  // H-01 fix: non-empty for selection-push changesets; used by broadcast
                         // barrier to skip entries only when their specific push is still pending
                         // 【H-01 修复】选择性推送的条目非空；广播屏障据此「仅当该次推送仍挂起」
                         //   时跳过对应条目（而非一刀切屏蔽整个 origin）。
    };

    // Read all entries with local_seq > afterLocalSeq whose origin != excludeOrigin.
    // Ordered by local_seq ASC (FIFO send order).  Used for broadcasting to a peer
    // so that we never echo a peer's own changes back to it, and always include our
    // own local changes (J-01 fix).
    // 读取所有 local_seq > afterLocalSeq 且 origin != excludeOrigin 的条目，按 local_seq 升序
    //   (FIFO 发送序)。用于向某 peer 广播：excludeOrigin 设为该 peer 自身的 origin，从而
    //   「绝不把 peer 自己的变更回声给它」，同时总是带上本机自产的变更(J-01 修复)。
    QList<EntryFull> readRangeAll(QSqlDatabase& db, const QString& excludeOrigin,
                                  qint64 afterLocalSeq, int limit = 1000);

    // Return the maximum local_seq in the changelog, or -1 if empty.
    // Used to initialise last_sent watermarks (J-01 fix).
    // 返回日志中最大的 local_seq；空表返回 -1。用于初始化各 peer 的 last_sent 发送水位(J-01)。
    qint64 maxLocalSeq(QSqlDatabase& db);

    // Delete entries with local_seq < beforeLocalSeq (GC / compaction).
    // 删除 local_seq < beforeLocalSeq 的旧条目（日志 GC / 压实）。失败置 *err 返回 false。
    bool truncate(QSqlDatabase& db, qint64 beforeLocalSeq, QString* err);

   private:
    // Shared INSERT helper; returns auto-assigned local_seq via lastInsertId.
    // 共享的 INSERT 实现；append 与 appendForward 都走它。成功后经 lastInsertId 回填 local_seq。
    bool insertRow(QSqlDatabase& db, const QString& kind, const QString& origin,
                   const QString& sourcePeer, qint64 originSeq, qint64 parentSeq, qint64 epoch,
                   qint64 schemaVer, const QString& schemaFp, const QByteArray& changeset,
                   bool authoritative, qint64* localSeqOut, QString* err,
                   const QString& pushId = QString());  // M-04 fix

    // Compute SHA-256 hex of a changeset blob (for payload_checksum).
    // 计算 changeset 字节的 SHA-256 十六进制串（写入 payload_checksum 列，用于完整性校验）。
    static QString blobChecksum(const QByteArray& data);
};

}  // namespace dbridge::sync
