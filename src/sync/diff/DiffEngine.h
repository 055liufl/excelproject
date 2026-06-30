#pragma once
#include "dbridge/sync/IComparisonSession.h"

#include <QHash>
#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QVariantMap>

#include "../schema/TableStateStore.h"

// ============================================================================
// DiffEngine.h — 两级差异比对引擎（表级 + 行级）的接口
// ============================================================================
//
// 【职责】DiffEngine 是 ComparisonSession 内部用来“算差异”的纯计算组件，提供两层
//   比对（与 IComparisonSession 文件头的“两级比对”一一对应）：
//     ① tableDiffs() —— 表级：用三元组（schemaFp 表结构指纹 + checksum 内容校验和
//        + rowCount 行数）做快速判等，只有三者全等才判 Identical，否则 Different；
//        同时根据“仅本地/仅对端有此表”给出 OnlyLocal/OnlyRemote。
//     ② rowDiffs() —— 行级：以主键(PK)为 key，把本地行集合与对端行集合做集合比较，
//        产出每行的 Added/Deleted/Modified/Same 以及逐列 CellDiff。
//
// 【数据从哪来】
//   · 本地侧（local）：直接从本地库 rconn 上 SELECT（fetchLocalRows）。
//   · 对端侧（remote）：表级来自调用方传入的 RemoteMeta；行级来自传入的 remoteRows。
//   · 本地表级状态：从 TableStateStore 读 __sync_table_state（按 streamEpoch 定位）。
//
// 【无状态？】DiffEngine 仅持有一个“表→主键列名”的缓存 pkColCache_，可被同一会话
//   反复调用。它不持有连接、不写库——所有写回由 StagingBuffer/UpsertExecutor 负责。
// ============================================================================

namespace dbridge::sync {

class DiffEngine {
   public:
    // 对端某表的“表级元数据”三元组：仅用于表级快速判等，不含具体行内容。
    struct RemoteMeta {
        QString schemaFp;  // 表结构指纹（列/类型定义的哈希）
        QString checksum;  // 内容校验和：十六进制表示的 quint64 模和（hex quint64 modular sum）
        qint64 rowCount = 0;  // 行数
    };

    // 表级差异：把本地 TableStateStore 中记录的状态 与 对端 RemoteMeta 逐表比较。
    //   参数：rconn —— 本地只读连接；tables —— 待比对的表名集合；
    //         streamEpoch —— 用于在 __sync_table_state 里定位本地状态的“流纪元”；
    //         localTs —— 本地表状态存储（读三元组）；remote —— 表名→对端元数据。
    //   返回：每张表一条 TableDiff（状态 + 增删改计数估算）。
    //   复杂度：O(tables) 次状态读取。
    QList<TableDiff> tableDiffs(QSqlDatabase& rconn, const QStringList& tables, qint64 streamEpoch,
                                TableStateStore& localTs, const QHash<QString, RemoteMeta>& remote);

    // 行级差异：把本地表中的行 与 对端行集合 remoteRows 做集合比较。
    //   参数：rconn —— 本地只读连接；table —— 表名；remoteRows —— 对端该表的行集合；
    //         offset/limit —— 分页窗口（两端会被切到同一窗口后再比，limit<0 取到末尾）。
    //   返回：窗口内每个 PK 一条 RowDiff（Added/Deleted/Modified/Same + 逐列 CellDiff）。
    QList<RowDiff> rowDiffs(QSqlDatabase& rconn, const QString& table,
                            const QList<QVariantMap>& remoteRows, int offset, int limit);

   private:
    // 从本地表读取行（每行一个 QVariantMap：列名→值）。会 ORDER BY 主键保证分页稳定。
    QList<QVariantMap> fetchLocalRows(QSqlDatabase& rconn, const QString& table, int offset,
                                      int limit);
    // 取某表的主键列名（经 PRAGMA table_info；结果进 pkColCache_ 缓存）。
    QString getPkColumn(QSqlDatabase& rconn, const QString& table);
    // 逐列比较两行，产出 CellDiff 列表（取两行列名的并集，逐列填 local/remote 值与 changed）。
    QList<CellDiff> compareRows(const QVariantMap& local, const QVariantMap& remote);
    // 表名 → 主键列名 的缓存，避免每次都查 PRAGMA。
    QHash<QString, QString> pkColCache_;
};

}  // namespace dbridge::sync
