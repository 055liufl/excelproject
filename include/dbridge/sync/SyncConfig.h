#pragma once
#include "dbridge/Export.h"
#include "dbridge/sync/SyncTypes.h"

#include <QStringList>

namespace dbridge::sync {

class DBRIDGE_EXPORT SyncConfig {
   public:
    class Builder;

    NodeRole role() const {
        return role_;
    }
    QString nodeId() const {
        return nodeId_;
    }
    QString centerNodeId() const {
        return centerNodeId_;
    }
    QStringList peerNodes() const {
        return peerNodes_;
    }
    QString sqlitePath() const {
        return sqlitePath_;
    }
    QStringList syncTables() const {
        return syncTables_;
    }
    QString outboxDir() const {
        return outboxDir_;
    }
    QString inboxDir() const {
        return inboxDir_;
    }
    QString quarantineDir() const {
        return quarantineDir_;
    }
    ConflictPolicy conflictPolicy() const {
        return conflictPolicy_;
    }
    int originRank(const QString& origin) const {
        return originRank_.value(origin, 0);
    }
    QHash<QString, int> allRanks() const {
        return originRank_;
    }
    qint64 peerLagSoftSeq() const {
        return peerLagSoftSeq_;
    }
    qint64 peerLagHardSeq() const {
        return peerLagHardSeq_;
    }
    qint64 peerLagSoftBytes() const {
        return peerLagSoftBytes_;
    }
    qint64 peerLagHardBytes() const {
        return peerLagHardBytes_;
    }
    qint64 peerLagSoftMs() const {
        return peerLagSoftMs_;
    }
    qint64 peerLagHardMs() const {
        return peerLagHardMs_;
    }
    qint64 outboxMaxBytesPerPeer() const {
        return outboxMaxBytesPerPeer_;
    }
    int outboxMaxArtifactsPerPeer() const {
        return outboxMaxArtifactsPerPeer_;
    }
    qint64 ackMaxDelayMs() const {
        return ackMaxDelayMs_;
    }
    qint64 baselineSizeWarnBytes() const {
        return baselineSizeWarnBytes_;
    }
    qint64 schemaVersion() const {
        return schemaVersion_;
    }
    int changelogRetention() const {
        return changelogRetention_;
    }
    bool verifySchemaFingerprint() const {
        return verifySchemaFingerprint_;
    }
    bool autoSyncAfterImport() const {
        return autoSyncAfterImport_;
    }
    qint64 broadcastIntervalMs() const {
        return broadcastIntervalMs_;
    }
    qint64 broadcastThreshold() const {
        return broadcastThreshold_;
    }
    qint64 maxSelectionSize() const {
        return maxSelectionSize_;
    }
    qint64 pushChunkBudgetBytes() const {
        return pushChunkBudgetBytes_;
    }
    bool consistencyCacheDurable() const {
        return consistencyCacheDurable_;
    }
    // M-1 fix: gap timeout is now configurable via SyncConfig (previously hardcoded to 30 s).
    qint64 gapTimeoutMs() const {
        return gapTimeoutMs_;
    }

    bool isValid() const {
        return valid_;
    }

   private:
    friend class Builder;
    SyncConfig() = default;

