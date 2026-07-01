#include "Scenario2SnapshotService.h"

#include "dbridge/DataBridge.h"
#include "dbridge/SchemaInfo.h"
#include "dbridge/Types.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QMutexLocker>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>
#include <QVariantMap>

// ============================================================================
// Scenario2SnapshotService.cpp — 快照编解码 + 中心A 快照响应线程 实现
// ============================================================================

using namespace dbridge;
using namespace dbridge::sync;

namespace {
constexpr quint32 kMagic = 0x53324E50u;  // "S2NP"：快照工件魔数（收端据此甄别）
constexpr quint32 kVersion = 1u;         // 编码版本（未来演进时递增）
}  // namespace

// ── 编解码 ────────────────────────────────────────────────────────────────────

QByteArray s2snap::encodeSnapshots(const QList<RemoteTableSnapshot>& snaps) {
    QByteArray out;
    QDataStream ds(&out, QIODevice::WriteOnly);
    ds.setVersion(QDataStream::Qt_5_12);  // 固定版本，保证收发端 QVariant 序列化格式一致
    ds << kMagic << kVersion << static_cast<quint32>(snaps.size());
    for (const RemoteTableSnapshot& s : snaps) {
        ds << s.table << s.meta.schemaFingerprint << s.meta.contentChecksum
           << static_cast<qint64>(s.meta.rowCount);
        ds << static_cast<quint32>(s.rows.size());
        for (const QVariantMap& r : s.rows)
            ds << r;  // QVariantMap（QMap<QString,QVariant>）QDataStream 原生支持
    }
    return out;
}

QList<RemoteTableSnapshot> s2snap::decodeSnapshots(const QByteArray& bytes) {
    QList<RemoteTableSnapshot> snaps;
    QDataStream ds(bytes);
    ds.setVersion(QDataStream::Qt_5_12);
    quint32 magic = 0, version = 0, tableCount = 0;
    ds >> magic >> version >> tableCount;
    if (ds.status() != QDataStream::Ok || magic != kMagic || version != kVersion)
        return snaps;  // 魔数/版本不符或流损坏 → 返回空（调用方视作拉取失败）
    for (quint32 i = 0; i < tableCount; ++i) {
        RemoteTableSnapshot s;
        qint64 rowCount = 0;
        quint32 rows = 0;
        ds >> s.table >> s.meta.schemaFingerprint >> s.meta.contentChecksum >> rowCount;
        s.meta.rowCount = rowCount;
        ds >> rows;
        for (quint32 j = 0; j < rows; ++j) {
            QVariantMap r;
            ds >> r;
            s.rows.append(r);
        }
        if (ds.status() != QDataStream::Ok)
            return {};  // 中途损坏 → 全部作废
        snaps.append(s);
    }
    return snaps;
}

// ── 中心A 侧：读行 + 校验和（供响应线程构造快照）─────────────────────────────

namespace {

// computeChecksum —— 为一张表全部行算一个顺序无关的内容指纹（与原 Scenario2Model 一致）。
QString computeChecksum(const QList<QVariantMap>& rows) {
    quint64 sum = 0;
    for (const QVariantMap& row : rows) {
        quint64 h = 0;
        for (auto it = row.begin(); it != row.end(); ++it)
            h = h * 31 + qHash(it.key()) + qHash(it.value().toString());
        sum += h;
    }
    return QString::number(sum, 16);
}

// readRowsOrdered —— 在给定连接上读某表全部行（按 pk 排序），每行一个 QVariantMap。
QList<QVariantMap> readRowsOrdered(QSqlDatabase& db, const QString& table, const QString& pk) {
    QList<QVariantMap> rows;
    QSqlQuery q(db);
    QString sql = QStringLiteral("SELECT * FROM \"%1\"").arg(table);
    if (!pk.isEmpty())
        sql += QStringLiteral(" ORDER BY \"%1\"").arg(pk);  // 稳定行序，供两栏对齐
    if (q.exec(sql)) {
        while (q.next()) {
            QVariantMap row;
            for (int i = 0; i < q.record().count(); ++i)
                row.insert(q.record().fieldName(i), q.value(i));
            rows.append(row);
        }
    }
    return rows;
}

}  // namespace

// ── CenterSnapshotResponder ───────────────────────────────────────────────────

CenterSnapshotResponder::CenterSnapshotResponder(const QString& centerDbPath,
                                                 const QString& inboxDir, const QString& outboxDir,
                                                 QObject* parent)
    : QThread(parent), centerDbPath_(centerDbPath), inboxDir_(inboxDir), outboxDir_(outboxDir) {
}

void CenterSnapshotResponder::requestStop() {
    stop_.storeRelease(1);
}

