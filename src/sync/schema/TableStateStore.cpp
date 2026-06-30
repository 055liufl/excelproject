#include "TableStateStore.h"

#include <QCryptographicHash>  // SHA-256 行哈希
#include <QDataStream>         // 行序列化（长度前缀 + 类型标签）
#include <QDateTime>           // updated_ms 时间戳
#include <QSqlError>           // 取 lastError() 文本
#include <QSqlQuery>
#include <QSqlRecord>  // 整表扫描时按字段名取值
#include <QStringList>
#include <QVariant>

#include "sql/SqlBuilder.h"  // quoteIdent：安全地为表名加引号

// ============================================================================
// TableStateStore.cpp — __sync_table_state 增量校验和的实现
// 设计动机、模加语义、字符串存储等详见配套头文件 TableStateStore.h 的文件头注释。
// ============================================================================

namespace dbridge::sync {

// ---------------------------------------------------------------------------
// public（对外接口实现）
// ---------------------------------------------------------------------------

bool TableStateStore::init(QSqlDatabase& db, QString* err) {
    // Table is created by SyncDDL; here we just verify it is accessible.
    // 译：表由 SyncDDL 负责建立；这里只验证它能被访问（不建表）。
    // "WHERE 0" 恒为假，故这是一条「零行查询」：不返回任何数据，纯粹用来探测
    // “表是否存在、连接是否可用”。任何 SQL 层错误都会让 exec() 返回 false。
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM __sync_table_state WHERE 0"))) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

bool TableStateStore::applyMutations(QSqlDatabase& db, const QList<TableMutation>& muts,
                                     qint64 streamEpoch, const QString& schemaFp, qint64 originSeq,
                                     QString* err) {
    // Aggregate per-table deltas using unsigned quint64 to preserve modular-sum semantics.
    // M-02 fix: use separate add/sub quint64 instead of signed checksumDelta to avoid
    // signed-overflow UB when high bits are set.
    // 译：先按表把变更聚合成增量，用无符号 quint64 保持「模加和」语义。
    // M-02 fix：刻意把「要加的」与「要减的」拆成两个独立 quint64（add/sub），而不是
    // 合并成一个带符号的 checksumDelta——因为行哈希高位常被置位，带符号相减会触碰
    // 有符号溢出这一未定义行为（UB）；全程无符号则只是良性的模 2^64 环绕。
    struct Delta {
        quint64 add = 0;      // 本表所有「新增/更新后」行哈希贡献之和
        quint64 sub = 0;      // 本表所有「删除/更新前」行哈希贡献之和
        qint64 rowDelta = 0;  // 本表行数净变化（插入 +1、删除 -1、更新 0）
    };
    QMap<QString, Delta> deltas;  // 按表名聚合（QMap 有序，遍历顺序确定）

    // 第一遍：把每条变更归类累加到其所属表的 Delta 上。
    for (const TableMutation& m : muts) {
        Delta& d = deltas[m.table];  // operator[] 不存在则默认构造一个全 0 的 Delta
        if (m.isInsert) {
            // 插入：只新增一行的哈希贡献，行数 +1（before 不存在）。
            d.add += hashToU64(m.afterHash);
            d.rowDelta += 1;
        } else if (m.isDelete) {
            // 删除：从校验和里减去这行原先的贡献，行数 -1（after 不存在）。
            d.sub += hashToU64(m.beforeHash);
            d.rowDelta -= 1;
        } else {
            // 更新：等价于「减旧值、加新值」，行数不变。
            d.add += hashToU64(m.afterHash);
            d.sub += hashToU64(m.beforeHash);
        }
    }

    // 第二遍：把每张受影响表的聚合增量各写一次（upsert）。
    // 任一表写失败立即短路返回——调用方应在事务内，失败会随事务回滚。
    for (auto it = deltas.begin(); it != deltas.end(); ++it) {
        if (!updateRow(db, it.key(), streamEpoch, schemaFp, it.value().add, it.value().sub,
                       it.value().rowDelta, originSeq, err)) {
            return false;
        }
    }
    return true;
}

bool TableStateStore::readState(QSqlDatabase& db, const QString& table, qint64 streamEpoch,
                                QString* fp, QString* checksum, qint64* rowCount, bool* found,
                                QString* err) {
    // J-12: Distinguish "not found" (table never synced) from "query error".
    // 译：J-12 — 必须区分「查无此行（表从未被同步过）」与「查询出错」。
    // 先把 *found 预置为 false：只有在确实命中一行时才会翻成 true（见下）。
    if (found)
        *found = false;

    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("SELECT schema_fingerprint, content_checksum, row_count "
                       "FROM __sync_table_state "
                       "WHERE table_name = ? AND stream_epoch = ?"));
    q.addBindValue(table);
    q.addBindValue(streamEpoch);
    if (!q.exec()) {
        // SQL 执行失败：这是真正的错误，返回 false 并上报。
        if (err)
            *err = q.lastError().text();
        return false;  // genuine query error（译：真正的查询错误）
    }
    if (!q.next()) {
        // Row does not exist — not an error; *found remains false.
        // 译：没有命中行 —— 这不是错误（表只是还没被同步过）；*found 保持 false。
        // 返回 true 让调用方据 *found 区分“新表”而非误判为查询失败。
        return true;
    }
    // 命中一行：标记 found 为真，并按需回填三项表态（输出指针允许为 null）。
    if (found)
        *found = true;
    if (fp)
        *fp = q.value(0).toString();  // 结构指纹
    if (checksum)
        *checksum = q.value(1).toString();  // 校验和（十进制字符串原样返回）
    if (rowCount)
        *rowCount = q.value(2).toLongLong();  // 行数
    return true;
}

bool TableStateStore::resetFromBaseline(QSqlDatabase& db, const QStringList& tables,
                                        qint64 streamEpoch, const QString& schemaFp, QString* err) {
    // Delete existing rows for this epoch, then rebuild by scanning each table.
    // 译：先删掉本纪元下已有的表态行，再逐表全扫重建（“从头算”出权威真值）。
    for (const QString& tbl : tables) {
        // 步骤 1：删除该表在本纪元的旧表态（用代码块 {} 限定 del 查询对象的生命周期）。
        {
            QSqlQuery del(db);
            del.prepare(QStringLiteral(
                "DELETE FROM __sync_table_state WHERE table_name = ? AND stream_epoch = ?"));
            del.addBindValue(tbl);
            del.addBindValue(streamEpoch);
            if (!del.exec()) {
                if (err)
                    *err = del.lastError().text();
                return false;
            }
        }

        // Scan all rows of the business table to compute checksum + count.
        // H-3 fix: use quoteIdent to handle table names with embedded double-quotes.
        // 译：整表扫描以重算校验和 + 行数。
        // H-3 fix：表名经 quoteIdent 安全加引号，正确处理名字里含双引号等特殊字符的表
        //   （把内嵌的 " 转义成 ""），杜绝 SQL 注入/语法错误。注意此处表名不能用占位符
        //   参数化（占位符只能绑定值，不能绑定标识符），所以必须靠 quoteIdent 拼接。
        QSqlQuery scan(db);
        if (!scan.exec(QStringLiteral("SELECT * FROM ") + detail::SqlBuilder::quoteIdent(tbl))) {
            if (err)
                *err = scan.lastError().text();
            return false;
        }

        // 步骤 2：逐行算行哈希并模加累计，同时计数。
        quint64 runningSum = 0;  // quint64 累加，溢出即按模 2^64 环绕（与增量记账同一语义）
        qint64 rowCount = 0;
        while (scan.next()) {
            // 把当前行的每个字段按「字段名 → 值」装入 QVariantMap；
            // rowHash() 内部会按字段名排序后规范化序列化，保证与机器/列序无关的确定哈希。
            QVariantMap row;
            const QSqlRecord rec = scan.record();
            for (int i = 0; i < rec.count(); ++i) {
                row.insert(rec.fieldName(i), rec.value(i));
            }
            runningSum += hashToU64(rowHash(row));
            ++rowCount;
        }

        // 步骤 3：写回一行全新的权威表态。
        // 用 INSERT OR REPLACE 兜底（即便上面的 DELETE 因并发等原因没清干净也能覆盖）；
        // high_water_seq 直接以字面量 0 重置——基线重算意味着“处理进度从头开始记”。
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        QSqlQuery ins(db);
        ins.prepare(
            QStringLiteral("INSERT OR REPLACE INTO __sync_table_state "
                           "(table_name, stream_epoch, schema_fingerprint, high_water_seq, "
                           " content_checksum, row_count, updated_ms) "
                           "VALUES (?, ?, ?, 0, ?, ?, ?)"));
        ins.addBindValue(tbl);
        ins.addBindValue(streamEpoch);
        ins.addBindValue(schemaFp);
        ins.addBindValue(QString::number(runningSum));  // quint64 → 十进制字符串无损落盘
        ins.addBindValue(rowCount);
        ins.addBindValue(nowMs);
        if (!ins.exec()) {
            if (err)
                *err = ins.lastError().text();
            return false;
        }
    }
    return true;
}

QByteArray TableStateStore::rowHash(const QVariantMap& row) {
    // H-04 fix: use length-prefix + type-tag encoding to prevent constructible collisions.
    // Format: quint32 col_count, then per-column: quint32 key_len, key_bytes,
    //   quint8 type (0=NULL, 1=int64, 2=double, 3=text, 4=blob), then value.
    // Columns are iterated in QMap (sorted) key order so hashes are deterministic.
    // 译：H-04 fix —— 采用「长度前缀 + 类型标签」编码，杜绝「可构造的碰撞」。
    // 格式：先写 quint32 列数；随后每列依次写 quint32 键长、键字节、quint8 类型标签
    //   （0=NULL、1=int64、2=double、3=text、4=二进制 blob），再写该类型对应的值。
    // 为什么这么严谨：若简单地把字段值拼起来再哈希，攻击者/巧合可让 {a="12", b="3"}
    //   与 {a="1", b="23"} 拼出相同字节流而碰撞；加上「每段长度前缀 + 类型标签」后，
    //   边界与类型都被显式编码，不同结构永远产生不同字节流。
    // 确定性来源：QVariantMap 即 QMap，遍历天然按键（列名）升序——保证无论列的插入
    //   顺序如何，同一行总产生同一字节流，两端校验和才可比较。
    QByteArray data;
    QDataStream ds(&data, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::BigEndian);  // 固定大端：跨平台字节序一致
    ds << quint32(row.size());                // 先写列数
    for (auto it = row.cbegin(); it != row.cend(); ++it) {
        // 列名：先写长度，再写原始 UTF-8 字节（长度前缀杜绝相邻字段粘连歧义）。
        const QByteArray key = it.key().toUtf8();
        ds << quint32(key.size());
        ds.writeRawData(key.constData(), key.size());
        const QVariant& v = it.value();
        // 按类型打标签后再写值。注意判定顺序有意为之：
        //   先判 NULL、再判 ByteArray（blob），最后才落到数值/文本——避免把二进制
        //   数据误当成可转 number 的文本处理。
        if (v.isNull()) {
            ds << quint8(0);  // NULL：只写标签，无值
        } else if (v.type() == QVariant::ByteArray) {
            ds << quint8(4);
            const QByteArray ba = v.toByteArray();
            ds << quint32(ba.size());  // blob 也加长度前缀
            ds.writeRawData(ba.constData(), ba.size());
        } else if (v.canConvert<qlonglong>()) {
            // 整型：标签 1 + 定长 8 字节 int64（定长无需长度前缀）。
            ds << quint8(1) << qint64(v.toLongLong());
        } else if (v.canConvert<double>()) {
            // 浮点：标签 2 + 定长 8 字节 double。
            ds << quint8(2) << double(v.toDouble());
        } else {
            // 其余一律按文本：标签 3 + 长度 + UTF-8 字节。
            ds << quint8(3);
            const QByteArray str = v.toString().toUtf8();
            ds << quint32(str.size());
            ds.writeRawData(str.constData(), str.size());
        }
    }
    // SHA-256 后只取前 16 字节：足够低碰撞率，又省存储/带宽（同步自检无需 32 字节强度）。
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).left(16);
}

