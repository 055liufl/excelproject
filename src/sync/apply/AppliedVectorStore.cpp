#include "AppliedVectorStore.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace dbridge::sync {

bool AppliedVectorStore::init(QSqlDatabase& db, QString* err) {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM __sync_applied_vector WHERE 0"))) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

SeqCheckResult AppliedVectorStore::check(QSqlDatabase& db, const QString& origin, qint64 epoch,
                                         qint64 seq, QString* err) {
    qint64 appliedSeq = -1;
    qint64 baselineGen = 0;
    if (!readRow(db, origin, epoch, &appliedSeq, &baselineGen)) {
        // No row yet: first ever seq must be 1
        if (seq == 1)
            return SeqCheckResult::Apply;
        if (seq <= 0)
            return SeqCheckResult::NoOp;
        if (err)
            *err = QStringLiteral("gap: no prior applied seq, got %1").arg(seq);
        return SeqCheckResult::Gap;
    }

    if (seq <= appliedSeq)
        return SeqCheckResult::NoOp;
    if (seq == appliedSeq + 1)
        return SeqCheckResult::Apply;

    if (err)
        *err = QStringLiteral("gap: applied=%1 but seq=%2").arg(appliedSeq).arg(seq);
    return SeqCheckResult::Gap;
}

bool AppliedVectorStore::advance(QSqlDatabase& db, const QString& origin, qint64 epoch, qint64 seq,
                                 QString* err) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("INSERT INTO __sync_applied_vector "
                       "(origin, stream_epoch, applied_seq, baseline_generation, updated_ms) "
                       "VALUES (?, ?, ?, 0, ?) "
                       "ON CONFLICT(origin, stream_epoch) DO UPDATE SET "
                       "  applied_seq  = excluded.applied_seq, "
                       "  updated_ms   = excluded.updated_ms "
                       "WHERE excluded.applied_seq > applied_seq"));
    q.addBindValue(origin);
    q.addBindValue(epoch);
    q.addBindValue(seq);
    q.addBindValue(nowMs);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

bool AppliedVectorStore::reset(QSqlDatabase& db, const QString& origin, qint64 epoch,
                               qint64 baselineGeneration, QString* err) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("INSERT INTO __sync_applied_vector "
                       "(origin, stream_epoch, applied_seq, baseline_generation, updated_ms) "
                       "VALUES (?, ?, 0, ?, ?) "
                       "ON CONFLICT(origin, stream_epoch) DO UPDATE SET "
                       "  applied_seq          = 0, "
                       "  baseline_generation  = excluded.baseline_generation, "
                       "  updated_ms           = excluded.updated_ms"));
    q.addBindValue(origin);
    q.addBindValue(epoch);
    q.addBindValue(baselineGeneration);
    q.addBindValue(nowMs);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

qint64 AppliedVectorStore::current(QSqlDatabase& db, const QString& origin, qint64 epoch) {
    qint64 appliedSeq = -1;
    qint64 baselineGen = 0;
    readRow(db, origin, epoch, &appliedSeq, &baselineGen);
    return appliedSeq;
}

// ---------------------------------------------------------------------------
// private
// ---------------------------------------------------------------------------

bool AppliedVectorStore::readRow(QSqlDatabase& db, const QString& origin, qint64 epoch,
                                 qint64* appliedSeq, qint64* baselineGen) {
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("SELECT applied_seq, baseline_generation "
                       "FROM __sync_applied_vector "
                       "WHERE origin = ? AND stream_epoch = ?"));
    q.addBindValue(origin);
    q.addBindValue(epoch);
    if (!q.exec() || !q.next()) {
        if (appliedSeq)
            *appliedSeq = -1;
        if (baselineGen)
            *baselineGen = 0;
        return false;
    }
    if (appliedSeq)
        *appliedSeq = q.value(0).toLongLong();
    if (baselineGen)
        *baselineGen = q.value(1).toLongLong();
    return true;
}

}  // namespace dbridge::sync
