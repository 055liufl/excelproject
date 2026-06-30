// ============================================================================
// BaselineManager.cpp — 「全量基线」导出 / 导入引擎的实现
// ============================================================================
//
// 【本文件做什么】
//   实现 BaselineManager.h 声明的四个动作（两个公开、两个私有），完成「冷启动 /
//   落后过多」时的全量对齐：
//     · serializeTables()      —— 私有：把若干同步表的全部行打成一段压缩二进制；
//     · queryOriginCuts()      —— 本文件内的 static 辅助：读取 __sync_applied_vector
//                                  得到「逐 origin 的 applied 水位切点」（含 epoch）；
//     · deserializeAndApply()  —— 私有：上面的逆操作，先清表再逐行 INSERT 回灌；
//     · exportBaseline()       —— 公开：序列化 + 采集水位 → 组装 BaselineArtifact；
//     · applyBaseline()        —— 公开：把工件灌进本地库，并「重置」全部追踪存储；
//     · shouldFallbackToBaseline() —— 公开：判断该不该从增量退化为基线对齐。
//
// 【在同步管线里的位置】
//   触发：边缘节点发现序号空洞(E_SYNC_GAP)或刚入网 → 发 BaselineRequest；
//   导出：源端（通常是中心节点）收到后调 exportBaseline() 产出 BaselineArtifact；
//   传输：工件被装入 BaselineResponsePayload，经 PayloadCodec 编码 → outbox→inbox；
//   落地：接收端调 applyBaseline() 全表替换 + 重置状态，使本地「像刚从快照长出来」，
//         随后从快照水位无缝衔接后续增量。
//
// 【协作者（本文件引用到的其它组件）】
//   · detail::SqlBuilder ——（sql/SqlBuilder.h）安全转义表名/列名（quoteIdent）。
//   · WriteTxn          ——（sync/WriteTxn.h）RAII 写事务，begin/commit/rollback。
//   · AppliedVectorStore（av）——「逐 (origin,epoch) 的 applied 水位」存储，基线据
//     originCuts 调 resetTo() 重置它（增量空洞判定的起点就来自这里）。
//   · TableStateStore  （ts）—— 表级状态/结构指纹存储，resetFromBaseline() 重置。
//   · RowWinnerStore   （rw）—— 冲突仲裁的「逐行胜者」存储，基线先清空再为每行播种。
//   · ConsistencyCache（cache）—— 内存里的一致性缓存，应用后逐表失效。
//
// 【几个反复出现的关键标识（详见 SyncTypes.h / Errors.h）】
//   · origin / seq / stream_epoch：每条变更由 (origin, stream_epoch, seq) 唯一定位；
//     基线必须如实带走「截至导出时各 origin 各自追到的 seq + 其 epoch」（=originCuts）。
//   · __sync_* 元数据表：__sync_changelog（本地变更日志，取 MAX(local_seq) 做诊断）、
//     __sync_applied_vector（逐 (origin,epoch) 的 applied 水位）等。
//   · 失败错误码：本文件所有失败路径最终都把 *err 冠以 E_SYNC_BASELINE_FAILED
//     （见 exportBaseline 的 I-20 包裹、applyBaseline 的 wrapErr lambda）。
//
// 【序列化格式（serialize/deserialize 必须严格对称，否则解析错位）】
//   外层：qCompress(raw, level=6)。
//   raw（QDataStream，版本钉死 Qt_5_12 以保证跨版本可读）布局：
//     quint32 表数;
//     对每张表：QString 表名; quint64 行数; 行数个 QVariantMap（列名→值）;
//
// 注释风格对齐 Errors.h / SyncTypes.h / RowPayload.h：`// ──` 分节、中文、信息密集。
// 命名空间 dbridge::sync。
// ============================================================================

#include "sync/baseline/BaselineManager.h"

#include "dbridge/Errors.h"  // err::E_SYNC_BASELINE_FAILED 等错误码字面量

// ── Qt 依赖：序列化 / 哈希 / JSON / SQL 访问 ─────────────────────────────────
#include <QCryptographicHash>  // SHA-256：算 pkHash、contentHash（行内容指纹）
#include <QDataStream>         // 基线二进制的逐字段读写（版本钉死 Qt_5_12）
#include <QJsonDocument>       // 播种胜者时把行内容序列化成 JSON（winningContent）
#include <QJsonObject>         // 同上：逐列填进 JSON 对象
#include <QJsonValue>          // JSON 空值（Null）等
#include <QSqlError>           // 取 SQL 失败的可读错误文本（lastError().text()）
#include <QSqlQuery>           // 执行 SELECT/INSERT/DELETE/PRAGMA
#include <QSqlRecord>          // 取结果集的列名/列数（record()）

#include "sql/SqlBuilder.h"             // detail::SqlBuilder::quoteIdent（标识符转义）
#include "sync/WriteTxn.h"              // WriteTxn：RAII 写事务包装
#include "sync/apply/RowWinnerStore.h"  // RowWinnerStore::pkHash / RowWinner（胜者播种）

