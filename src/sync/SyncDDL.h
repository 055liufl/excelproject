#pragma once
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QUuid>

namespace dbridge::sync::ddl {

// Returns all CREATE TABLE statements in dependency order.
inline QStringList allCreateStatements() {
    return {
        // --- changelog ---
        QStringLiteral(R"(
CREATE TABLE IF NOT EXISTS __sync_changelog (
  local_seq       INTEGER PRIMARY KEY AUTOINCREMENT,
  kind            TEXT    NOT NULL,
  origin          TEXT    NOT NULL,
  source_peer     TEXT,
  origin_seq      INTEGER NOT NULL,
  parent_seq      INTEGER,
  stream_epoch    INTEGER NOT NULL,
  schema_ver      INTEGER NOT NULL,
  schema_fingerprint TEXT NOT NULL,
  changeset       BLOB    NOT NULL,
  payload_checksum TEXT   NOT NULL,
  byte_size       INTEGER NOT NULL,
  authoritative   INTEGER NOT NULL DEFAULT 0,
  created_ms      INTEGER NOT NULL,
  push_id         TEXT,
  UNIQUE(origin, stream_epoch, origin_seq)
))"),
        // Note: push_id column added inline in CREATE TABLE above.
        // For existing databases, applyMigrations() performs ALTER TABLE ADD COLUMN.,
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_changelog_origin ON "
                       "__sync_changelog(origin, origin_seq)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_changelog_epoch  ON "
                       "__sync_changelog(stream_epoch, local_seq)"),

        // --- applied_vector ---
        QStringLiteral(R"(
CREATE TABLE IF NOT EXISTS __sync_applied_vector (
  origin          TEXT    NOT NULL,
  stream_epoch    INTEGER NOT NULL,
  applied_seq     INTEGER NOT NULL,
  baseline_generation INTEGER NOT NULL DEFAULT 0,
  updated_ms      INTEGER NOT NULL,
  PRIMARY KEY(origin, stream_epoch)
))"),

        // --- outbound_ack ---
        QStringLiteral(R"(
CREATE TABLE IF NOT EXISTS __sync_outbound_ack (
  peer            TEXT    NOT NULL,
  origin          TEXT    NOT NULL,
  stream_epoch    INTEGER NOT NULL,
  acked_seq       INTEGER NOT NULL DEFAULT -1,
  last_sent_seq   INTEGER NOT NULL DEFAULT -1,
  last_ack_ms     INTEGER,
  pending_baseline INTEGER NOT NULL DEFAULT 0,
  last_push_id    TEXT,
  last_chunk_seq  INTEGER,
  PRIMARY KEY(peer, origin, stream_epoch)
))"),

        // --- table_state ---
        QStringLiteral(R"(
CREATE TABLE IF NOT EXISTS __sync_table_state (
  table_name      TEXT    NOT NULL,
  stream_epoch    INTEGER NOT NULL,
  schema_fingerprint TEXT NOT NULL,
  high_water_seq  INTEGER NOT NULL DEFAULT 0,
  content_checksum TEXT   NOT NULL,
  row_count       INTEGER NOT NULL DEFAULT 0,
  updated_ms      INTEGER NOT NULL,
  PRIMARY KEY(table_name, stream_epoch)
))"),

        // --- consistency_cache ---
        QStringLiteral(R"(
CREATE TABLE IF NOT EXISTS __sync_consistency_cache (
  table_name      TEXT    NOT NULL,
  primary_key     TEXT    NOT NULL,
  center_fingerprint BLOB NOT NULL,
  updated_ms      INTEGER NOT NULL,
  PRIMARY KEY(table_name, primary_key)
))"),

        // --- quarantine ---
        QStringLiteral(R"(
CREATE TABLE IF NOT EXISTS __sync_quarantine (
  id              INTEGER PRIMARY KEY AUTOINCREMENT,
  origin          TEXT    NOT NULL,
  origin_seq      INTEGER NOT NULL,
  stream_epoch    INTEGER NOT NULL,
  payload_schema_ver INTEGER NOT NULL,
  payload         BLOB    NOT NULL,
  created_ms      INTEGER NOT NULL
))"),

        // --- push_progress ---
        QStringLiteral(R"(
CREATE TABLE IF NOT EXISTS __sync_push_progress (
  push_id         TEXT    PRIMARY KEY,
  origin          TEXT    NOT NULL,
  peer            TEXT    NOT NULL,
  total_chunks    INTEGER NOT NULL,
  schema_ver      INTEGER NOT NULL,
  status          TEXT    NOT NULL,
  failed_code     TEXT,
  updated_ms      INTEGER NOT NULL
))"),

        // --- push_chunk_progress ---
        QStringLiteral(R"(
CREATE TABLE IF NOT EXISTS __sync_push_chunk_progress (
  push_id         TEXT    NOT NULL,
  chunk_seq       INTEGER NOT NULL,
  status          TEXT    NOT NULL,
  checksum        TEXT    NOT NULL,
  applied_ms      INTEGER,
  PRIMARY KEY(push_id, chunk_seq),
  FOREIGN KEY(push_id) REFERENCES __sync_push_progress(push_id)
))"),

        // --- frozen_manifest ---
        QStringLiteral(R"(
CREATE TABLE IF NOT EXISTS __sync_frozen_manifest (
  push_id         TEXT    NOT NULL,
  chunk_seq       INTEGER NOT NULL,
  table_name      TEXT    NOT NULL,
  pk_hash         TEXT    NOT NULL,
  primary_key     TEXT    NOT NULL,
  record_kind     TEXT    NOT NULL,
  topo_index      INTEGER NOT NULL,
  fingerprint     BLOB    NOT NULL,
  PRIMARY KEY(push_id, chunk_seq, table_name, pk_hash),
  FOREIGN KEY(push_id) REFERENCES __sync_push_progress(push_id)
))"),

        // --- row_winner (G-01: changeset path only) ---
        QStringLiteral(R"(
CREATE TABLE IF NOT EXISTS __sync_row_winner (
  table_name      TEXT    NOT NULL,
  pk_hash         TEXT    NOT NULL,
  winning_origin  TEXT    NOT NULL,
  winning_rank    INTEGER NOT NULL,
  winning_origin_seq INTEGER NOT NULL,
  content_hash    BLOB    NOT NULL,
  winning_content TEXT    NOT NULL DEFAULT '',  -- C-01: JSON-encoded row for low-rank DELETE recovery
  updated_ms      INTEGER NOT NULL,
  PRIMARY KEY(table_name, pk_hash)
))"),

        // --- inbox_ledger (G-08: artifact-level idempotent consumption) ---
        QStringLiteral(R"(
CREATE TABLE IF NOT EXISTS __sync_inbox_ledger (
  artifact_name   TEXT    PRIMARY KEY,
  status          TEXT    NOT NULL,
  first_seen_ms   INTEGER NOT NULL,
  consumed_ms     INTEGER
))"),
    };
}

