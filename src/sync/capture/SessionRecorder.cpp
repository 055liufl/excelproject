// ============================================================================
// SessionRecorder.cpp — 变更捕获器的实现（session 钩子的建立、封存与拆除）
// 总览与时序要求见头文件 SessionRecorder.h。
// ============================================================================
#include "SessionRecorder.h"

#include <QByteArray>

namespace dbridge::sync {

// ── begin：建立会话并附着所有同步表 ──────────────────────────────────────────
bool SessionRecorder::begin(sqlite3* h, const QStringList& syncTables, QString* err) {
    // 防御性检查 1：禁止在已有活动会话之上重复 begin（否则会泄漏前一个会话）。
    if (session_) {
        if (err)
            *err = QStringLiteral("SessionRecorder already active");
        return false;
    }
    // 防御性检查 2：句柄必须有效。
    if (!h) {
        if (err)
            *err = QStringLiteral("null sqlite3 handle");
        return false;
    }

    // 在 "main" 数据库上创建会话对象。从此刻起，针对已附表的写操作会被 preupdate 钩子记录。
    int rc = sqlite3session_create(h, "main", &session_);
    if (rc != SQLITE_OK) {
        session_ = nullptr;  // create 失败时 session_ 状态未定义，显式清空以维持不变量
        if (err)
            *err = QStringLiteral("sqlite3session_create failed: %1").arg(sqlite3_errmsg(h));
        return false;
    }

    // 逐张把同步表附着到会话；只有被附着的表，其行变更才会进入 changeset。
    for (const QString& tbl : syncTables) {
        const QByteArray tblUtf8 = tbl.toUtf8();  // C API 需要 UTF-8 C 字符串
        rc = sqlite3session_attach(session_, tblUtf8.constData());
        if (rc != SQLITE_OK) {
            // 任一表附着失败：删除已建会话并复位，整体视为 begin 失败（要么全附上、要么不开始）。
            sqlite3session_delete(session_);
            session_ = nullptr;
            if (err)
                *err = QStringLiteral("sqlite3session_attach failed for '%1': %2")
                           .arg(tbl)
                           .arg(sqlite3_errmsg(h));
            return false;
        }
    }
    return true;
}

// ── sealInto：收集 changeset → 写 changelog → 拆会话（“封存”）─────────────────
// 总览见头文件：本函数必须在与 begin() 相同的那个 WriteTxn 内调用，使 changelog 写入
// 与被记录的业务改动同事务原子落地（要么一起提交，要么一起回滚）。
bool SessionRecorder::sealInto(sqlite3* h, ChangelogStore& store, QSqlDatabase& db, WriteTxn& txn,
                               const QString& origin, qint64 epoch, qint64 schemaVer,
                               const QString& schemaFp, qint64 parentSeq, qint64 originSeq,
                               qint64* outLocalSeq, QString* err, const QString& pushId,
                               QByteArray* outChangeset) {
    // 前置校验 1：必须有活动会话（begin 成功且尚未 seal/abort）。
    if (!session_) {
        if (err)
            *err = QStringLiteral("SessionRecorder not active");
        return false;
    }
    // 前置校验 2：必须处于活动写事务中——这是“原子封存”的根本前提，没有事务就无法保证
    // changelog 与业务改动同生共死。
    if (!txn.isActive()) {
        if (err)
            *err = QStringLiteral("WriteTxn not active during sealInto");
        return false;
    }

    // 从会话提取 changeset 字节。null（出错）与 empty（无变更）的区分见 collectChangeset。
    QByteArray changeset = collectChangeset(err);
    if (changeset.isNull()) {
        // collectChangeset sets *err
        // 译：提取出错（collectChangeset 已写 *err）→ 拆会话放弃，返回失败。
        abort();
        return false;
    }

    // Detach session before writing to avoid re-recording the changelog INSERT.
    // 译：在写 changelog 之前先拆掉会话——否则接下来对 __sync_changelog 的 INSERT 本身
    //   会被同一个 session 再次捕获，造成“变更记录自我递归记录”的污染。
    sqlite3session_delete(session_);
    session_ = nullptr;

    if (changeset.isEmpty()) {
        // No changes captured; still report success with seq=0.
        // 译：本事务实际没捕获到任何行变更（空 changeset）。这不是错误：仍按成功返回，
        //   并令 *outLocalSeq=0 表示“没有可广播的变更”（调用方据 seq==0 知道无需广播）。
        if (outLocalSeq)
            *outLocalSeq = 0;
        return true;
    }

    // M-01 fix: expose changeset bytes to caller for incremental table_state updates.
    // 译：M-01 fix —— 把原始 changeset 字节回带给调用方，使其能据此对 __sync_table_state
    //   做“增量”更新（解析 changeset 得到逐行 add/sub），免去代价高昂的全表 resetFromBaseline。
    if (outChangeset)
        *outChangeset = changeset;

    // 把 changeset 连同全部同步元信息追加进 changelog；append 内部分配并回填 *outLocalSeq
    // （本条记录的 local_seq）。authoritative=true 表示这是本节点权威产生的本地变更；
    // sourcePeer 留空（本地起源，非从某对端转发而来）。
    bool ok = store.append(db, QStringLiteral("changeset"), origin,
                           /*sourcePeer=*/QString(), originSeq, parentSeq, epoch, schemaVer,
                           schemaFp, changeset,
                           /*authoritative=*/true, outLocalSeq, err,
                           /*pushId=*/pushId);  // H-01 fix: forward pushId so selection-push
                                                // changesets have push_id recorded in changelog
                                                // 译：H-01 fix —— 透传 pushId，使“选择性推送”
                                                //   的 changeset 在 changelog 里记下其 push_id，
                                                //   从而广播屏障只阻塞这一次特定推送（而非该
                                                //   origin 的全部条目）。
    Q_UNUSED(h);  // 句柄 h 在本路径不再直接使用（会话已在上面用句柄拆除），标注以免编译告警。
    return ok;
}

// ── abort：拆除并丢弃会话，不写任何 changelog（回滚/放弃路径，以及析构兜底）────────
// 幂等：无活动会话时调用是安全空操作。设计为“随时可调”，使异常/提前 return 也能安全清理。
void SessionRecorder::abort() {
    if (session_) {
        sqlite3session_delete(session_);  // 释放原生会话资源
        session_ = nullptr;  // 复位，维持“session_==nullptr 即无活动会话”的不变量
    }
}

// ---------------------------------------------------------------------------
// private
// ---------------------------------------------------------------------------

// ── collectChangeset：把会话累积的变更导出为 changeset 字节块 ────────────────────
// 三态返回约定（见头文件 H-06 fix）：null=出错；非 null 的空=无变更；非空=有变更字节。
QByteArray SessionRecorder::collectChangeset(QString* err) {
    int nChangeset = 0;          // 输出：changeset 字节数（由 SQLite 填写）
    void* pChangeset = nullptr;  // 输出：指向 SQLite 分配的 changeset 缓冲（需我们 free）

    // sqlite3session_changeset outputs a patchset / changeset blob.
    // 译：sqlite3session_changeset 把会话期间记录的全部行级改动序列化成一个 changeset blob，
    //   缓冲区由 SQLite 分配，所有权移交给我们（成功时必须用 sqlite3_free 释放，见下）。
    int rc = sqlite3session_changeset(session_, &nChangeset, &pChangeset);
    if (rc != SQLITE_OK) {
        if (err)
            *err = QStringLiteral("sqlite3session_changeset failed: rc=%1").arg(rc);
        return QByteArray();  // null = error
                              // 译：返回 null QByteArray 表示“出错”（调用方用 isNull() 判定）。
    }

    if (nChangeset == 0 || pChangeset == nullptr) {
        // H-06 fix: distinguish "no changes captured" from "error" (both returned QByteArray()
        // before). Return a non-null empty QByteArray so the caller can use isNull() for
        // error-detection and isEmpty() for the no-changes fast-path.
        // 译：H-06 fix —— 区分“没捕获到任何变更”与“出错”（修复前两者都返回 QByteArray()，
        //   无法分辨）。这里返回“非 null 的空”QByteArray，使调用方可用 isNull() 专判出错、
        //   用 isEmpty() 走“无变更”的快速路径。注意：此处无需 sqlite3_free——空结果时
        //   pChangeset 为 nullptr（无缓冲可释放）。
        return QByteArray("");  // non-null empty = no changes, not an error
    }

    // 有变更：把 SQLite 缓冲的 nChangeset 字节深拷贝进 QByteArray（值语义，便于后续传递/落库）。
    QByteArray result(static_cast<const char*>(pChangeset), nChangeset);
    sqlite3_free(pChangeset);  // 释放 SQLite 分配的原生缓冲，避免内存泄漏
    return result;
}

}  // namespace dbridge::sync
