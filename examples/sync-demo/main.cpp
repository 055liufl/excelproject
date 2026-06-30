/**
 * sync-demo — dbridge SQLite 1 中心节点 + 3 边缘节点完整同步流程演示（UDP 传输层）
 *
 * 拓扑：center（rank=100）← → edge_b（rank=70）
 *                          ← → edge_c（rank=50）
 *                          ← → edge_d（rank=30）
 *
 * UDP 端口分配：
 *   center  监听 15001，向 edge_b:15002 / edge_c:15003 / edge_d:15004 发送
 *   edge_b  监听 15002，向 center:15001 发送
 *   edge_c  监听 15003，向 center:15001 发送
 *   edge_d  监听 15004，向 center:15001 发送
 *
 * center 使用多 peer UdpFileTransport，从 artifact 文件名中提取目标 peer 后路由发送。
 * 每个 edge 使用单 peer UdpFileTransport，直接发往 center。
 *
 * 完整同步阶段（无任何简化）：
 *   Phase 1 — 下行初始同步：center 写入基线数据 → broadcast → 3 个空白 edge 从零追齐
 *   Phase 2 — 上行增量 + 中心转发：edge_b 修改 → center 应用 → center 转发给 edge_c/edge_d
 *   Phase 3 — 下行增量广播：center 新增员工 → 3 个 edge 自动应用
 *   Phase 4 — 选择推送 + 转发：edge_c syncSelected() → center → center 转发给 edge_b/edge_d
 *   Phase 5 — 冲突解决：edge_d 写 28000，center 写 30000（rank=100 胜出），四端收敛
 *
 * 运行方式：
 *   ./sync-demo <workspace-dir>     （必须使用全新目录）
 */

#include "dbridge/DataBridge.h"
#include "dbridge/Errors.h"
#include "dbridge/sync/ISyncEngine.h"
#include "dbridge/sync/SyncConfig.h"
#include "dbridge/sync/SyncSelection.h"
#include "dbridge/sync/SyncTypes.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QString>
#include <QThread>

#include "udp_transport.h"
#include <iostream>

// ── 常量 ──────────────────────────────────────────────────────────────────────

// 各节点优先级（rank 必须全局唯一，冲突仲裁时高 rank 胜出）
static constexpr int RANK_CENTER = 100;
static constexpr int RANK_EDGE_B = 70;
static constexpr int RANK_EDGE_C = 50;
static constexpr int RANK_EDGE_D = 30;

// UDP 端口分配
static constexpr quint16 PORT_CENTER = 15001;
static constexpr quint16 PORT_EDGE_B = 15002;
static constexpr quint16 PORT_EDGE_C = 15003;
static constexpr quint16 PORT_EDGE_D = 15004;

// ── 工具宏 / 函数 ─────────────────────────────────────────────────────────────
#define LOG(node, msg) std::cout << "[" << (node) << "] " << (msg) << std::endl

static void printSyncResult(const std::string& tag, const dbridge::sync::SyncResult& r) {
    std::cout << "\n  ── " << tag << " SyncResult ──\n"
              << "     ok=" << (r.ok ? "true" : "false") << "  payloadsSent=" << r.payloadsSent
              << "  payloadsApplied=" << r.payloadsApplied
              << "  changesApplied=" << r.changesApplied << "  conflicts=" << r.conflicts << "\n";
    for (const auto& e : r.errors)
        std::cout << "     [ERR] " << e.code.toStdString() << " " << e.message.toStdString()
                  << "\n";
}

static void printLogs(const std::string& node, const QList<dbridge::sync::SyncLogEntry>& logs) {
    for (const auto& e : logs) {
        const char* sev = e.severity == dbridge::sync::Severity::Warning ? "WARN"
                          : e.severity == dbridge::sync::Severity::Error ? "ERR "
                          : e.severity == dbridge::sync::Severity::Fatal ? "FATL"
                                                                         : "INFO";
        std::cout << "  [" << node << "|" << sev << "] [" << e.phase.toStdString() << "] "
                  << e.message.toStdString() << "\n";
    }
}

