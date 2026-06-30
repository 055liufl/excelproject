#include "DiffEngine.h"

#include <QSet>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QVariant>

#include "sql/SqlBuilder.h"
#include <algorithm>

// ============================================================================
// DiffEngine.cpp — 两级差异比对的实现
// ============================================================================
// 本文件实现 DiffEngine.h 声明的两层比对算法。阅读顺序建议：
//   tableDiffs()（表级三元组判等）→ rowDiffs()（行级集合比较）→ 三个私有辅助。
// 关键不直观处（三元组判等、增删改计数估算、Added/Deleted/Modified 的集合判定、
// CellDiff 生成、分页稳定性）均逐行注释。
// ============================================================================

namespace dbridge::sync {

// ── 表级比对：逐表比较“本地状态三元组” vs “对端元数据三元组” ─────────────────
//   做什么：对 tables 中每张表给出一条 TableDiff（Identical/Different/OnlyLocal/
//           OnlyRemote 及增删改的“估算”计数）。
//   为什么用三元组：表级判等不读全表内容，仅靠 (内容校验和 + 表结构指纹 + 行数)
//           即可快速判定“是否完全一致”，把昂贵的逐行比对留到 rowDiffs()。
//   参数/返回/复杂度见 DiffEngine.h。
//   错误模式：本地状态读取失败时，按“未找到”保守处理（见 J-12 注释）。
QList<TableDiff> DiffEngine::tableDiffs(QSqlDatabase& rconn, const QStringList& tables,
                                        qint64 streamEpoch, TableStateStore& localTs,
                                        const QHash<QString, RemoteMeta>& remote) {
    QList<TableDiff> result;
    result.reserve(tables.size());  // 预分配：每张表恰好产出一条结果

    for (const QString& table : tables) {
        TableDiff td;
        td.table = table;

        // —— 读“本地侧”三元组：表结构指纹 / 内容校验和 / 行数 ——
        QString localFp, localChecksum;
        qint64 localRowCount = 0;
        bool localFound = false;  // 本地是否存在该表的已同步状态记录
        QString err;
        // J-12（J-12 修复）：readState() 通过 bool* found 区分两种“无值”：
        //   · “未找到”——这张表从未被同步过（合法状态，found=false）；
        //   · “查询出错”——真正的读失败。
        // 出错时我们把状态当作“未找到”来保守处理（宁可判 OnlyRemote/Identical，
        // 也不要因一次读失败而误判成 Different 触发不必要的行级比对/合并）。
        localTs.readState(rconn, table, streamEpoch, &localFp, &localChecksum, &localRowCount,
                          &localFound, &err);

        bool remoteFound = remote.contains(table);  // 对端快照里是否有这张表

        // —— 四象限判定：{本地有/无} × {对端有/无} ——
        if (!remoteFound && localFound) {
            // 仅本地有此表 → 对端缺失这张表
            td.status = TableDiffStatus::OnlyLocal;
        } else if (remoteFound && !localFound) {
            // 仅对端有此表 → 对端整张表都算“新增”行
            td.status = TableDiffStatus::OnlyRemote;
            td.addedRows = static_cast<int>(remote[table].rowCount);
        } else if (!remoteFound && !localFound) {
            // 两端都没有（或本地读失败被当作未找到）→ 无差异
            td.status = TableDiffStatus::Identical;
        } else {
            // 两端都有此表：比三元组决定是否真的一致
            const RemoteMeta& rm = remote[table];
            // I-13 fix（I-13 修复）：判 Identical 必须三项同时相等——
            //   内容校验和 == 且 表结构指纹 == 且 行数 ==。
            // 任意一项不同都判 Different（例如行数相同但校验和不同，说明有“原地修改”）。
            if (localChecksum == rm.checksum && localFp == rm.schemaFp &&
                localRowCount == rm.rowCount) {
                td.status = TableDiffStatus::Identical;
            } else {
                td.status = TableDiffStatus::Different;
                // 增删改计数：表级阶段拿不到逐行真值，这里只做“估算”供 UI 概览
                // （精确的逐行结论由 rowDiffs() 给出）。
                qint64 rowDiff = rm.rowCount - localRowCount;  // 对端比本地多多少行
                if (rowDiff > 0) {
                    // 对端行更多：差额计为“新增”，并粗略估计约 1/4 本地行被改
                    td.addedRows = static_cast<int>(rowDiff);
                    td.modifiedRows = static_cast<int>(localRowCount / 4);
                } else if (rowDiff < 0) {
                    // 对端行更少：差额计为“删除”，并粗略估计约 1/4 对端行被改
                    td.deletedRows = static_cast<int>(-rowDiff);
                    td.modifiedRows = static_cast<int>(rm.rowCount / 4);
                } else {
                    // 行数相同但校验和不同——没有增删，全部差异只能是“修改”。
                    td.modifiedRows = static_cast<int>(localRowCount);
                }
            }
        }

        result.append(td);
    }

    return result;
}

// ── 行级比对：以主键为 key，把本地行集合与对端行集合做集合比较 ──────────────────
//   做什么：在同一分页窗口 [offset, offset+limit) 内，把本地行与对端行按 PK 配对，
//           判定每个 PK 是 Added（仅对端）/ Deleted（仅本地）/ Modified（两端不同）/
//           Same（两端相同），并对每行产出逐列 CellDiff。
//   为什么用 PK 做 key：行没有稳定身份就无法谈“同一行的两端差异”；主键就是这个身份。
//   复杂度：O(本地行数 + 对端行数)——两遍建哈希表 + 三遍线性扫描。
//   错误模式：表无主键时（pkCol 为空）无法配对，localMap/remoteMap 均为空 → 返回空表。
QList<RowDiff> DiffEngine::rowDiffs(QSqlDatabase& rconn, const QString& table,
                                    const QList<QVariantMap>& remoteRows, int offset, int limit) {
    // 本地侧：从本地库取这一页的行（内部已 ORDER BY 主键，保证分页稳定）。
    QList<QVariantMap> localRows = fetchLocalRows(rconn, table, offset, limit);
    QString pkCol = getPkColumn(rconn, table);  // 该表主键列名（两端共用）

    // 把对端行集合切到与本地相同的窗口，保证“同页对同页”地比较。
    int remoteStart = offset;
    int remoteEnd =
        (limit < 0) ? remoteRows.size() : std::min(remoteStart + limit, (int)remoteRows.size());
    QList<QVariantMap> remoteSlice = remoteRows.mid(remoteStart, remoteEnd - remoteStart);

    // —— 建立两张以“主键字符串”为 key 的查找表，便于 O(1) 判断对端是否含某 PK ——
    QHash<QString, QVariantMap> localMap;
    for (const QVariantMap& row : localRows) {
        // pkCol 为空（无主键）时 key 取空串，下面的 isEmpty 判断会把它丢弃。
        QString key = pkCol.isEmpty() ? QString() : row.value(pkCol).toString();
        if (!key.isEmpty())
            localMap.insert(key, row);
    }

    QHash<QString, QVariantMap> remoteMap;
    for (const QVariantMap& row : remoteSlice) {
        QString key = pkCol.isEmpty() ? QString() : row.value(pkCol).toString();
        if (!key.isEmpty())
            remoteMap.insert(key, row);
    }

    QList<RowDiff> result;

    // —— Added：在对端有、本地没有的 PK（以本地视角看是“对端新增、待加入”的行）——
    // compareRows(空, 对端行)：local 侧全空，逐列只体现对端值。
    for (auto it = remoteMap.constBegin(); it != remoteMap.constEnd(); ++it) {
        if (!localMap.contains(it.key())) {
            RowDiff rd;
            rd.kind = RowDiffKind::Added;
            rd.primaryKey = it.key();
            rd.cells = compareRows(QVariantMap{}, it.value());
            result.append(rd);
        }
    }

    // —— Deleted：在本地有、对端没有的 PK（以本地视角看是“对端已删除”的行）——
    // compareRows(本地行, 空)：remote 侧全空，逐列只体现本地值。
    for (auto it = localMap.constBegin(); it != localMap.constEnd(); ++it) {
        if (!remoteMap.contains(it.key())) {
            RowDiff rd;
            rd.kind = RowDiffKind::Deleted;
            rd.primaryKey = it.key();
            rd.cells = compareRows(it.value(), QVariantMap{});
            result.append(rd);
        }
    }

    // —— Modified / Same：两端都有同一 PK 的行 ——
    // 先逐列比出 CellDiff，再看是否“有任意一列 changed”：
    //   有 → Modified（真的不同，需要合并取舍）；无 → Same（内容完全一致）。
    for (auto it = localMap.constBegin(); it != localMap.constEnd(); ++it) {
        if (remoteMap.contains(it.key())) {
            QList<CellDiff> cells = compareRows(it.value(), remoteMap[it.key()]);
            bool anyChanged = std::any_of(cells.constBegin(), cells.constEnd(),
                                          [](const CellDiff& c) { return c.changed; });
            RowDiff rd;
            rd.kind = anyChanged ? RowDiffKind::Modified : RowDiffKind::Same;
            rd.primaryKey = it.key();
            rd.cells = cells;
            result.append(rd);
        }
    }

    return result;
}

QList<QVariantMap> DiffEngine::fetchLocalRows(QSqlDatabase& rconn, const QString& table, int offset,
                                              int limit) {
    QList<QVariantMap> rows;
    // H-01 fix: always ORDER BY pk so offset-based paging is stable across INSERT/DELETE.
    // Without ORDER BY, rows arrive in undefined order and different offsets can overlap or skip.
    const QString pkCol = getPkColumn(rconn, table);
    // M-08 fix: use double-quote quoting via the same pattern as getPkColumn's PRAGMA.
    const QString quotedTable = QStringLiteral("\"") +
                                QString(table).replace(QLatin1Char('"'), QLatin1String("\"\"")) +
                                QStringLiteral("\"");
    QString sql = QStringLiteral("SELECT * FROM %1").arg(quotedTable);
    if (!pkCol.isEmpty()) {
        const QString quotedPk = QStringLiteral("\"") +
                                 QString(pkCol).replace(QLatin1Char('"'), QLatin1String("\"\"")) +
                                 QStringLiteral("\"");
        sql += QStringLiteral(" ORDER BY %1").arg(quotedPk);
    }
    if (limit >= 0)
        sql += " LIMIT :lim OFFSET :off";

    QSqlQuery q(rconn);
    q.prepare(sql);
    if (limit >= 0) {
        q.bindValue(":lim", limit);
        q.bindValue(":off", offset);
    }

    if (!q.exec())
        return rows;

    QSqlRecord rec = q.record();
    int colCount = rec.count();
    while (q.next()) {
        QVariantMap row;
        for (int i = 0; i < colCount; ++i)
            row.insert(rec.fieldName(i), q.value(i));
        rows.append(row);
    }
    return rows;
}

QString DiffEngine::getPkColumn(QSqlDatabase& rconn, const QString& table) {
    if (pkColCache_.contains(table))
        return pkColCache_.value(table);

    QSqlQuery q(rconn);
    // H-4 fix: use quoteIdent for table name in PRAGMA.
    q.prepare(QStringLiteral("PRAGMA table_info(") + detail::SqlBuilder::quoteIdent(table) +
              QLatin1Char(')'));
    if (!q.exec())
        return {};

    QString pkCol;
    int bestPk = INT_MAX;
    while (q.next()) {
        int pk = q.value("pk").toInt();
        if (pk > 0 && pk < bestPk) {
            bestPk = pk;
            pkCol = q.value("name").toString();
        }
    }

    pkColCache_.insert(table, pkCol);
    return pkCol;
}

QList<CellDiff> DiffEngine::compareRows(const QVariantMap& local, const QVariantMap& remote) {
    QList<CellDiff> cells;

    // Union of all column names.
    QSet<QString> allCols;
    for (auto it = local.constBegin(); it != local.constEnd(); ++it)
        allCols.insert(it.key());
    for (auto it = remote.constBegin(); it != remote.constEnd(); ++it)
        allCols.insert(it.key());

    for (const QString& col : allCols) {
        CellDiff cd;
        cd.column = col;
        cd.localValue = local.value(col);
        cd.remoteValue = remote.value(col);
        cd.changed = (cd.localValue != cd.remoteValue);
        cells.append(cd);
    }

    return cells;
}

}  // namespace dbridge::sync
