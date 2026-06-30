#include "InboxWatcher.h"

#include <QDir>       // 目录枚举 entryList
#include <QFileInfo>  // 校验主文件确实存在
#include <QStringList>

// ============================================================================
// InboxWatcher.cpp — inbox 扫描的实现
// 「只认 .ready 哨兵 / 同步 scan / 台账去重」的动机详见 InboxWatcher.h 文件头注释。
// ============================================================================

namespace dbridge::sync {

InboxWatcher::InboxWatcher(const QString& inboxDir, QSqlDatabase& db, InboxLedger& ledger,
                           QObject* parent)
    : QObject(parent), dir_(inboxDir), db_(db), ledger_(ledger) {
    // 仅保存配置；不在构造时做任何扫描或 IO。
}

// ---------------------------------------------------------------------------
// scan() — called directly on the worker thread (no event loop required).
// Returns newly discovered artifact paths (full paths, not just names).
// 译：scan() —— 在 worker 线程上直接调用（无需事件循环）。返回新发现的 artifact 完整路径
//   （是完整路径，不止文件名）。
// ---------------------------------------------------------------------------

QStringList InboxWatcher::scan(QSqlDatabase& db) {
    QStringList newArtifacts;

    QDir d(dir_);
    if (!d.exists())
        return newArtifacts;  // inbox 目录尚不存在：返回空，下轮再扫

    // Find all .ready markers, sorted oldest-first for FIFO processing.
    // M-08 fix: QDir::Reversed inverts the default QDir::Time (newest-first) order to
    // produce the oldest-first sequence the comment and the gap-detection code assume.
    // 译：列出所有 .ready 哨兵，按「最旧在前」排序以实现 FIFO 处理。
    //   M-08 fix：QDir::Time 默认是「最新在前」，叠加 QDir::Reversed 翻转成「最旧在前」，
    //   才符合本处注释与「空洞检测」逻辑对“按到达先后处理”的假设。顺序很关键：
    //   乱序处理可能让依赖前序变更的 artifact 先被尝试而失败。
    const QStringList readyFiles = d.entryList(QStringList{QStringLiteral("*.ready")}, QDir::Files,
                                               QDir::Time | QDir::Reversed);

    for (const QString& readyName : readyFiles) {
        // Derive artifact name: strip the trailing ".ready" (6 chars)
        // 译：由哨兵名推出主文件名——去掉结尾的 ".ready"（恰好 6 个字符）。
        //   例如 "A__1__changeset__...uuid.payload.ready" → "...uuid.payload"。
        QString artifactName = readyName.left(readyName.length() - 6);

        // Check ledger: skip if already consumed or corrupt.
        // 译：查台账——已 consumed（处理过）或 corrupt（损坏）的直接跳过，实现幂等去重，
        //   避免同一 artifact 被重复处理或反复尝试损坏文件。
        LedgerStatus st = ledger_.status(db, artifactName);
        if (st == LedgerStatus::Consumed || st == LedgerStatus::Corrupt)
            continue;

        // Mark as seen (idempotent: no-op if already known).
        // M-5 fix: if markSeen fails, skip this artifact — processing it without a ledger entry
        // could cause it to be re-processed indefinitely on the next scan.
        // 译：登记为 seen（幂等：已知则无操作）。
        //   M-5 fix：若 markSeen 写台账失败，则跳过此 artifact——若在「没有台账记录」的情况下
        //   贸然把它交给上层处理，一旦后续无法把它推进到 consumed，下一轮扫描会把它再发现一次，
        //   造成「无限重复处理」。故台账写失败时本轮直接跳过，宁可下轮重来。
        //   仅当状态不是 Seen 时才尝试写（已是 Seen 说明先前登记过，无需重复写）。
        if (st != LedgerStatus::Seen) {
            QString markErr;
            if (!ledger_.markSeen(db, artifactName, &markErr))
                continue;  // ledger write failed; skip this scan cycle（译：台账写失败，本轮跳过）
        }

        // 最后再确认主文件确实存在才纳入结果：理论上 .ready 在主文件就位后才创建，
        // 但极端情况下（主文件被外部清理/搬走、只剩孤儿 .ready）需此防御，避免返回不存在的路径。
        const QString artifactPath = d.filePath(artifactName);
        if (QFileInfo::exists(artifactPath))
            newArtifacts.append(artifactPath);
    }

    return newArtifacts;
}

}  // namespace dbridge::sync