// 建库建表（4 节点共用同一 schema；仅建表，不插行）
static bool setupDatabase(const QString& dbPath) {
    const QString conn = QStringLiteral("setup_") + dbPath;
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        if (!db.open()) {
            std::cerr << "Cannot open " << dbPath.toStdString() << "\n";
            return false;
        }
        QSqlQuery q(db);
        q.exec(
            QStringLiteral("CREATE TABLE IF NOT EXISTS employees ("
                           "  id      INTEGER PRIMARY KEY,"
                           "  name    TEXT    NOT NULL,"
                           "  dept    TEXT    NOT NULL,"
                           "  salary  INTEGER NOT NULL)"));
        q.exec(
            QStringLiteral("CREATE TABLE IF NOT EXISTS assignments ("
                           "  id          INTEGER PRIMARY KEY,"
                           "  employee_id INTEGER NOT NULL REFERENCES employees(id),"
                           "  project     TEXT    NOT NULL,"
                           "  role        TEXT    NOT NULL)"));
        db.close();
    }
    QSqlDatabase::removeDatabase(conn);
    return true;
}

// 查询并打印一张表
static void dumpTable(const QString& dbPath, const QString& table, const std::string& label) {
    const QString conn = QStringLiteral("dump_") + dbPath + table +
                         QString::number(QDateTime::currentMSecsSinceEpoch());
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        db.open();
        QSqlQuery q(db);
        q.exec(QStringLiteral("SELECT * FROM ") + table);
        std::cout << "\n  [" << label << " / " << table.toStdString() << "]\n";
        while (q.next()) {
            std::cout << "    |";
            for (int i = 0; i < q.record().count(); ++i)
                std::cout << " " << q.value(i).toString().toStdString() << " |";
            std::cout << "\n";
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(conn);
}

// 打印四节点同一张表
static void dump4(const QString& cDb, const QString& bDb, const QString& cCDb, const QString& dDb,
                  const QString& table) {
    dumpTable(cDb, table, "center");
    dumpTable(bDb, table, "edge_b");
    dumpTable(cCDb, table, "edge_c");
    dumpTable(dDb, table, "edge_d");
}

// 轮询引擎状态直到终止或超时
static bool waitForEngine(dbridge::sync::ISyncEngine* engine, const char* name,
                          int timeoutMs = 12000) {
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        auto st = engine->state();
        if (st == dbridge::sync::SyncState::Completed || st == dbridge::sync::SyncState::Idle ||
            st == dbridge::sync::SyncState::Failed)
            return st != dbridge::sync::SyncState::Failed;
        QThread::msleep(100);
    }
    std::cerr << "waitForEngine timeout: " << name << "\n";
    return false;
}

// RowMutation 辅助（employees 表）
static dbridge::sync::RowMutation empMut(int id, const QString& name, const QString& dept,
                                         int salary) {
    dbridge::sync::RowMutation m;
    m.table = QStringLiteral("employees");
    m.columns = QStringList{QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("dept"),
                            QStringLiteral("salary")};
    m.values = QVariantList{id, name, dept, salary};
    m.pkColumns = QStringList{QStringLiteral("id")};
    m.mode = dbridge::sync::UpsertMode::DoUpdate;
    return m;
}

// RowMutation 辅助（assignments 表）
static dbridge::sync::RowMutation asnMut(int id, int empId, const QString& project,
                                         const QString& role) {
    dbridge::sync::RowMutation m;
    m.table = QStringLiteral("assignments");
    m.columns = QStringList{QStringLiteral("id"), QStringLiteral("employee_id"),
                            QStringLiteral("project"), QStringLiteral("role")};
    m.values = QVariantList{id, empId, project, role};
    m.pkColumns = QStringList{QStringLiteral("id")};
    m.mode = dbridge::sync::UpsertMode::DoUpdate;
    return m;
}

