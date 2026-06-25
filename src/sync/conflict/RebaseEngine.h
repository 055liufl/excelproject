#pragma once
#include <QByteArray>
#include <QString>

namespace dbridge::sync {

// Rebases a changeset onto an authoritative rebase buffer using sqlite3rebaser_*.
// The rebaseBuffer comes from ChangesetApplier::ApplyOutcome.rebaseBuffer
// (produced by sqlite3changeset_apply_v2 with SQLITE_CHANGESETAPPLY_NOSAVEPOINT).
class RebaseEngine {
   public:
    // Rebase `changeset` against `rebaseBuffer`.
    // On success, writes rebased data to *rebased and returns true.
    // On failure, sets *err and returns false.
    bool rebase(const QByteArray& rebaseBuffer, const QByteArray& changeset, QByteArray* rebased,
                QString* err);
};

}  // namespace dbridge::sync