// ---------------------------------------------------------------------------
// private helpers（私有辅助）
// ---------------------------------------------------------------------------

quint64 TableStateStore::hashToU64(const QByteArray& h) {
    // 取哈希前 8 字节，按大端组装成 quint64。
    // 不足 8 字节直接返回 0（理论上 rowHash 产出 16 字节，这里是防御性兜底）。
    if (h.size() < 8)
        return 0;
    quint64 v = 0;
    for (int i = 0; i < 8; ++i) {
        // 每轮左移 8 位腾出一个字节位，再 OR 进下一个字节（h[i] 先转 quint8 防符号扩展）。
        v = (v << 8) | static_cast<quint8>(h[i]);
    }
    return v;
}

bool TableStateStore::updateRow(QSqlDatabase& db, const QString& table, qint64 streamEpoch,
                                const QString& schemaFp, quint64 add, quint64 sub,
                                qint64 rowCountDelta, qint64 highWaterSeq, QString* err) {
    // 先读旧值（不存在则按 0 处理），再叠加本批增量算出新值。
    const quint64 oldSum = readChecksum(db, table, streamEpoch);
    const qint64 oldRows = readRowCount(db, table, streamEpoch);

    // M-02 fix: pure unsigned modular arithmetic — no signed overflow UB.
    // 译：M-02 fix —— 全程无符号模运算，无有符号溢出 UB。
    // newSum = oldSum + add − sub 在 quint64 下即“模 2^64 加减”，与解码端、与
    // resetFromBaseline 的全扫累加在数学上等价（这正是模加和可增量维护的根基）。
    const quint64 newSum = oldSum + add - sub;
    const qint64 newRows = oldRows + rowCountDelta;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    // 用 UPSERT（INSERT ... ON CONFLICT DO UPDATE）一条语句完成「有则更新、无则插入」。
    // 冲突键是 (table_name, stream_epoch)——每表每纪元唯一一行表态。
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("INSERT INTO __sync_table_state "
                       "(table_name, stream_epoch, schema_fingerprint, high_water_seq, "
                       " content_checksum, row_count, updated_ms) "
                       "VALUES (?, ?, ?, ?, ?, ?, ?) "
                       "ON CONFLICT(table_name, stream_epoch) DO UPDATE SET "
                       "  schema_fingerprint = excluded.schema_fingerprint, "
                       // high_water_seq 取 MAX：只增不减地推进“已处理来源序号”，
                       // 即便变更乱序到达，也不会让水位回退。
                       "  high_water_seq     = MAX(high_water_seq, excluded.high_water_seq), "
                       "  content_checksum   = excluded.content_checksum, "
                       "  row_count          = excluded.row_count, "
                       "  updated_ms         = excluded.updated_ms"));
    q.addBindValue(table);
    q.addBindValue(streamEpoch);
    q.addBindValue(schemaFp);
    q.addBindValue(highWaterSeq);
    q.addBindValue(QString::number(newSum));  // quint64 → 十进制字符串落盘
    q.addBindValue(newRows);
    q.addBindValue(nowMs);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

quint64 TableStateStore::readChecksum(QSqlDatabase& db, const QString& table, qint64 streamEpoch) {
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("SELECT content_checksum FROM __sync_table_state "
                       "WHERE table_name = ? AND stream_epoch = ?"));
    q.addBindValue(table);
    q.addBindValue(streamEpoch);
    if (q.exec() && q.next()) {
        // 校验和以十进制字符串存储；toULongLong 无损还原回 quint64。
        // ok 防御解析失败（数据被外部破坏成非数字时），失败一律按 0 起算。
        bool ok = false;
        quint64 v = q.value(0).toString().toULongLong(&ok);
        return ok ? v : 0;
    }
    return 0;  // 查无此行（新表）或查询失败 → 起始校验和视为 0
}

qint64 TableStateStore::readRowCount(QSqlDatabase& db, const QString& table, qint64 streamEpoch) {
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("SELECT row_count FROM __sync_table_state "
                       "WHERE table_name = ? AND stream_epoch = ?"));
    q.addBindValue(table);
    q.addBindValue(streamEpoch);
    if (q.exec() && q.next())
        return q.value(0).toLongLong();
    return 0;  // 查无此行（新表）或查询失败 → 起始行数视为 0
}

}  // namespace dbridge::sync