// 构建节点 SyncConfig（所有节点共享同一套 rank 表）
static dbridge::sync::SyncConfig buildConfig(const QString& nodeId, dbridge::sync::NodeRole role,
                                             const QString& centerNodeId, const QStringList& peers,
                                             const QString& dbPath, const QString& ws,
                                             QString* err) {
    auto builder = dbridge::sync::SyncConfig::Builder()
                       .nodeId(nodeId)
                       .role(role)
                       .database(dbPath)
                       .outboxDir(ws + "/" + nodeId + "/outbox")
                       .inboxDir(ws + "/" + nodeId + "/inbox")
                       .quarantineDir(ws + "/" + nodeId + "/quarantine")
                       .conflictPolicy(dbridge::sync::ConflictPolicy::SourceWins)
                       .originPriority(QStringLiteral("center"), RANK_CENTER)
                       .originPriority(QStringLiteral("edge_b"), RANK_EDGE_B)
                       .originPriority(QStringLiteral("edge_c"), RANK_EDGE_C)
                       .originPriority(QStringLiteral("edge_d"), RANK_EDGE_D)
                       .broadcastIntervalMs(300)
                       .ackMaxDelayMs(10000);

    if (!centerNodeId.isEmpty())
        builder.centerNodeId(centerNodeId);

    for (const QString& p : peers)
        builder.addPeerNode(p);

    return builder.build(err);
}