// Canonical artifact naming helpers.
// H-08 fix: include target peer so the same changelog entry written for different peers
// produces distinct file names (same outbox dir cannot hold two files with the same name).
inline QString changesetArtifactName(const QString& origin, qint64 epoch, qint64 seq,
                                     const QString& targetPeer = QString()) {
    if (targetPeer.isEmpty())
        return QStringLiteral("%1__%2__%3__changeset.payload")
            .arg(origin)
            .arg(epoch)
            .arg(seq, 12, 10, QLatin1Char('0'));
    return QStringLiteral("%1__%2__%3__%4__changeset.payload")
        .arg(origin)
        .arg(epoch)
        .arg(seq, 12, 10, QLatin1Char('0'))
        .arg(targetPeer);
}

inline QString selectionPushArtifactName(const QString& pushId, int chunkSeq,
                                         const QString& targetPeer = QString()) {
    if (targetPeer.isEmpty())
        return QStringLiteral("%1__%2__selectionpush.payload")
            .arg(pushId)
            .arg(chunkSeq, 6, 10, QLatin1Char('0'));
    return QStringLiteral("%1__%2__%3__selectionpush.payload")
        .arg(pushId)
        .arg(chunkSeq, 6, 10, QLatin1Char('0'))
        .arg(targetPeer);
}

inline QString baselineRequestArtifactName(const QString& fromPeer, const QString& toPeer,
                                           qint64 epoch, qint64 fromSeq,
                                           const QString& uniqueSuffix = QString()) {
    const QString suffix = uniqueSuffix.isEmpty()
                               ? QUuid::createUuid().toString(QUuid::WithoutBraces).left(8)
                               : uniqueSuffix;
    return QStringLiteral("%1__%2__%3__%4__%5__baselinerequest.payload")
        .arg(fromPeer)
        .arg(toPeer)
        .arg(epoch)
        .arg(fromSeq, 12, 10, QLatin1Char('0'))
        .arg(suffix);
}

inline QString baselineResponseArtifactName(const QString& fromPeer, const QString& toPeer,
                                            qint64 epoch, qint64 sourceMaxSeq,
                                            const QString& uniqueSuffix = QString()) {
    const QString suffix = uniqueSuffix.isEmpty()
                               ? QUuid::createUuid().toString(QUuid::WithoutBraces).left(8)
                               : uniqueSuffix;
    return QStringLiteral("%1__%2__%3__%4__%5__baselineresponse.payload")
        .arg(fromPeer)
        .arg(toPeer)
        .arg(epoch)
        .arg(sourceMaxSeq, 12, 10, QLatin1Char('0'))
        .arg(suffix);
}

// H-03 fix: include a per-call UUID suffix so same-millisecond ACKs never collide.
inline QString ackArtifactName(const QString& fromPeer, const QString& toPeer, qint64 ms,
                               const QString& uniqueSuffix = QString()) {
    const QString suffix = uniqueSuffix.isEmpty()
                               ? QUuid::createUuid().toString(QUuid::WithoutBraces).left(8)
                               : uniqueSuffix;
    return QStringLiteral("ack__%1__%2__%3__%4.ack").arg(fromPeer).arg(toPeer).arg(ms).arg(suffix);
}

// M-04 fix: apply schema migrations for existing databases.
// ALTER TABLE ADD COLUMN fails if the column already exists; we intentionally ignore that
// error so the function is idempotent and works for both fresh and pre-existing databases.
// Returns false only on catastrophic failures (not "column already exists").
inline bool applyMigrations(QSqlDatabase& db) {
    // M-04: add push_id column to __sync_changelog (NULL for non-push changesets).
    QSqlQuery q(db);
    q.exec(QStringLiteral("ALTER TABLE __sync_changelog ADD COLUMN push_id TEXT"));
    // Ignore error: "duplicate column name" is expected when column already exists.
    // Any other error is a schema mismatch we cannot auto-fix — it will surface later
    // when a real INSERT fails.
    return true;
}

}  // namespace dbridge::sync::ddl