// pushLog / takeLog —— A 侧处理日志的写入与抽干（互斥保护，跨线程安全）。
void CenterSnapshotResponder::pushLog(const QString& line) {
    QMutexLocker lock(&logMutex_);
    pendingLog_.append(line);
}
QStringList CenterSnapshotResponder::takeLog() {
    QMutexLocker lock(&logMutex_);
    QStringList out;
    out.swap(pendingLog_);  // 取走并清空
    return out;
}

void CenterSnapshotResponder::run() {
    // ① 打开 A 的 DataBridge（用于 schema 发现：userTables/describeTable —— 需求 #3）。
    DataBridge bridge;
    ConnectionSpec spec;
    spec.sqlitePath = centerDbPath_;
    QString err;
    if (!bridge.open(spec, &err)) {
        qWarning("CenterSnapshotResponder: open bridge failed — %s", qPrintable(err));
        return;
    }

    // ② 另开一条读连接用于 SELECT 行数据（DataBridge 不暴露读行 API）。仅本线程使用。
    const QString rconn =
        QStringLiteral("s2resp_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
    {
        auto rdb = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), rconn);
        rdb.setDatabaseName(centerDbPath_);
        if (!rdb.open()) {
            qWarning("CenterSnapshotResponder: open read conn failed — %s",
                     qPrintable(rdb.lastError().text()));
            QSqlDatabase::removeDatabase(rconn);
            return;
        }

        QDir(inboxDir_).mkpath(QStringLiteral("."));
        QDir(outboxDir_).mkpath(QStringLiteral("."));

        // ③ 轮询主循环：监视 inbox 里的快照请求，逐个应答。
        while (!stop_.loadAcquire()) {
            QDir dir(inboxDir_);
            // 请求到达后由 UDP 传输层落成 "snapreq__<id>.payload" + "…payload.ready" 哨兵。
            const QStringList readyFiles = dir.entryList(
                QStringList{QStringLiteral("snapreq__*.payload.ready")}, QDir::Files, QDir::Name);
            for (const QString& readyName : readyFiles) {
                // 去掉 ".ready"(6) 得 payload 名；从中解析 reqId。
                const QString payloadName = readyName.left(readyName.size() - 6);
                QString reqId = payloadName;
                reqId.remove(QStringLiteral("snapreq__"));
                if (reqId.endsWith(QStringLiteral(".payload")))
                    reqId.chop(8);
                pushLog(QStringLiteral("收到快照请求（reqId=%1）").arg(reqId));

                // 构造快照：发现 A 的用户表 → 每表列/主键 → SELECT 行 → 校验和。
                QList<RemoteTableSnapshot> snaps;
                QString e2;
                const QStringList tables = bridge.userTables(&e2);
                pushLog(QStringLiteral("读本地库：userTables/describeTable 发现 %1 张表 [%2]")
                            .arg(tables.size())
                            .arg(tables.join(QStringLiteral(", "))));
                for (const QString& t : tables) {
                    TableSchema ts;
                    QString pk;
                    if (bridge.describeTable(t, &ts))
                        pk = ts.primaryKeyColumns().isEmpty() ? QString()
                                                              : ts.primaryKeyColumns().first();
                    RemoteTableSnapshot snap;
                    snap.table = t;
                    snap.rows = readRowsOrdered(rdb, t, pk);
                    snap.meta.schemaFingerprint = QStringLiteral("fp_") + t;
                    snap.meta.contentChecksum = computeChecksum(snap.rows);
                    snap.meta.rowCount = snap.rows.size();
                    snaps.append(snap);
                }

                // 序列化并按 outbox 两步协议写响应（先内容后 .ready 哨兵）。
                const QByteArray body = s2snap::encodeSnapshots(snaps);
                // 先把 A 侧处理日志写进缓冲（在写响应工件之前，确保 B 收到响应时已能抽干）。
                pushLog(QStringLiteral("回传快照：%1 张表 / %2 字节（经 UDP 发回子节点B）")
                            .arg(snaps.size())
                            .arg(body.size()));
                const QString respPayload = QStringLiteral("snapresp__%1.payload").arg(reqId);
                const QString respPath = QDir(outboxDir_).filePath(respPayload);
                QFile f(respPath);
                if (f.open(QIODevice::WriteOnly)) {
                    f.write(body);
                    f.flush();
                    f.close();
                    QFile ready(respPath + QStringLiteral(".ready"));
                    ready.open(QIODevice::WriteOnly);
                    ready.close();
                }

                // 标记该请求已处理：改名加 .done，避免下一轮重复应答。
                QFile::rename(dir.filePath(payloadName),
                              dir.filePath(payloadName) + QStringLiteral(".done"));
                QFile::rename(dir.filePath(readyName),
                              dir.filePath(readyName) + QStringLiteral(".done"));
            }
            QThread::msleep(kPollMs);
        }
        rdb.close();
    }
    QSqlDatabase::removeDatabase(rconn);
    // bridge 在此作用域结束时析构（内部自动 close）。
}