// ──────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    // Qt 5.12：将 applicationDirPath() 提到 libraryPaths 首位，让 session-enabled
    // QSQLITE 插件（sqldrivers/libqsqlite.so）优先于系统预编译版本加载。
    {
        QStringList paths;
        paths.append(app.applicationDirPath());
        for (const QString& p : QCoreApplication::libraryPaths())
            if (!paths.contains(p))
                paths.append(p);
        QCoreApplication::setLibraryPaths(paths);
    }

    // ── 0. 解析工作目录 ──────────────────────────────────────────────────────
    if (argc < 2) {
        std::cerr << "Usage: sync-demo <workspace-dir>   (必须使用全新目录)\n";
        return 1;
    }
    const QString ws = QString::fromLocal8Bit(argv[1]);

    for (const QString& node : {QStringLiteral("center"), QStringLiteral("edge_b"),
                                QStringLiteral("edge_c"), QStringLiteral("edge_d")}) {
        for (const QString& sub :
             {QStringLiteral("outbox"), QStringLiteral("inbox"), QStringLiteral("quarantine")})
            QDir().mkpath(ws + "/" + node + "/" + sub);
    }

    const QString centerDb = ws + "/center.db";
    const QString edgeBDb = ws + "/edge_b.db";
    const QString edgeCDb = ws + "/edge_c.db";
    const QString edgeDDb = ws + "/edge_d.db";

    std::cout << "=================================================\n"
              << " sync-demo: 1 中心节点 + 3 边缘节点完整同步（UDP）\n"
              << "=================================================\n"
              << "  workspace : " << ws.toStdString() << "\n"
              << "  center.db : " << centerDb.toStdString() << "\n"
              << "  edge_b.db : " << edgeBDb.toStdString() << "\n"
              << "  edge_c.db : " << edgeCDb.toStdString() << "\n"
              << "  edge_d.db : " << edgeDDb.toStdString() << "\n\n";

    // ── 1. 建库建表 ──────────────────────────────────────────────────────────
    // 四端均只建 schema（CREATE TABLE），不插任何行——模拟全新节点冷接入。
    LOG("SETUP", "建库建表（四端 schema 对齐，edge 节点无任何行数据）...");
    for (const QString& db : {centerDb, edgeBDb, edgeCDb, edgeDDb}) {
        if (!setupDatabase(db))
            return 1;
    }
    LOG("SETUP", "schema 建立完成");

    // ── 2. 打开 DataBridge ───────────────────────────────────────────────────
    dbridge::DataBridge centerBridge, edgeBBridge, edgeCBridge, edgeDBridge;
    QString err;

    auto openBridge = [&](dbridge::DataBridge& bridge, const QString& path,
                          const char* name) -> bool {
        dbridge::ConnectionSpec spec;
        spec.sqlitePath = path;
        spec.enableWal = true;
        if (!bridge.open(spec, &err)) {
            std::cerr << name << " DataBridge open failed: " << err.toStdString() << "\n";
            return false;
        }
        return true;
    };

    if (!openBridge(centerBridge, centerDb, "center") ||
        !openBridge(edgeBBridge, edgeBDb, "edge_b") ||
        !openBridge(edgeCBridge, edgeCDb, "edge_c") || !openBridge(edgeDBridge, edgeDDb, "edge_d"))
        return 1;
    LOG("SETUP", "DataBridge 已打开（4 节点）");

    // ── 3. 构建 SyncConfig ───────────────────────────────────────────────────
    auto centerCfg =
        buildConfig(QStringLiteral("center"), dbridge::sync::NodeRole::Center, {},
                    {QStringLiteral("edge_b"), QStringLiteral("edge_c"), QStringLiteral("edge_d")},
                    centerDb, ws, &err);
    if (!centerCfg.isValid()) {
        std::cerr << "center SyncConfig failed: " << err.toStdString() << "\n";
        return 1;
    }

    auto edgeBCfg =
        buildConfig(QStringLiteral("edge_b"), dbridge::sync::NodeRole::Edge,
                    QStringLiteral("center"), {QStringLiteral("center")}, edgeBDb, ws, &err);
    if (!edgeBCfg.isValid()) {
        std::cerr << "edge_b SyncConfig failed: " << err.toStdString() << "\n";
        return 1;
    }

    auto edgeCCfg =
        buildConfig(QStringLiteral("edge_c"), dbridge::sync::NodeRole::Edge,
                    QStringLiteral("center"), {QStringLiteral("center")}, edgeCDb, ws, &err);
    if (!edgeCCfg.isValid()) {
        std::cerr << "edge_c SyncConfig failed: " << err.toStdString() << "\n";
        return 1;
    }

    auto edgeDCfg =
        buildConfig(QStringLiteral("edge_d"), dbridge::sync::NodeRole::Edge,
                    QStringLiteral("center"), {QStringLiteral("center")}, edgeDDb, ws, &err);
    if (!edgeDCfg.isValid()) {
        std::cerr << "edge_d SyncConfig failed: " << err.toStdString() << "\n";
        return 1;
    }
    LOG("SETUP", "SyncConfig 构建完成（center 3 peers / 每 edge 1 peer）");

    // ── 4. 创建并初始化同步引擎 ──────────────────────────────────────────────
    auto centerEngine = dbridge::sync::createSyncEngine(centerBridge);
    auto edgeBEngine = dbridge::sync::createSyncEngine(edgeBBridge);
    auto edgeCEngine = dbridge::sync::createSyncEngine(edgeCBridge);
    auto edgeDEngine = dbridge::sync::createSyncEngine(edgeDBridge);

    for (auto& [engine, cfg, name] :
         std::initializer_list<std::tuple<dbridge::sync::ISyncEngine*,
                                          const dbridge::sync::SyncConfig*, const char*>>{
             {centerEngine.get(), &centerCfg, "center"},
             {edgeBEngine.get(), &edgeBCfg, "edge_b"},
             {edgeCEngine.get(), &edgeCCfg, "edge_c"},
             {edgeDEngine.get(), &edgeDCfg, "edge_d"},
         }) {
        if (!engine->initialize(*cfg, &err)) {
            std::cerr << name << " initialize failed: " << err.toStdString() << "\n";
            return 1;
        }
        LOG(name, "同步引擎初始化成功");
    }

    // ── 5. 启动 UDP 传输层 ───────────────────────────────────────────────────
    //
    //  center 使用多 peer 传输：单线程绑定 15001，从 artifact 文件名提取目标 peer 后路由发送。
    //  每个 edge 使用单 peer 传输：直接发往 center:15001。
    //
    //    center:15001 ←─ edge_b:15002
    //                 ←─ edge_c:15003
    //                 ←─ edge_d:15004
    //    center:15001 ─→ edge_b:15002
    //                 ─→ edge_c:15003
    //                 ─→ edge_d:15004

    std::cout << "\n--- 启动 UDP 传输层 ---\n";
    const QHostAddress loopback(QHostAddress::LocalHost);

    // center: 多 peer 传输（一个线程覆盖全部 3 个 edge 方向）
    UdpFileTransport centerTransport(PORT_CENTER,
                                     QHash<QString, UdpPeerEndpoint>{
                                         {QStringLiteral("edge_b"), {loopback, PORT_EDGE_B}},
                                         {QStringLiteral("edge_c"), {loopback, PORT_EDGE_C}},
                                         {QStringLiteral("edge_d"), {loopback, PORT_EDGE_D}},
                                     },
                                     ws + "/center/outbox", ws + "/center/inbox");

    // 每个 edge：单 peer 传输（直接发往 center）
    UdpFileTransport edgeBTransport(PORT_EDGE_B, loopback, PORT_CENTER, ws + "/edge_b/outbox",
                                    ws + "/edge_b/inbox");
    UdpFileTransport edgeCTransport(PORT_EDGE_C, loopback, PORT_CENTER, ws + "/edge_c/outbox",
                                    ws + "/edge_c/inbox");
    UdpFileTransport edgeDTransport(PORT_EDGE_D, loopback, PORT_CENTER, ws + "/edge_d/outbox",
                                    ws + "/edge_d/inbox");

    centerTransport.start();
    edgeBTransport.start();
    edgeCTransport.start();
    edgeDTransport.start();
    QThread::msleep(120);  // 等待所有 socket bind 完成
    LOG("UDP", "center 侦听 :15001  edge_b :15002  edge_c :15003  edge_d :15004  传输层就绪");

    // ════════════════════════════════════════════════════════════════════════
    // Phase 1: 下行初始同步（Center 基线 → 3 个空白 Edge）
    //
    // center 通过 ISyncEngine::write() 写入全部基线数据（session 捕获，进入 changelog）。
    // sync() 将 changelog 打包为 changeset artifact，SyncWorker 的 broadcastTopeer()
    // 为每个 edge 生成独立 artifact（文件名含目标 peer 名）并写入 center/outbox。
    // center 的多 peer 传输从文件名提取目标后分别发往 edge_b:15002、edge_c:15003、
    // edge_d:15004。三个 edge 各自应用，回送 ACK。center 的 pendingAckWindow_ 要求
    // 全部三个 edge 的 ACK 到齐后方才 Completed。
    // ════════════════════════════════════════════════════════════════════════
    std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
              << " Phase 1: 下行初始同步（Center 基线 → 3 个空白 Edge）\n"
              << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    // employees 先写，保证 FK 顺序
    if (!centerEngine->write({empMut(1, QStringLiteral("张三"), QStringLiteral("研发"), 20000),
                              empMut(2, QStringLiteral("李四"), QStringLiteral("产品"), 18000),
                              empMut(3, QStringLiteral("王五"), QStringLiteral("研发"), 22000)},
                             &err)) {
        std::cerr << "center write employees failed: " << err.toStdString() << "\n";
        return 1;
    }
    LOG("center", "[1-a] 写入员工基线（id 1-3）→ changelog");

    // assignments 后写
    if (!centerEngine->write({asnMut(1, 1, QStringLiteral("ProjectA"), QStringLiteral("Lead")),
                              asnMut(2, 2, QStringLiteral("ProjectA"), QStringLiteral("Member")),
                              asnMut(3, 3, QStringLiteral("ProjectB"), QStringLiteral("Lead"))},
                             &err)) {
        std::cerr << "center write assignments failed: " << err.toStdString() << "\n";
        return 1;
    }
    LOG("center", "[1-b] 写入分配基线（id 1-3）→ changelog");

    // sync() 广播到全部 3 个 edge，等待三方 ACK
    std::cout << "\n  [center] sync() → UDP 广播至 edge_b/edge_c/edge_d ...\n";
    if (!centerEngine->sync(&err)) {
        std::cerr << "center sync failed: " << err.toStdString() << "\n";
        return 1;
    }
    if (!waitForEngine(centerEngine.get(), "center Phase 1")) {
        std::cerr << "center Phase 1 超时\n";
        return 1;
    }
    printSyncResult("center Phase 1", centerEngine->result());

    QThread::msleep(700);
    printLogs("edge_b", edgeBEngine->logs());
    printLogs("edge_c", edgeCEngine->logs());

    std::cout << "\n===== Phase 1 完成：四端数据应完全一致 =====\n";
    dump4(centerDb, edgeBDb, edgeCDb, edgeDDb, QStringLiteral("employees"));
    dump4(centerDb, edgeBDb, edgeCDb, edgeDDb, QStringLiteral("assignments"));

    // ════════════════════════════════════════════════════════════════════════
    // Phase 2: 上行增量同步 + 中心转发（Edge_b → Center → Edge_c / Edge_d）
    //
    // edge_b 修改王五薪资并新增员工赵六，sync() 推送到 center。
    // center 应用 edge_b 的 changeset 后，再次 sync()——SyncWorker 将 edge_b 的变更
    // 以 origin=edge_b 转发给 edge_c 和 edge_d（anti-echo 阻止回送 edge_b）。
    // center 的 pendingAckWindow_ 仅包含 edge_c 和 edge_d 的 ACK 条件；
    // 两者均 ACK 后 center Completed，确认转发完成。
    // ════════════════════════════════════════════════════════════════════════
    std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
              << " Phase 2: 上行增量 + 中心转发（Edge_b → Center → Edge_c/Edge_d）\n"
              << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    if (!edgeBEngine->write({empMut(3, QStringLiteral("王五"), QStringLiteral("研发"), 25000),
                             empMut(4, QStringLiteral("赵六"), QStringLiteral("市场"), 16000)},
                            &err)) {
        std::cerr << "edge_b write failed: " << err.toStdString() << "\n";
        return 1;
    }
    LOG("edge_b", "[2-a] 写入：王五 22000→25000，新增赵六（市场）→ changelog");

    // 2-b: edge_b → center（上行）
    std::cout << "\n  [edge_b] sync() → UDP 发往 center ...\n";
    if (!edgeBEngine->sync(&err)) {
        std::cerr << "edge_b sync failed: " << err.toStdString() << "\n";
        return 1;
    }
    if (!waitForEngine(edgeBEngine.get(), "edge_b Phase 2")) {
        std::cerr << "edge_b Phase 2 超时\n";
    }
    printSyncResult("edge_b Phase 2", edgeBEngine->result());
    QThread::msleep(400);

    // 2-c: center 将 edge_b 的变更转发给 edge_c 和 edge_d
    std::cout << "\n  [center] sync() → 转发 edge_b 变更至 edge_c/edge_d ...\n";
    if (!centerEngine->sync(&err)) {
        std::cerr << "center forward sync failed: " << err.toStdString() << "\n";
        return 1;
    }
    if (!waitForEngine(centerEngine.get(), "center Phase 2 forward")) {
        std::cerr << "center Phase 2 转发超时\n";
    }
    printSyncResult("center Phase 2 转发", centerEngine->result());
    QThread::msleep(600);
    printLogs("center", centerEngine->logs());

    std::cout << "\n===== Phase 2 完成：四端 employees 应一致 =====\n";
    dump4(centerDb, edgeBDb, edgeCDb, edgeDDb, QStringLiteral("employees"));

    // ════════════════════════════════════════════════════════════════════════
    // Phase 3: 下行增量广播（Center → 所有 Edge）
    //
    // center 新增员工钱七，sync() 广播给全部 3 个 edge。
    // 这条路径与 Phase 1 相同，但演示的是增量场景（现有数据基础上的新增）。
    // ════════════════════════════════════════════════════════════════════════
    std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
              << " Phase 3: 下行增量广播（Center → 所有 Edge）\n"
              << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    if (!centerEngine->write({empMut(5, QStringLiteral("钱七"), QStringLiteral("运营"), 17000)},
                             &err)) {
        std::cerr << "center write failed: " << err.toStdString() << "\n";
        return 1;
    }
    LOG("center", "[3-a] 写入：新员工钱七（运营，id=5）→ changelog");

    std::cout << "\n  [center] sync() → UDP 广播至 edge_b/edge_c/edge_d ...\n";
    if (!centerEngine->sync(&err)) {
        std::cerr << "center sync failed: " << err.toStdString() << "\n";
        return 1;
    }
    if (!waitForEngine(centerEngine.get(), "center Phase 3")) {
        std::cerr << "center Phase 3 超时\n";
    }
    printSyncResult("center Phase 3", centerEngine->result());
    QThread::msleep(600);
    printLogs("edge_b", edgeBEngine->logs());

    std::cout << "\n===== Phase 3 完成：四端 employees 应一致（含钱七）=====\n";
    dump4(centerDb, edgeBDb, edgeCDb, edgeDDb, QStringLiteral("employees"));

    // ════════════════════════════════════════════════════════════════════════
    // Phase 4: 选择性推送 + 中心转发（Edge_c syncSelected → Center → Edge_b/Edge_d）
    //
    // edge_c 为赵六新建分配记录（assignments.id=4），通过 syncSelected() 精确推送，
    // FK 闭包自动补全 employees.id=4（赵六），并对 center 已有的一致父行执行剪枝。
    //
    // center 收到 SelectionPush artifact，应用后以 origin=center 写入 changelog（seq=K）。
    // center 再次 sync()，以 changeset 形式把 seq=K 转发给 edge_b 和 edge_d，
    // 两者 ACK 后 center Completed，确认转发完成。
    //
    // 完整路径：
    //   edge_c.syncSelected() → SelectionPush artifact → UDP → center.inbox
    //   → processSelectionPushArtifact() → branchBC(InboundSelectionPush)
    //   → center.changelog(origin=center, seq=K) → center.sync()
    //   → changeset artifact for edge_b/edge_d → UDP → edge_b/edge_d.inbox
    //   → branchA(InboundChangeset) → ACK → center Completed
    // ════════════════════════════════════════════════════════════════════════
    std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
              << " Phase 4: 选择性推送 + 转发（Edge_c → Center → Edge_b/Edge_d）\n"
              << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    // 4-a: 写入赵六的分配记录
    if (!edgeCEngine->write({asnMut(4, 4, QStringLiteral("ProjectC"), QStringLiteral("Member"))},
                            &err)) {
        std::cerr << "edge_c write assignment failed: " << err.toStdString() << "\n";
        return 1;
    }
    LOG("edge_c", "[4-a] 写入：赵六 → ProjectC（assignments.id=4）→ changelog");

    // 4-b: 构建精确选择集，FK 闭包自动补全 employees.id=4
    dbridge::sync::SyncSelection sel =
        dbridge::sync::SyncSelection::Builder()
            .addRecord(QStringLiteral("assignments"), QStringLiteral("4"))
            .includeFkDependencies(true)
            .pruneConsistentDependencies(true)
            .build(&err);
    if (sel.isEmpty()) {
        std::cerr << "SyncSelection build failed: " << err.toStdString() << "\n";
        return 1;
    }

    // 4-c: syncSelected → center 应用并 ACK
    std::cout << "\n  [edge_c] syncSelected(assignments.id=4, FK deps=true) ...\n";
    if (!edgeCEngine->syncSelected(sel, &err)) {
        std::cerr << "syncSelected rejected: " << err.toStdString() << "\n";
        return 1;
    }
    LOG("edge_c", "[4-b] syncSelected() 已提交，等待 center ACK ...");
    if (!waitForEngine(edgeCEngine.get(), "edge_c Phase 4")) {
        std::cerr << "edge_c Phase 4 超时\n";
    }
    printSyncResult("edge_c Phase 4 syncSelected", edgeCEngine->result());
    QThread::msleep(400);

    // 4-d: center 转发 seq=K 给 edge_b 和 edge_d
    std::cout << "\n  [center] sync() → 转发 edge_c 推送至 edge_b/edge_d ...\n";
    if (!centerEngine->sync(&err)) {
        std::cerr << "center forward sync failed: " << err.toStdString() << "\n";
        return 1;
    }
    if (!waitForEngine(centerEngine.get(), "center Phase 4 forward")) {
        std::cerr << "center Phase 4 转发超时\n";
    }
    printSyncResult("center Phase 4 转发", centerEngine->result());
    QThread::msleep(600);
    printLogs("center", centerEngine->logs());

    std::cout << "\n===== Phase 4 完成：四端 assignments 应一致 =====\n";
    dump4(centerDb, edgeBDb, edgeCDb, edgeDDb, QStringLiteral("assignments"));

    // ════════════════════════════════════════════════════════════════════════
    // Phase 5: 冲突解决（SourceWins，center rank=100 胜出）
    //
    // edge_d（rank=30）写张三薪资 28000 并广播 → center 应用（winner←{edge_d, rank=30}）
    // center（rank=100）写张三薪资 30000 并广播 → 三个 edge 各自解决冲突：
    //   edge_b/edge_c：当前=20000，center 的 changeset old=28000，DATA 冲突触发仲裁：
    //     incumbent={center,100,seq_1}（Phase1 写入时的 winner），challenger={center,100,seq_K}
    //     同 origin 且 seq_K > seq_1 → challenger 胜 → 应用 30000 ✅
    //   edge_d：当前=28000，center 的 changeset old=28000，无冲突 → 直接应用 30000 ✅
    // 四端最终均收敛到 张三=30000。
    // ════════════════════════════════════════════════════════════════════════
    std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
              << " Phase 5: 冲突解决（SourceWins, center rank=100）\n"
              << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    // 先刷清双端 changelog，避免历史变更干扰冲突检测
    std::cout << "\n  [pre-flush] 清空四端待广播 changelog ...\n";
    for (auto& [eng, tag] :
         std::initializer_list<std::pair<dbridge::sync::ISyncEngine*, const char*>>{
             {edgeDEngine.get(), "edge_d"},
             {edgeBEngine.get(), "edge_b"},
             {edgeCEngine.get(), "edge_c"},
             {centerEngine.get(), "center"},
         }) {
        eng->sync(&err);
        waitForEngine(eng, tag);
    }
    QThread::msleep(600);

    // 5-a: edge_d 写 28000 并广播 → center 应用，建立 winner={edge_d, rank=30}
    if (!edgeDEngine->write({empMut(1, QStringLiteral("张三"), QStringLiteral("研发"), 28000)},
                            &err))
        std::cerr << "edge_d write failed: " << err.toStdString() << "\n";
    LOG("edge_d", "[5-a] 写入：张三薪资 → 28000");

    edgeDEngine->sync(&err);
    if (!waitForEngine(edgeDEngine.get(), "edge_d 广播 28000"))
        std::cerr << "edge_d 广播 28000 超时\n";
    QThread::msleep(900);  // 等 center 应用并写 winner={edge_d, rank=30}
    LOG("center", "[5-a] 应已收到 edge_d 的 28000，winner={edge_d, rank=30}");

    // 5-b: center 写 30000 并广播给全部三个 edge（rank=100 胜出）
    if (!centerEngine->write({empMut(1, QStringLiteral("张三"), QStringLiteral("研发"), 30000)},
                             &err))
        std::cerr << "center write failed: " << err.toStdString() << "\n";
    LOG("center", "[5-b] 写入：张三薪资 → 30000（rank=100，将覆盖全部 edge 的 28000）");

    std::cout << "\n  [center] sync() → 广播 30000 至 edge_b/edge_c/edge_d ...\n";
    centerEngine->sync(&err);
    if (!waitForEngine(centerEngine.get(), "center 广播 30000"))
        std::cerr << "center 广播 30000 超时\n";
    printSyncResult("center Phase 5", centerEngine->result());

    QThread::msleep(1000);  // 等三个 edge 的自动广播周期处理完毕

    LOG("", "[5] 冲突解决完毕：期望四端 张三薪资 均为 30000（center rank=100 胜出）");
    std::cout << "\n===== Phase 5 完成：冲突解决后四端数据状态 =====\n";
    dump4(centerDb, edgeBDb, edgeCDb, edgeDDb, QStringLiteral("employees"));

    // ── 最终同步日志汇总 ─────────────────────────────────────────────────────
    std::cout << "\n===== 最终同步日志 =====\n";
    printLogs("center", centerEngine->logs());
    printLogs("edge_b", edgeBEngine->logs());
    printLogs("edge_c", edgeCEngine->logs());
    printLogs("edge_d", edgeDEngine->logs());

    for (auto& [eng, tag] :
         std::initializer_list<std::pair<dbridge::sync::ISyncEngine*, const char*>>{
             {centerEngine.get(), "center"},
             {edgeBEngine.get(), "edge_b"},
             {edgeCEngine.get(), "edge_c"},
             {edgeDEngine.get(), "edge_d"},
         }) {
        const auto errs = eng->errors();
        if (!errs.isEmpty()) {
            std::cout << "\n[WARNING] " << tag << " 存在错误记录：\n";
            for (const auto& e : errs)
                std::cout << "  " << e.code.toStdString() << " " << e.message.toStdString() << "\n";
        }
    }

    // ── 清理 ─────────────────────────────────────────────────────────────────
    centerEngine.reset();
    edgeBEngine.reset();
    edgeCEngine.reset();
    edgeDEngine.reset();
    centerBridge.close();
    edgeBBridge.close();
    edgeCBridge.close();
    edgeDBridge.close();

    centerTransport.requestStop();
    edgeBTransport.requestStop();
    edgeCTransport.requestStop();
    edgeDTransport.requestStop();
    centerTransport.wait();
    edgeBTransport.wait();
    edgeCTransport.wait();
    edgeDTransport.wait();

    std::cout << "\n=================================================\n"
              << " sync-demo 完成（1 center + 3 edges）\n"
              << "=================================================\n";
    return 0;
}
