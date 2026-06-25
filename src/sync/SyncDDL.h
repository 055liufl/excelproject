#pragma once
#include <QString>
#include <QStringList>

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
  UNIQUE(origin, stream_epoch, origin_seq)
))"),
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

// Canonical artifact naming helpers
inline QString changesetArtifactName(const QString& origin, qint64 epoch, qint64 seq) {
    return QStringLiteral("%1__%2__%3__changeset.payload")
        .arg(origin)
        .arg(epoch)
        .arg(seq, 12, 10, QLatin1Char('0'));
}

inline QString selectionPushArtifactName(const QString& pushId, int chunkSeq) {
    return QStringLiteral("%1__%2__selectionpush.payload")
        .arg(pushId)
        .arg(chunkSeq, 6, 10, QLatin1Char('0'));
}

inline QString ackArtifactName(const QString& fromPeer, const QString& toPeer, qint64 ms) {
    return QStringLiteral("ack__%1__%2__%3.ack").arg(fromPeer).arg(toPeer).arg(ms);
}

}  // namespace dbridge::sync::ddl
