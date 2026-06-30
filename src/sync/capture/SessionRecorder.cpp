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

bool SessionRecorder::sealInto(sqlite3* h, ChangelogStore& store, QSqlDatabase& db, WriteTxn& txn,
                               const QString& origin, qint64 epoch, qint64 schemaVer,
                               const QString& schemaFp, qint64 parentSeq, qint64 originSeq,
                               qint64* outLocalSeq, QString* err, const QString& pushId,
                               QByteArray* outChangeset) {
    if (!session_) {
        if (err)
            *err = QStringLiteral("SessionRecorder not active");
        return false;
    }
    if (!txn.isActive()) {
        if (err)
            *err = QStringLiteral("WriteTxn not active during sealInto");
        return false;
    }

    QByteArray changeset = collectChangeset(err);
    if (changeset.isNull()) {
        // collectChangeset sets *err
        abort();
        return false;
    }

    // Detach session before writing to avoid re-recording the changelog INSERT.
    sqlite3session_delete(session_);
    session_ = nullptr;

    if (changeset.isEmpty()) {
        // No changes captured; still report success with seq=0.
        if (outLocalSeq)
            *outLocalSeq = 0;
        return true;
    }

    // M-01 fix: expose changeset bytes to caller for incremental table_state updates.
    if (outChangeset)
        *outChangeset = changeset;

    bool ok = store.append(db, QStringLiteral("changeset"), origin,
                           /*sourcePeer=*/QString(), originSeq, parentSeq, epoch, schemaVer,
                           schemaFp, changeset,
                           /*authoritative=*/true, outLocalSeq, err,
                           /*pushId=*/pushId);  // H-01 fix: forward pushId so selection-push
                                                // changesets have push_id recorded in changelog
    Q_UNUSED(h);
    return ok;
}

void SessionRecorder::abort() {
    if (session_) {
        sqlite3session_delete(session_);
        session_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// private
// ---------------------------------------------------------------------------

QByteArray SessionRecorder::collectChangeset(QString* err) {
    int nChangeset = 0;
    void* pChangeset = nullptr;

    // sqlite3session_changeset outputs a patchset / changeset blob.
    int rc = sqlite3session_changeset(session_, &nChangeset, &pChangeset);
    if (rc != SQLITE_OK) {
        if (err)
            *err = QStringLiteral("sqlite3session_changeset failed: rc=%1").arg(rc);
        return QByteArray();  // null = error
    }

    if (nChangeset == 0 || pChangeset == nullptr) {
        // H-06 fix: distinguish "no changes captured" from "error" (both returned QByteArray()
        // before). Return a non-null empty QByteArray so the caller can use isNull() for
        // error-detection and isEmpty() for the no-changes fast-path.
        return QByteArray("");  // non-null empty = no changes, not an error
    }

    QByteArray result(static_cast<const char*>(pChangeset), nChangeset);
    sqlite3_free(pChangeset);
    return result;
}

}  // namespace dbridge::sync
