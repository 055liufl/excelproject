#pragma once

// ── 场景2 后端逻辑（不依赖任何 QWidget） ─────────────────────────────────────
//
// Scenario2Model 封装了「子节点B（本地/local） ⇄ 中心节点A（远端/remote）」差异
// 比对与列级同步的全部业务逻辑，供 Scenario2Widget（GUI）与 --selftest（headless）
// 复用同一套代码路径：
//
//   ① setup()          —— 清理并 seed 两个有意制造差异的 SQLite 库（center_A / child_B），
//                          打开 B 的 DataBridge、初始化 B 的 ISyncEngine（建立 SyncContext）。
//   ② rebuildSession() —— 从 center_A 读取整库快照，createComparisonSession(B) + initialize()，
//                          触发 DiffEngine 计算差异。
//   ③ tableStatuses()  —— 逐表给出 相同(绿)/不同(红) 及 +/-/~ 计数（直接由行级 diff 推导，
//                          始终准确，不依赖 __sync_table_state 的 checksum 快路径）。
//   ④ stageCellValue() / acceptRemoteRow() / acceptLocalRow() / unstageRow()
//                      —— 把「采用中心A」的决策（精确到列或整行）暂存进比对会话的
//                          StagingBuffer（保存在内存中，未落库）。
//   ⑤ save()           —— 将内存中的合并决策经 SyncWorker 写回 B 的数据库（A→B 同步）。
//   ⑥ discard()        —— 放弃所有暂存，恢复原状。
//
// 真实使用 dbridge::sync::IComparisonSession（内部即 DiffEngine + StagingBuffer +
// UpsertExecutor），不做任何简化。

#include "dbridge/DataBridge.h"
#include "dbridge/sync/IComparisonSession.h"
#include "dbridge/sync/ISyncEngine.h"
#include "dbridge/sync/SyncConfig.h"

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <memory>
#include <optional>

class Scenario2Model {
   public:
    // 单表对比状态摘要（驱动「表对比清单」中的 红/绿 显示）。
    struct TableStatus {
        QString table;
        bool identical = true;  // true=相同(绿)，false=不同(红)
        int added = 0;          // 仅中心A 存在（B 需新增）
        int deleted = 0;        // 仅子节点B 存在（A 没有）
        int modified = 0;       // 两端都有但字段不同
    };

    explicit Scenario2Model(const QString& ws);
    ~Scenario2Model();

    Scenario2Model(const Scenario2Model&) = delete;
    Scenario2Model& operator=(const Scenario2Model&) = delete;

    // 清理 + seed 两库 + 打开 B 的 bridge/engine + 建首个比对会话。
    bool setup(QString* err);

    // 重新 seed 两库（"重置演示数据"按钮）并重建会话。
    bool reseed(QString* err);

    // 重新构建比对会话（从 center_A 读快照并 initialize）。
    bool rebuildSession(QString* err);

    // 参与比对的表清单。
    QStringList tables() const {
        return tables_;
    }

    // 逐表状态（由行级 diff 推导，始终准确）。
    QList<TableStatus> tableStatuses() const;

    // 指定表的行级差异（含逐列 CellDiff，localValue=B 值，remoteValue=A 值）。
    QList<dbridge::sync::RowDiff> rowDiffs(const QString& table) const;

    // 该表的列顺序（用于双栏展示；取自 B 库 schema，A 与之同构）。
    QStringList columns(const QString& table) const;

    // 该表主键列名。
    QString pkColumn(const QString& table) const;

    // ── 合并决策（暂存到内存，不落库）──────────────────────────────────────
    // 采用中心A 的整行（acceptRemote）。
    bool acceptRemoteRow(const QString& table, const QString& pk);
    // 保留本地B（撤销该行的所有暂存）。
    bool acceptLocalRow(const QString& table, const QString& pk);
    // 精确到列：采用中心A 的某一列值（可在同一行多次累积）。
    bool stageCellValue(const QString& table, const QString& pk, const QString& column,
                        const QVariant& value);
    // 撤销该行的暂存决策。
    bool unstageRow(const QString& table, const QString& pk);

    // ── 暂存状态查询（驱动界面高亮）──────────────────────────────────────
    bool isRowStaged(const QString& table, const QString& pk) const;
    bool isCellStaged(const QString& table, const QString& pk, const QString& column) const;
    int pendingCount() const {
        return stagedRows_.size();
    }

    // 保存（A→B 写回）。成功后清空暂存并重建会话。
    bool save(QString* err);
    // 放弃暂存，重建会话。
    void discard();

    // 读取某库某表的全部行（B 用于左栏展示，A 用于右栏/远端快照）。
    QList<QVariantMap> centerRows(const QString& table) const;  // 中心A（远端）
    QList<QVariantMap> childRows(const QString& table) const;   // 子节点B（本地）

    QString centerDbPath() const {
        return centerDb_;
    }
    QString childDbPath() const {
        return childDb_;
    }

    // headless 自检：制造一次列级 + 整行的「采用A」并 save，校验 B 库被正确写回。
    bool runHeadlessSelfTest(QString* err);

   private:
    // 行键 / 单元键的内部编码（QSet 用）。
    static QString rowKey(const QString& table, const QString& pk);
    static QString cellKey(const QString& table, const QString& pk, const QString& column);

    // 从 center_A 读全库，构造 RemoteTableSnapshot 列表。
    QList<dbridge::sync::RemoteTableSnapshot> buildRemoteSnapshots(QString* err) const;

    // 在指定库路径上建表 + 灌入 seed 数据（direct SQL）。
    bool seedDatabases(QString* err);

    // 读取任意库某表所有行（独立连接）。
    static QList<QVariantMap> readRows(const QString& dbPath, const QString& table);
    // 读取任意库某表列顺序。
    static QStringList readColumns(const QString& dbPath, const QString& table);
    // 读取任意库某表主键列。
    static QString readPkColumn(const QString& dbPath, const QString& table);

    QString ws_;
    QString centerDb_;  // 中心节点A 数据库（远端，比对的"对端"）
    QString childDb_;   // 子节点B 数据库（本地，当前节点，写回目标）
    QStringList tables_;

    dbridge::DataBridge childBridge_;
    std::unique_ptr<dbridge::sync::ISyncEngine> childEngine_;
    std::unique_ptr<dbridge::sync::IComparisonSession> session_;
    // SyncConfig 的默认构造为私有，且本类在构造时尚无配置 → 用 optional 延迟构造。
    std::optional<dbridge::sync::SyncConfig> cfg_;
    bool engineReady_ = false;

    // 暂存追踪（用于界面高亮 + pendingCount；比对会话本身不暴露"已暂存了哪些"）。
    QSet<QString> stagedRows_;   // "tablepk"
    QSet<QString> stagedCells_;  // "tablepkcolumn"
};