    bool valid_ = false;
    NodeRole role_ = NodeRole::Edge;
    QString nodeId_;
    QString centerNodeId_;
    QStringList peerNodes_;
    QString sqlitePath_;
    QStringList syncTables_;
    QString outboxDir_;
    QString inboxDir_;
    QString quarantineDir_;
    ConflictPolicy conflictPolicy_ = ConflictPolicy::SourceWins;
    QHash<QString, int> originRank_;
    qint64 peerLagSoftSeq_ = 10000;
    qint64 peerLagHardSeq_ = 100000;
    qint64 peerLagSoftBytes_ = 50 * 1024 * 1024;
    qint64 peerLagHardBytes_ = 500 * 1024 * 1024;
    qint64 peerLagSoftMs_ = 300000;
    qint64 peerLagHardMs_ = 3600000;
    qint64 outboxMaxBytesPerPeer_ = 1024 * 1024 * 1024;
    int outboxMaxArtifactsPerPeer_ = 10000;
    qint64 ackMaxDelayMs_ = 5000;
    qint64 baselineSizeWarnBytes_ = 100 * 1024 * 1024;
    qint64 schemaVersion_ = 1;
    int changelogRetention_ = 100000;
    bool verifySchemaFingerprint_ = true;
    bool autoSyncAfterImport_ = false;
    qint64 broadcastIntervalMs_ = 5000;
    qint64 broadcastThreshold_ = 100;
    qint64 maxSelectionSize_ = 100000;
    qint64 pushChunkBudgetBytes_ = 2 * 1024 * 1024;
    bool consistencyCacheDurable_ = true;
    qint64 gapTimeoutMs_ = 30 * 1000;  // M-1: default 30 s; configurable via Builder
};

class DBRIDGE_EXPORT SyncConfig::Builder {
   public:
    Builder() = default;
    Builder& nodeId(const QString& id) {
        cfg_.nodeId_ = id;
        return *this;
    }
    Builder& role(NodeRole r) {
        cfg_.role_ = r;
        return *this;
    }
    Builder& centerNodeId(const QString& id) {
        cfg_.centerNodeId_ = id;
        return *this;
    }
    Builder& addPeerNode(const QString& id) {
        cfg_.peerNodes_ << id;
        return *this;
    }
    Builder& database(const QString& path) {
        cfg_.sqlitePath_ = path;
        return *this;
    }
    Builder& syncTables(const QStringList& t) {
        cfg_.syncTables_ = t;
        return *this;
    }
    Builder& outboxDir(const QString& d) {
        cfg_.outboxDir_ = d;
        return *this;
    }
    Builder& inboxDir(const QString& d) {
        cfg_.inboxDir_ = d;
        return *this;
    }
    Builder& quarantineDir(const QString& d) {
        cfg_.quarantineDir_ = d;
        return *this;
    }
    Builder& conflictPolicy(ConflictPolicy p) {
        cfg_.conflictPolicy_ = p;
        return *this;
    }
    Builder& originPriority(const QString& origin, int rank) {
        cfg_.originRank_[origin] = rank;
        return *this;
    }
    Builder& peerLagSoftLimit(qint64 s) {
        cfg_.peerLagSoftSeq_ = s;
        return *this;
    }
    Builder& peerLagHardLimit(qint64 s) {
        cfg_.peerLagHardSeq_ = s;
        return *this;
    }
    Builder& peerLagSoftBytes(qint64 b) {
        cfg_.peerLagSoftBytes_ = b;
        return *this;
    }
    Builder& peerLagHardBytes(qint64 b) {
        cfg_.peerLagHardBytes_ = b;
        return *this;
    }
    Builder& peerLagSoftMs(qint64 ms) {
        cfg_.peerLagSoftMs_ = ms;
        return *this;
    }
    Builder& peerLagHardMs(qint64 ms) {
        cfg_.peerLagHardMs_ = ms;
        return *this;
    }
    Builder& outboxMaxBytesPerPeer(qint64 b) {
        cfg_.outboxMaxBytesPerPeer_ = b;
        return *this;
    }
    Builder& outboxMaxArtifactsPerPeer(int n) {
        cfg_.outboxMaxArtifactsPerPeer_ = n;
        return *this;
    }
    Builder& ackMaxDelayMs(qint64 ms) {
        cfg_.ackMaxDelayMs_ = ms;
        return *this;
    }
    Builder& baselineSizeWarnBytes(qint64 b) {
        cfg_.baselineSizeWarnBytes_ = b;
        return *this;
    }
    Builder& schemaVersion(qint64 v) {
        cfg_.schemaVersion_ = v;
        return *this;
    }
    Builder& changelogRetention(int n) {
        cfg_.changelogRetention_ = n;
        return *this;
    }
    Builder& verifySchemaFingerprint(bool on) {
        cfg_.verifySchemaFingerprint_ = on;
        return *this;
    }
    Builder& autoSyncAfterImport(bool on) {
        cfg_.autoSyncAfterImport_ = on;
        return *this;
    }
    Builder& broadcastIntervalMs(qint64 ms) {
        cfg_.broadcastIntervalMs_ = ms;
        return *this;
    }
    Builder& broadcastThreshold(qint64 n) {
        cfg_.broadcastThreshold_ = n;
        return *this;
    }
    Builder& maxSelectionSize(qint64 n) {
        cfg_.maxSelectionSize_ = n;
        return *this;
    }
    Builder& pushChunkBudgetBytes(qint64 b) {
        cfg_.pushChunkBudgetBytes_ = b;
        return *this;
    }
    Builder& consistencyCacheDurable(bool on) {
        cfg_.consistencyCacheDurable_ = on;
        return *this;
    }
    Builder& gapTimeoutMs(qint64 ms) {
        cfg_.gapTimeoutMs_ = ms;
        return *this;
    }

    SyncConfig build(QString* err = nullptr) {
        if (cfg_.nodeId_.isEmpty()) {
            if (err)
                *err = QStringLiteral("nodeId is required");
            return {};
        }
        if (cfg_.sqlitePath_.isEmpty()) {
            if (err)
                *err = QStringLiteral("database path is required");
            return {};
        }
        if (cfg_.outboxDir_.isEmpty() || cfg_.inboxDir_.isEmpty()) {
            if (err)
                *err = QStringLiteral("outboxDir and inboxDir are required");
            return {};
        }
        // M-01 fix: additional validation.
        if (cfg_.peerNodes_.contains(cfg_.nodeId_)) {
            if (err)
                *err = QStringLiteral("nodeId must not appear in peerNodes");
            return {};
        }
        if (cfg_.ackMaxDelayMs_ <= 0) {
            if (err)
                *err = QStringLiteral("ackMaxDelayMs must be positive");
            return {};
        }
        if (cfg_.broadcastIntervalMs_ <= 0) {
            if (err)
                *err = QStringLiteral("broadcastIntervalMs must be positive");
            return {};
        }
        if (cfg_.broadcastThreshold_ <= 0) {
            if (err)
                *err = QStringLiteral("broadcastThreshold must be positive");
            return {};
        }
        if (cfg_.maxSelectionSize_ <= 0) {
            if (err)
                *err = QStringLiteral("maxSelectionSize must be positive");
            return {};
        }
        if (cfg_.pushChunkBudgetBytes_ <= 0) {
            if (err)
                *err = QStringLiteral("pushChunkBudgetBytes must be positive");
            return {};
        }
        if (cfg_.peerLagSoftMs_ <= 0) {
            if (err)
                *err = QStringLiteral("peerLagSoftMs must be positive");
            return {};
        }
        if (cfg_.schemaVersion_ < 1) {
            if (err)
                *err = QStringLiteral("schemaVersion must be >= 1");
            return {};
        }
        cfg_.valid_ = true;
        return cfg_;
    }

   private:
    SyncConfig cfg_;
};

}  // namespace dbridge::sync