namespace dbridge::sync {

// ---------------------------------------------------------------------------
// serializeTables —— 把 tables 列出的全部表整张序列化成一段压缩二进制
// ---------------------------------------------------------------------------
// 做什么：遍历每张表，把所有行（列名→值的 QVariantMap）写入一个 QDataStream，最后
//         qCompress 压缩输出到 *out；并把 __sync_changelog 的 MAX(local_seq) 写入 *maxSeq。
// 为什么：基线 = 当前全量快照；这里用「自描述」的扁平格式（表数 / 表名 / 行数 / 行）打包，
//         接收端 deserializeAndApply() 按完全相同的顺序读回（两端格式必须严格对称）。
// 参数  ：rconn  只读连接（只 SELECT，不改库）；
//         tables 要导出的同步表清单；
//         out    出参：压缩后的基线二进制（成功时有效）；
//         maxSeq 出参：导出时刻本地 changelog 的最大 local_seq（诊断 + 作为基线行的种子 seq）；
//         err    出参：失败原因（此处为「裸」原因，未冠错误码；由 exportBaseline 统一加前缀）。
// 返回  ：true=全部表成功序列化；false=任一表 COUNT/SELECT 失败或行数对不上。
// 副作用：只读数据库；写 *out / *maxSeq / *err。
// 错误模式：COUNT 失败、SELECT 失败、写入行数 != COUNT（说明读取期间发生异常/并发改动）。
// 线程  ：在持有 rconn 的线程上同步执行；无内部并发。
// 复杂度：O(总行数 × 平均列数)；内存峰值 ≈ 未压缩 raw 的大小（基线很大时这里是体积来源，
//         上层据最终体积决定是否发 W_SYNC_BASELINE_LARGE 告警——本函数不做阈值判断）。
bool BaselineManager::serializeTables(QSqlDatabase& rconn, const QStringList& tables,
                                      QByteArray* out, qint64* maxSeq, QString* err) {
    QByteArray raw;
    QDataStream ds(&raw, QIODevice::WriteOnly);
    // 钉死流版本：保证「不同 Qt 版本的导出端/接收端」对 QVariant/QVariantMap 的二进制
    // 表示一致，避免跨版本读出错位。deserializeAndApply() 必须用相同版本读。
    ds.setVersion(QDataStream::Qt_5_12);

    // 格式第一字段：表的数量（接收端据此知道要循环读多少张表）。
    ds << static_cast<quint32>(tables.size());

    for (const QString& table : tables) {
        // M-02 fix: use quoteIdent() to safely escape table names with embedded double-quotes.
        // M-02 修复：用 quoteIdent() 安全转义表名（哪怕表名里含双引号也不会被 SQL 注入/破坏）。
        const QString quotedTable = detail::SqlBuilder::quoteIdent(table);

        // Count rows first so we can write rowCount before row data.
        // 先数行数，这样能在「行数据」之前把 rowCount 写进流——接收端必须先知道要读几行。
        QSqlQuery cntQ(rconn);
        cntQ.prepare(QStringLiteral("SELECT COUNT(*) FROM %1").arg(quotedTable));
        if (!cntQ.exec() || !cntQ.next()) {
            if (err)
                *err = QStringLiteral("count failed on %1: %2").arg(table, cntQ.lastError().text());
            return false;
        }
        const quint64 rowCount = static_cast<quint64>(cntQ.value(0).toLongLong());

        // 写「本表的头」：表名 + 行数。顺序与 deserializeAndApply 的 `ds >> tableName >> rowCount`
        // 对称。
        ds << table;
        ds << rowCount;

        // 再开一个查询拉取全部行。注意 SELECT * 的列序由表定义决定；行用「列名→值」的 map
        // 存储（而非按位置），所以接收端即便列序不同也能按列名回灌。
        QSqlQuery rowQ(rconn);
        rowQ.prepare(QStringLiteral("SELECT * FROM %1").arg(quotedTable));
        if (!rowQ.exec()) {
            if (err)
                *err =
                    QStringLiteral("select failed on %1: %2").arg(table, rowQ.lastError().text());
            return false;
        }

        const QSqlRecord rec = rowQ.record();  // 从结果集取列名/列数（用于构造每行的 map）
        const int colCount = rec.count();

        quint64 written = 0;
        while (rowQ.next()) {
            // 把当前行的每一列按「列名 → 值」放进 map，再整体写入流。
            QVariantMap rowMap;
            for (int c = 0; c < colCount; ++c) {
                rowMap.insert(rec.fieldName(c), rowQ.value(c));
            }
            ds << rowMap;
            ++written;
        }

        // 一致性自检：实际写出的行数必须等于先前 COUNT 出来的 rowCount。不等通常意味着
        // 读取过程中游标提前结束/发生并发改动——此时直接判失败，避免产出「头声明 N 行、
        // 实际只有 M 行」的损坏基线（接收端会按 rowCount 死等读 N 行而错位）。
        if (written != rowCount) {
            if (err)
                *err = QStringLiteral("row count mismatch for %1: expected %2 got %3")
                           .arg(table)
                           .arg(rowCount)
                           .arg(written);
            return false;
        }
    }

    // Max local_seq from changelog (diagnostic).
    // 取本地变更日志的最大 local_seq（仅诊断用 + 充当「基线行的种子序号」）。
    // 注意它与 originCuts 不同：maxSeq 是「本地见过的最高 changelog 序号」这一单值，
    // 而 originCuts 是「逐 origin 的权威 applied 水位」（更精确，见 queryOriginCuts）。
    {
        QSqlQuery seqQ(rconn);
        seqQ.prepare(QStringLiteral("SELECT MAX(local_seq) FROM __sync_changelog"));
        // 表为空时 MAX 返回 NULL；此时退化为 0（空库基线的合理种子）。
        if (seqQ.exec() && seqQ.next() && !seqQ.value(0).isNull()) {
            *maxSeq = seqQ.value(0).toLongLong();
        } else {
            *maxSeq = 0;
        }
    }

    // 压缩输出。level=6 是 zlib 的「速度/压缩比」折中默认值；基线常含大量重复结构，
    // 压缩能显著降低传输体积（也间接降低触发 W_SYNC_BASELINE_LARGE 的概率）。
    *out = qCompress(raw, 6);
    return true;
}

// ── queryOriginCuts —— 采集「逐 origin 的 applied 水位切点」（含 stream_epoch）─────
//
// C-03 fix: query the applied_vector (not changelog MAX) so the baseline carries authoritative
// applied sequences per (origin, stream_epoch) rather than the highest-seen changeset seq.
// Using the applied_vector prevents a race where a changeset arrived but was not yet applied.
// C-03 修复：从 __sync_applied_vector（而非 changelog 的 MAX）读取水位，使基线携带的是
//   「逐 (origin, stream_epoch) 真正已应用到的权威序号」，而不是「见过的最高 changeset 序号」。
//   用 applied_vector 能避免一个竞态：某条 changeset 已「收到」但还「没应用」时，若按
//   changelog MAX 取数，会把尚未应用的序号也算进基线水位，导致接收端漏掉那段变更。
//
// 做什么：扫描 __sync_applied_vector 全表，每行映射成一个 BaselineOriginCut。
// 参数  ：rconn 只读连接。
// 返回  ：水位切点向量；查询失败（如表不存在）时返回空向量（调用方会据此做兜底）。
// 副作用：只读。  线程：随调用线程。  复杂度：O(applied_vector 行数)。
// 文件作用域 static：仅供本文件内 exportBaseline 使用，不暴露到外部链接。
static QVector<BaselineOriginCut> queryOriginCuts(QSqlDatabase& rconn) {
    QVector<BaselineOriginCut> result;
    QSqlQuery q(rconn);
    // 列序：origin, stream_epoch, applied_seq —— 与下方 value(0/1/2) 取值一一对应。
    if (!q.exec(
            QStringLiteral("SELECT origin, stream_epoch, applied_seq FROM __sync_applied_vector")))
        return result;  // 查询失败：返回空。注意——空 originCuts 在 applyBaseline 里会触发
                        // 「primary origin 未命中 → 按 seq=0 兜底重置」的分支（见那里的注释）。
    while (q.next()) {
        BaselineOriginCut cut;
        cut.origin = q.value(0).toString();  // 来源节点 id
        cut.streamEpoch = q.value(1).toLongLong();  // 该来源所在的流纪元（基线对齐的关键维度）
        cut.appliedSeq = q.value(2).toLongLong();  // 已应用到该 (origin,epoch) 的此序号为止
        result.append(cut);
    }
    return result;
}

// ---------------------------------------------------------------------------
// deserializeAndApply —— serializeTables 的逆操作：解压 → 逐表「先清空、后逐行回灌」
// ---------------------------------------------------------------------------
// 做什么：解压 data，按与 serializeTables 完全对称的顺序读出（表数 / 每表：表名+行数+各行），
//         对每张表先 DELETE 清空、再把每一行 INSERT 回去；同时把读到的表名收集到 *tables。
// 为什么：基线是「全量替换」——接收端要把本地表整张换成快照内容，所以先清空再回灌，而不是合并。
// 参数  ：wconn  写连接（会 DELETE/INSERT）；
//         data   exportBaseline 产出的压缩基线二进制；
//         tables 出参：本基线涵盖的表名列表（供 applyBaseline 后续重置 table-state / 播种胜者）；
//         err    出参：失败「裸」原因（由上层 wrapErr 统一冠 E_SYNC_BASELINE_FAILED）。
// 返回  ：true=全部表清空+回灌成功；false=解压失败/数据损坏/SQL 失败。
// 副作用：清空并重写 *tables 列出的各同步表（破坏性！）。
// 重要约束：本函数必须在调用方已开启的写事务内运行，使「清表 + 重灌」要么全成、要么全回滚；
//           且调用方应在进入前关闭外键（PRAGMA foreign_keys=OFF），否则逐表 DELETE/INSERT
//           的中间态会违反父子表的外键约束（见 applyBaseline 的 C-1 fix）。
// 线程  ：随持有 wconn 的线程。  复杂度：O(总行数 × 平均列数)。
bool BaselineManager::deserializeAndApply(QSqlDatabase& wconn, const QByteArray& data,
                                          QStringList* tables, QString* err) {
    // 解压。qUncompress 对「损坏/截断/非 qCompress 数据」会返回空 QByteArray——据此判失败。
    const QByteArray raw = qUncompress(data);
    if (raw.isEmpty()) {
        if (err)
            *err = QStringLiteral("decompression failed or empty payload");
        return false;
    }

    QDataStream ds(raw);
    // 必须与 serializeTables 同一版本，否则 QVariantMap 等的二进制布局对不上 → 读出乱码。
    ds.setVersion(QDataStream::Qt_5_12);

    // 读第一字段：表数量。每次读后都检查 ds.status()——一旦流损坏/读越界，QDataStream 会把
    // status 置为非 Ok 且后续读全部返回默认值，所以「检查 status」是发现损坏基线的唯一手段。
    quint32 tableCount = 0;
    ds >> tableCount;
    if (ds.status() != QDataStream::Ok) {
        if (err)
            *err = QStringLiteral("corrupt baseline: failed to read table count");
        return false;
    }

    tables->clear();

    for (quint32 t = 0; t < tableCount; ++t) {
        // 读「本表的头」：表名 + 行数（与 serializeTables 写入顺序对称）。
        QString tableName;
        quint64 rowCount = 0;
        ds >> tableName >> rowCount;
        if (ds.status() != QDataStream::Ok) {
            if (err)
                *err = QStringLiteral("corrupt baseline: failed to read table header at index %1")
                           .arg(t);
            return false;
        }

        tables->append(tableName);  // 记录此表名，供 applyBaseline 后续步骤使用

        // DELETE existing rows.
        // 清空本表的现有行（全量替换的前半步）。表名用 quoteIdent 转义。
        {
            QSqlQuery delQ(wconn);
            delQ.prepare(QStringLiteral("DELETE FROM ") +
                         detail::SqlBuilder::quoteIdent(tableName));
            if (!delQ.exec()) {
                if (err)
                    *err = QStringLiteral("delete failed on %1: %2")
                               .arg(tableName, delQ.lastError().text());
                return false;
            }
        }

        // 逐行回灌（全量替换的后半步）。每行先按对称顺序从流里读出 map，再 INSERT。
        for (quint64 r = 0; r < rowCount; ++r) {
            QVariantMap rowMap;
            ds >> rowMap;
            if (ds.status() != QDataStream::Ok) {
                if (err)
                    *err = QStringLiteral("corrupt baseline: row %1 in table %2")
                               .arg(r)
                               .arg(tableName);
                return false;
            }

            // 防御：空 map 行（理论上不该出现）跳过，避免拼出无列的非法 INSERT。
            if (rowMap.isEmpty())
                continue;

            // M-05 fix: plain INSERT (not OR REPLACE). The DELETE above already cleared the
            // table, so no PK conflicts exist. INSERT OR REPLACE triggers DELETE+INSERT
            // semantics which can fire FK cascade deletes on child tables unexpectedly.
            // M-05 修复：用普通 INSERT（不是 INSERT OR REPLACE）。上面的 DELETE 已清空本表，
            //   不存在主键冲突，因此无需 REPLACE。而 INSERT OR REPLACE 在冲突时会触发
            //   「先 DELETE 旧行、再 INSERT」的语义，那次内部 DELETE 可能意外触发子表的
            //   外键级联删除（FK cascade），把刚回灌的数据连带删掉——故坚决用普通 INSERT。
            // M-11 fix: quote table and column identifiers (escapes embedded double-quotes).
            // M-11 修复：表名与列名都用 quoteIdent 转义（同样防御含双引号的标识符）。
            QStringList cols, placeholders;
            for (auto it = rowMap.cbegin(); it != rowMap.cend(); ++it) {
                cols << detail::SqlBuilder::quoteIdent(it.key());  // 列名（已转义）
                placeholders << QStringLiteral("?");  // 对应的占位符（参数化绑定）
            }

            // 拼 INSERT INTO 表 (列…) VALUES (?,?…)。用参数化占位符而非把值直接拼进 SQL，
            // 既防注入、又能让驱动正确处理各类型/二进制值。
            const QString sql =
                QStringLiteral("INSERT INTO %1 (%2) VALUES (%3)")
                    .arg(detail::SqlBuilder::quoteIdent(tableName), cols.join(QLatin1Char(',')),
                         placeholders.join(QLatin1Char(',')));

            QSqlQuery insQ(wconn);
            insQ.prepare(sql);
            // 绑定顺序：QVariantMap 的迭代顺序（按 key 排序）与上面拼 cols 的顺序一致，
            // 故同一遍迭代既定列序、也按相同序绑值，二者天然对齐。
            int idx = 0;
            for (auto it = rowMap.cbegin(); it != rowMap.cend(); ++it, ++idx) {
                insQ.bindValue(idx, it.value());
            }
            if (!insQ.exec()) {
                if (err)
                    *err = QStringLiteral("insert failed on %1 row %2: %3")
                               .arg(tableName)
                               .arg(r)
                               .arg(insQ.lastError().text());
                return false;
            }
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// exportBaseline —— 组装一份完整的 BaselineArtifact（导出方调用）
// ---------------------------------------------------------------------------
// 做什么：① 序列化全部同步表 → 压缩二进制；② 采集逐 origin 的 applied 水位切点；
//         ③ 把「导出节点自身的切点」按最大值并入；最终填好 *out。
// 为什么：让滞后/空白节点一次性拿到「全量数据 + 各 origin 的精确水位」，从而既能整张替换、
//         又能从正确的序号处继续追增量（不漏不重）。
// 参数  ：rconn 只读连接；tables 同步表清单；out 出参（工件）；err 出参（失败原因）；
//         localOrigin/localEpoch/localOriginSeq —— 导出节点「自己」的身份与当前已产出序号
//         （默认空/0；为 0 或空时跳过 M-01 的自身切点合并）。
// 返回  ：true=成功且 *out 完整；false=序列化失败，*err 已冠 E_SYNC_BASELINE_FAILED。
// 副作用：只读数据库；写 *out / *err。  线程：随调用线程。
// 复杂度：以 serializeTables 为主 O(总行数×列数)，加 O(applied_vector 行数)。
bool BaselineManager::exportBaseline(QSqlDatabase& rconn, const QStringList& tables,
                                     BaselineArtifact* out, QString* err,
                                     const QString& localOrigin, qint64 localEpoch,
                                     qint64 localOriginSeq) {
    qint64 maxSeq = 0;
    QByteArray data;
    if (!serializeTables(rconn, tables, &data, &maxSeq, err)) {
        // I-20: map inner failure to E_SYNC_BASELINE_FAILED error code.
        // I-20 修复：serializeTables 给的是「裸」原因；此处统一冠以 E_SYNC_BASELINE_FAILED，
        //   使上层只需匹配该错误码即可识别「基线导出失败」这一类错误（除非已是 E_SYNC* 开头）。
        if (err && !err->startsWith(QLatin1String("E_SYNC")))
            *err = QStringLiteral("%1: %2").arg(QLatin1String(err::E_SYNC_BASELINE_FAILED), *err);
        return false;
    }
    out->data = data;
    out->sourceMaxSeq = maxSeq;
    // C-03 fix: capture per-origin applied_vector snapshot so applyBaseline can call
    // av.resetTo(origin, epoch, seq, generation) with the correct epoch per origin.
    // C-03 修复：采集「逐 origin 的 applied_vector 快照」（含各自 epoch），使接收端的
    //   applyBaseline 能用「每个 origin 自己的 epoch」去调 av.resetTo()，而不是一律用本地
    //   epoch（远端 origin 的 epoch 可能与本地不同，用错就会错置水位）。
    out->originCuts = queryOriginCuts(rconn);

    // M-01 fix: the exporting node's own writes never advance its own __sync_applied_vector,
    // so the (localOrigin, localEpoch) cut is absent from queryOriginCuts().  Merge it in
    // by taking the max, so the receiver does not reset the exporter's seq to 0.
    // M-01 修复：applied_vector 只记「我应用了别人到第几号」，导出节点「自己产生」的写入
    //   从不会推进它自己的 applied_vector——所以 (localOrigin, localEpoch) 这条切点在
    //   queryOriginCuts() 的结果里是缺失的。这里据 localOriginSeq 把这条「自身切点」并进去
    //   （取最大值），否则接收端会把导出方的水位重置为 0，导出方之后的增量会被全当成 Gap 漏收。
    if (!localOrigin.isEmpty() && localEpoch > 0) {
        bool found = false;
        // 先看自身切点是否已意外存在于结果中（理论上不会，但若已存在则取较大的 seq，安全合并）。
        for (BaselineOriginCut& cut : out->originCuts) {
            if (cut.origin == localOrigin && cut.streamEpoch == localEpoch) {
                if (localOriginSeq > cut.appliedSeq)
                    cut.appliedSeq = localOriginSeq;
                found = true;
                break;
            }
        }
        // 未找到（常态）：追加一条「自身切点」，把导出方的真实序号带给接收端。
        if (!found) {
            BaselineOriginCut selfCut;
            selfCut.origin = localOrigin;
            selfCut.streamEpoch = localEpoch;
            selfCut.appliedSeq = localOriginSeq;
            out->originCuts.append(selfCut);
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// applyBaseline —— 把基线工件灌进本地库，并「重置」全部追踪存储（接收方调用）
// ---------------------------------------------------------------------------
// 做什么（整体步骤，全程在一个写事务内保证原子）：
//   ① 关外键（FK=OFF，必须在事务外执行，见 C-1）；
//   ② 开写事务 → deserializeAndApply() 逐表清空 + 回灌行；
//   ③ 用 art.originCuts 把 applied-vector 逐 (origin,epoch) 重置到权威水位；
//   ④ ts.resetFromBaseline() 重置 table-state 并写入 schemaFp；
//   ⑤ rw.resetAll() 清空 row-winner，再为每个导入行「播种」一条胜者记录；
//   ⑥ 提交事务 → 复原外键（FK=ON）→ 逐表失效一致性缓存；
//   ⑦ 把 newAnchorSeq 置为 sourceMaxSeq（新的同步锚点）。
// 为什么：基线 = 全量替换 + 状态对齐。仅换数据不够——还得把「我追到哪了 / 表结构指纹 /
//   逐行谁是胜者」这些追踪状态一并重置，否则后续增量/冲突仲裁会基于过期状态出错。
// 参数  ：见 BaselineManager.h 的逐项说明。h（原生 sqlite3*）当前实现未用，预留。
// 返回  ：true=全部成功并已提交；false=任一步失败，已回滚 + 复原外键，*err 冠
// E_SYNC_BASELINE_FAILED。 副作用：破坏性重写各同步表与多张 __sync_* 元数据表；临时改动 PRAGMA
// foreign_keys（最终复原）。 错误模式：FK 关闭失败 / 事务开启失败 / 反序列化失败 / 任一存储 reset
// 失败 / 提交失败。 线程  ：必须在持有写连接 wconn 的线程上独占执行（基线期间不应有并发写）。
// 复杂度：O(总行数 × 列数)（反序列化 + 播种各扫一遍）。
bool BaselineManager::applyBaseline(QSqlDatabase& wconn, sqlite3* /*h*/,
                                    const BaselineArtifact& art, AppliedVectorStore& av,
                                    TableStateStore& ts, RowWinnerStore& rw,
                                    ConsistencyCache& cache, qint64 epoch, const QString& origin,
                                    const QString& schemaFp, qint64* newAnchorSeq, QString* err,
                                    int baselineRank) {
    // I-20 helper: prefix err with E_SYNC_BASELINE_FAILED if not already an E_SYNC code.
    // I-20 辅助：把「裸」错误原因统一冠以 E_SYNC_BASELINE_FAILED（若尚非 E_SYNC 开头）。
    //   下方各失败路径都先填具体原因、再调 wrapErr(err) 加前缀，保证调用方能按码识别。
    auto wrapErr = [&](QString* ep) {
        if (ep && !ep->startsWith(QLatin1String("E_SYNC")))
            *ep = QStringLiteral("%1: %2").arg(QLatin1String(err::E_SYNC_BASELINE_FAILED), *ep);
    };

    // C-1 fix: PRAGMA foreign_keys cannot be changed from within a SQLite transaction — the
    // pragma is silently ignored if issued inside BEGIN...COMMIT. Disable FK enforcement
    // BEFORE opening the transaction so the setting takes effect immediately.
    // C-1 修复：PRAGMA foreign_keys 不能在事务内修改——若在 BEGIN...COMMIT 之间发出，SQLite
    //   会「静默忽略」它。所以必须在开事务之前就把外键约束关掉，让设置立即生效。
    //   为何要关：全量回灌时父子表逐张清空/重灌，中间态必然出现「子表引用了尚未灌入的父行」，
    //   若开着外键就会报约束失败。关掉外键，等全部灌完、提交后再开回来即可。
    {
        QSqlQuery pragmaOff(wconn);
        if (!pragmaOff.exec(QStringLiteral("PRAGMA foreign_keys=OFF"))) {
            if (err)
                *err = QStringLiteral("%1: cannot disable FK enforcement: %2")
                           .arg(QLatin1String(err::E_SYNC_BASELINE_FAILED),
                                pragmaOff.lastError().text());
            return false;  // 注意：此处尚未开事务、也还没关成外键，直接返回即可，无需回滚/复原。
        }
    }

    // 开写事务。从这里开始，后续任一失败都必须 rollback + 把外键开回去（见各失败分支）。
    WriteTxn txn(wconn);
    if (!txn.begin(err)) {
        // 事务都没开起来：直接把刚关掉的外键复原，再返回。
        QSqlQuery pragmaOn(wconn);
        pragmaOn.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
        wrapErr(err);
        return false;
    }

    // 步骤②：解压并「先清空、后逐行回灌」。tables 收到本基线涵盖的表名（后续步骤复用）。
    QStringList tables;
    if (!deserializeAndApply(wconn, art.data, &tables, err)) {
        txn.rollback();  // 回滚清表/回灌
        QSqlQuery pragmaOn(wconn);
        pragmaOn.exec(QStringLiteral("PRAGMA foreign_keys=ON"));  // 复原外键
        wrapErr(err);
        return false;
    }

    // C-4 fix: helper lambda to restore FK ON — called on every error and success path.
    // This is the RAII-style guard that prevents FK=OFF from leaking into the connection state
    // after any failure between FK=OFF and the final successful FK=ON.
    // C-4 修复：用一个 lambda 统一「把外键开回去」，此后每条错误路径与成功路径都调用它。
    //   它充当类 RAII 的「守卫」，确保 FK=OFF 这个连接级设置不会因中途失败而泄漏残留
    //   （否则该连接后续所有操作都将在「无外键约束」下运行，埋下数据完整性隐患）。
    //   注意：上面两个分支发生在本 lambda 定义之前，故它们各自内联写了 PRAGMA foreign_keys=ON。
    auto restoreFk = [&]() {
        QSqlQuery pragmaOn(wconn);
        pragmaOn.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
    };

    // C-03 fix: reset applied_vector per (origin, epoch) using the authoritative cuts captured
    // at export time. Each cut carries its own stream_epoch so we call resetTo() with the
    // correct epoch instead of the local node's epoch (which may differ for remote origins).
    // C-03 修复（步骤③）：用导出时采集的权威切点 art.originCuts，逐 (origin,epoch) 重置
    //   applied-vector。关键点：每个 cut 自带 stream_epoch，所以对每个 origin 都用「它自己的
    //   epoch」调 resetTo()，而不是用本地节点的 epoch（远端 origin 的 epoch 可能不同）。
    //   这是「多 origin 基线」能正确对齐水位的核心修复。
    {
        bool primaryReset = false;  // 标记「主 origin」是否已在切点循环里被重置过
        for (const BaselineOriginCut& cut : art.originCuts) {
            // Use each cut's own epoch — this is the critical fix for multi-origin baselines.
            // 用每个切点自己的 epoch（cut.streamEpoch）—— 多 origin 基线的关键。
            // resetTo 把该 (origin,epoch) 的水位重置到 cut.appliedSeq；art.sourceMaxSeq 作为
            // 「代次/generation」参数（用于隔离本次基线与历史，具体语义见 AppliedVectorStore.h）。
            if (!av.resetTo(wconn, cut.origin, cut.streamEpoch, cut.appliedSeq, art.sourceMaxSeq,
                            err)) {
                txn.rollback();
                restoreFk();
                wrapErr(err);
                return false;
            }
            if (cut.origin == origin)
                primaryReset = true;  // 主 origin 已被切点覆盖，无需下面的兜底
        }
        // If the primary origin was not in the cuts (empty baseline), reset it to seq=0.
        // 若主 origin 不在切点里（典型：空库基线，applied_vector 无任何行）——兜底把它重置到
        // seq=0，使接收端把主 origin 的水位归零，从头按增量追（避免主 origin 水位悬空未定义）。
        if (!primaryReset) {
            if (!av.resetTo(wconn, origin, epoch, 0, art.sourceMaxSeq, err)) {
                txn.rollback();
                restoreFk();
                wrapErr(err);
                return false;
            }
        }
    }

    // H-05 fix: pass the caller-supplied schemaFp so DiffEngine::tableDiffs() can match
    // the remote fingerprint and avoid producing false "Different" results after baseline apply.
    // H-05 修复（步骤④）：把调用方给的 schemaFp 写进 table-state，重置各表的表级状态。
    //   这样基线应用后，DiffEngine::tableDiffs() 能拿到与远端一致的结构指纹，避免误判为
    //   「Different」而触发一次无谓的全表 diff。epoch 也一并写入，标记表进入新的流纪元。
    if (!ts.resetFromBaseline(wconn, tables, epoch, schemaFp, err)) {
        txn.rollback();
        restoreFk();
        wrapErr(err);
        return false;
    }

    // Clear row winner store — all rows now reflect baseline truth.
    // 步骤⑤(a)：清空 row-winner 存储——基线一落，所有行都以「基线真相」为准，旧胜者记录全部作废。
    if (!rw.resetAll(wconn, err)) {
        txn.rollback();
        restoreFk();
        wrapErr(err);
        return false;
    }

    // M-02 fix: after clearing row winners, seed RowWinner entries for every imported row so
    // subsequent low-rank challengers cannot overwrite baseline truth.  We use the same
    // pkHash format as ChangesetApplier: SHA-256(pk_value1\0pk_value2\0...).left(16).toHex().
    // appliedSeq for baseline rows is sourceMaxSeq (the highest changelog seq on the exporter).
    // M-02 修复（步骤⑤(b)）：清空后，为「每一个导入行」播种一条 RowWinner 记录，使后续
    //   「rank 更低的挑战者」无法覆盖基线确立的事实（仲裁规则见 RowWinnerStore.h：先比 rank、
    //   再比 seq）。pkHash 格式必须与 ChangesetApplier 完全一致：
    //     SHA-256(pk值1\0pk值2\0...).left(16).toHex()  ——否则两边落在不同键空间、对不上。
    //   基线行的 appliedSeq 取 sourceMaxSeq（导出端最高的 changelog 序号）作为种子序号。
    {
        const qint64 baselineSeq = art.sourceMaxSeq;
        bool seedOk = true;  // 任一表/行播种失败即置 false，跳出后统一回滚
        for (const QString& tableName : tables) {
            if (!seedOk)
                break;
            // Collect PK column names and their indices via PRAGMA table_info.
            // 先用 PRAGMA table_info 取出本表的主键列名（pk>0 的列即主键，可能是复合主键）。
            QStringList pkCols;
            {
                QSqlQuery pragmaQ(wconn);
                pragmaQ.prepare(QStringLiteral("PRAGMA table_info(") +
                                detail::SqlBuilder::quoteIdent(tableName) + QLatin1Char(')'));
                // M-01 fix: treat PRAGMA failure as a hard error — we cannot compute pk_hash
                // without column info; silently skipping would leave the table unprotected.
                // M-01 修复：PRAGMA 失败按「硬错误」处理——没有列信息就算不出 pk_hash；若静默
                //   跳过这张表，它就不会被播种胜者，基线真相得不到保护（低 rank 变更能随意覆盖）。
                if (!pragmaQ.exec()) {
                    if (err)
                        *err = QStringLiteral("M-01: PRAGMA table_info failed for %1: %2")
                                   .arg(tableName, pragmaQ.lastError().text());
                    seedOk = false;
                    break;
                }
                while (pragmaQ.next()) {
                    if (pragmaQ.value(QStringLiteral("pk")).toInt() > 0)
                        pkCols << pragmaQ.value(QStringLiteral("name")).toString();
                }
            }
            if (pkCols.isEmpty()) {
                // M-01 fix: no PK columns found — PRAGMA ran but returned no PK rows.
                // This means we cannot compute pk_hash; fail so the caller rolls back.
                // M-01 修复：PRAGMA 跑通了但没有任何主键列——同样算不出 pk_hash，直接判失败，
                //   让本函数走回滚分支（同步表理应都有主键；无主键说明表设计或基线异常）。
                if (err)
                    *err =
                        QStringLiteral("M-01: table %1 has no primary key columns").arg(tableName);
                seedOk = false;
                break;
            }

            // 拉取本表全部行（这些行是刚刚 deserializeAndApply 回灌进去的基线行），逐行播种胜者。
            QSqlQuery rowQ(wconn);
            rowQ.prepare(QStringLiteral("SELECT * FROM ") +
                         detail::SqlBuilder::quoteIdent(tableName));
            // M-01 fix: SELECT failure must roll back baseline, not silently skip the table.
            // M-01 修复：SELECT 失败必须回滚整个基线，而不是悄悄跳过这张表（否则该表无胜者保护）。
            if (!rowQ.exec()) {
                if (err)
                    *err = QStringLiteral("M-01: SELECT * failed for %1: %2")
                               .arg(tableName, rowQ.lastError().text());
                seedOk = false;
                break;
            }

            const QSqlRecord rec = rowQ.record();  // 取列名/列数（构造 pkMap 与内容材料用）
            const int colCount = rec.count();
            while (rowQ.next()) {
                // H-05 fix: use canonical type-tagged pkHash (same as ChangesetApplier)
                // so baseline-seeded winners share the same key space as changeset challengers.
                // H-05 修复：用与 ChangesetApplier 一致的「带类型标签的规范 pkHash」，使基线
                //   播种出的胜者与增量挑战者落在同一键空间——这样后续增量才能正确命中同一行的胜者。
                // 先把本行的主键列收成 pkMap（列名→值）；找不到列下标则填空 QVariant（防御）。
                QVariantMap pkMap;
                for (const QString& pkCol : pkCols) {
                    const int colIdx = rec.indexOf(pkCol);
                    pkMap[pkCol] = (colIdx >= 0) ? rowQ.value(colIdx) : QVariant();
                }
                const QString pkHashStr = RowWinnerStore::pkHash(pkMap);  // 复用统一的 pkHash 算法

                // H-01 fix: build winningContent (JSON) and contentHash so that
                // ChangesetApplier::updateWinnersFromChangeset() can restore this row when a
                // low-rank DELETE arrives later. Without these fields the recovery path returns
                // E_SYNC_APPLY_CONSTRAINT and permanently stalls the sync stream.
                //
                // Format mirrors ChangesetApplier's INSERT/UPDATE branch exactly:
                //   INTEGER  → "__i64:<n>"  (preserves full int64 precision)
                //   BLOB     → "__b64:<base64>"
                //   FLOAT    → JSON double
                //   TEXT/etc → JSON string
                //   NULL     → JSON null
                //
                // contentHash = SHA-256 of the content material (all column bytes + \0) .left(16).
                // H-01 修复：构造 winningContent（整行内容的 JSON）与 contentHash（内容指纹），
                //   使得当「低 rank 的 DELETE」稍后到达时，ChangesetApplier::updateWinners-
                //   FromChangeset() 能据此「复活/还原」这一行。若缺这两个字段，该恢复路径会返回
                //   E_SYNC_APPLY_CONSTRAINT，并使整条同步流永久卡死。
                //
                //   编码格式必须与 ChangesetApplier 的 INSERT/UPDATE 分支「逐字节」一致：
                //     INTEGER  → "__i64:<n>"   （文本承载，保全 int64 完整精度，避免 JSON double
                //     丢精度） BLOB     → "__b64:<base64>"（二进制转 base64 文本） FLOAT    → JSON
                //     double TEXT/其它 → JSON 字符串 NULL     → JSON null
                //   contentHash = SHA-256(内容材料).left(16)，内容材料 = 各列字节依次拼接、每列后补
                //   '\0' 分隔。
                QJsonObject rowJson;         // 将落入 winningContent 的 JSON 对象
                QByteArray contentMaterial;  // 算 contentHash 的原料（逐列字节 + 分隔符）
                for (int ci = 0; ci < colCount; ++ci) {
                    const QVariant v = rowQ.value(ci);
                    const QString colKey = rec.fieldName(ci);

                    QByteArray colBytes;  // 本列贡献给 contentMaterial 的字节
                    if (v.isNull() || !v.isValid()) {
                        rowJson[colKey] = QJsonValue::Null;
                        // null contributes empty bytes + separator
                        // NULL：JSON 记 null；contentMaterial 只追加空字节 + 分隔符（colBytes
                        // 保持空）。
                    } else {
                        // 按 SQLite/QVariant 的存储类型分支编码，确保与 ChangesetApplier 完全对称。
                        switch (static_cast<int>(v.type())) {
                            case QVariant::LongLong:
                            case QVariant::Int:
                            case QVariant::UInt:
                            case QVariant::ULongLong: {
                                // 整数：用 "__i64:" 文本前缀承载，避免被当成 JSON double 丢失精度。
                                const qint64 iv = v.toLongLong();
                                rowJson[colKey] = QStringLiteral("__i64:") + QString::number(iv);
                                colBytes = QByteArray::number(iv);
                                break;
                            }
                            case QVariant::Double: {
                                // 浮点：直接存 JSON double。
                                rowJson[colKey] = v.toDouble();
                                colBytes = v.toByteArray();
                                break;
                            }
                            case QVariant::ByteArray: {
                                // BLOB：转 base64 文本，加 "__b64:" 前缀标识。
                                const QByteArray ba = v.toByteArray();
                                rowJson[colKey] =
                                    QStringLiteral("__b64:") + QString::fromLatin1(ba.toBase64());
                                colBytes = ba;  // 注意：指纹用原始二进制字节，而非 base64 文本
                                break;
                            }
                            default: {
                                // 文本及其它：存为 JSON 字符串；指纹用其 UTF-8 字节。
                                const QString s = v.toString();
                                rowJson[colKey] = s;
                                colBytes = s.toUtf8();
                                break;
                            }
                        }
                    }
                    contentMaterial.append(colBytes);
                    contentMaterial.append(
                        '\0');  // 列间分隔符，避免不同切分产生相同拼接（哈希碰撞）
                }

                // 内容指纹：SHA-256 取前 16 字节（与 ChangesetApplier 的截断长度一致）。
                const QByteArray contentH =
                    QCryptographicHash::hash(contentMaterial, QCryptographicHash::Sha256).left(16);
                // 胜者内容：rowJson 紧凑序列化成字符串，供后续 DELETE 恢复时重建该行。
                const QString winningContentStr =
                    QString::fromUtf8(QJsonDocument(rowJson).toJson(QJsonDocument::Compact));

                // 组装这一行的胜者记录：来源记为本基线的 origin，rank 用 baselineRank（M-02
                // 防覆盖）， 序号用 baselineSeq（=sourceMaxSeq）。
                RowWinner winner;
                winner.origin = origin;
                winner.rank = baselineRank;
                winner.originSeq = baselineSeq;
                winner.contentHash = contentH;
                winner.winningContent = winningContentStr;
                // M-01 fix: propagate put() failures so the transaction is rolled back.
                // M-01 修复：把 put()
                // 的失败如实上抛，使事务回滚（而非吞掉错误留下不完整的胜者集）。
                QString putErr;
                if (!rw.put(wconn, tableName, pkHashStr, winner, &putErr)) {
                    if (err)
                        *err =
                            QStringLiteral("M-01: rw.put failed for %1: %2").arg(tableName, putErr);
                    seedOk = false;
                    break;
                }
            }
        }
        // 播种过程中任一步失败：回滚整个基线事务 + 复原外键，返回失败。
        if (!seedOk) {
            txn.rollback();
            restoreFk();
            wrapErr(err);
            return false;
        }
    }

    // 步骤⑥：提交事务。提交失败也要复原外键再返回（事务对象析构会兜底回滚未提交事务）。
    if (!txn.commit(err)) {
        restoreFk();
        wrapErr(err);
        return false;
    }

    // Re-enable FK enforcement AFTER the transaction commits.
    // 事务提交「之后」再把外键约束开回来（同 C-1：PRAGMA 不能在事务内改，提交后才是安全时机）。
    restoreFk();

    // Invalidate in-memory consistency cache for each table (outside txn is fine).
    // 逐表失效内存里的一致性缓存——表数据已被整张替换，旧缓存全部过期。放在事务外执行即可
    // （缓存是进程内状态，不参与 DB 事务）。
    for (const QString& table : tables) {
        cache.invalidateTable(wconn, table);
    }

    // 步骤⑦：把新的同步锚点序号回传给调用方（= 本次基线截止的最大序号）。
    *newAnchorSeq = art.sourceMaxSeq;
    return true;
}

// ---------------------------------------------------------------------------
// shouldFallbackToBaseline —— 判断「该不该从增量退化为全量基线对齐」
// ---------------------------------------------------------------------------
// 做什么：纯比较。本地已应用到 appliedSeq；源端目前能提供的「最早」序号是 sourceMinSeq。
//   若 appliedSeq < sourceMinSeq，说明本地缺的那段 changeset（(appliedSeq, sourceMinSeq) 之间）
//   已被源端压实/清除（compact），永远拿不到了——只能改走全量基线一次性追平。
// 参数  ：appliedSeq 本地已应用到的序号；sourceMinSeq 源端现存最早可提供的序号。
// 返回  ：true=应回退到基线；false=增量仍可无缝衔接（本地起点仍在源端可提供的范围内）。
// 副作用：无（const 纯函数）。  线程：无状态，随处可调。  复杂度：O(1)。
bool BaselineManager::shouldFallbackToBaseline(qint64 appliedSeq, qint64 sourceMinSeq) const {
    return appliedSeq < sourceMinSeq;
}

}  // namespace dbridge::sync
