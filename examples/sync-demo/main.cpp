/**
 * sync-demo — dbridge SQLite 多节点完整同步流程演示（UDP 传输层）
 *
 * 无任何简化：完整覆盖从「全新边缘节点冷接入」到「双向增量同步、选择性推送、冲突解决」
 * 的全套同步协议，传输层使用真实 UDP Socket（环回地址）。
 *
 * 节点拓扑：
 *   center（中心节点，port 15001）  ←→  edge_b（边缘节点，port 15002）
 *
 * 完整同步阶段：
 *   Phase 1 — 下行初始同步
 *             center 通过 ISyncEngine::write() 写入基线数据（session 捕获，进入 changelog），
 *             sync() 广播到全空的 edge_b；edge_b 通过 changeset 从零追齐 center。
 *             这是"新边缘节点冷接入"的完整协议路径，无任何数据预填简化。
 *
 *   Phase 2 — 上行增量同步（Edge_b → Center）
 *             edge_b 本地写入变更，sync() 打包发往 center，center 应用后回送 ACK。
 *
 *   Phase 3 — 下行增量同步（Center → Edge_b）
 *             center 主动写入新员工，sync() 广播；edge_b worker 自动扫描 inbox 并应用，
 *             演示 center 主动下推的完整路径。
 *
 *   Phase 4 — 选择性推送（Edge_b syncSelected → Center）
 *             syncSelected() + FK 闭包补全 + 一致性剪枝，UDP 全程自动传输 ACK。
 *
 *   Phase 5 — 冲突解决（SourceWins，center rank=100）
 *             两端先后写同一行，RowWinnerStore 仲裁，高优先级一方最终在两端胜出。
 *
 * 运行方式：
 *   ./sync-demo <workspace-dir>
 *
 * workspace-dir 自动创建：
 *   center.db / edge_b.db
 *   center/{outbox,inbox,quarantine}
 *   edge_b/{outbox,inbox,quarantine}
 *
 * 每次运行请使用全新 workspace 以获得干净的冷启动效果。
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

// ── 工具宏 ────────────────────────────────────────────────────────────────────
#define LOG(node, msg) std::cout << "[" << (node) << "] " << (msg) << std::endl

static void printSyncResult(const std::string& tag, const dbridge::sync::SyncResult& r) {
    std::cout << "\n  ── " << tag << " SyncResult ──\n"
              << "     ok=" << (r.ok ? "true" : "false") << "  payloadsSent=" << r.payloadsSent
              << "  payloadsApplied=" << r.payloadsApplied
              << "  changesApplied=" << r.changesApplied << "  conflicts=" << r.conflicts << "\n";
    for (const auto& e : r.errors)
        std::cout << "     [ERR] code=" << e.code.toStdString()
                  << " phase=" << e.phase.toStdString() << " msg=" << e.message.toStdString()
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

// 建库建表（center / edge_b 共用同一 schema；仅建表，不插行）
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

// 轮询引擎状态直到终止（Completed / Idle / Failed）或超时
static bool waitForEngine(dbridge::sync::ISyncEngine* engine, const char* name,
                          int timeoutMs = 8000) {
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

// 构造 RowMutation 辅助（employees 表：4 列）
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

// 构造 RowMutation 辅助（assignments 表：4 列）
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

// ──────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    // Qt 5.12 把 applicationDirPath() 追加到 libraryPaths 末尾，导致系统 QSQLITE
    // （无 session 支持）优先加载。用 setLibraryPaths 把应用目录提到首位，让
    // sqldrivers/libqsqlite.so（session-enabled）赢得插件竞争。
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
        std::cerr << "Usage: sync-demo <workspace-dir>\n";
        return 1;
    }
    const QString ws = QString::fromLocal8Bit(argv[1]);
    for (const QString& sub : {QStringLiteral("center/outbox"), QStringLiteral("center/inbox"),
                               QStringLiteral("center/quarantine"), QStringLiteral("edge_b/outbox"),
                               QStringLiteral("edge_b/inbox"), QStringLiteral("edge_b/quarantine")})
        QDir().mkpath(ws + "/" + sub);

    const QString centerDb = ws + "/center.db";
    const QString edgeBDb = ws + "/edge_b.db";

    std::cout << "=================================================\n"
              << " sync-demo: dbridge 完整多节点同步流程（UDP）\n"
              << "=================================================\n"
              << "  workspace : " << ws.toStdString() << "\n"
              << "  center.db : " << centerDb.toStdString() << "\n"
              << "  edge_b.db : " << edgeBDb.toStdString() << "\n\n";

    // ── 1. 建库建表 ──────────────────────────────────────────────────────────
    // 双端均只建 schema（CREATE TABLE）；edge_b 不写任何行，
    // 模拟「全新边缘节点冷接入」的真实场景。
    LOG("SETUP", "建库建表（双端 schema 对齐，edge_b 无任何行数据）...");
    if (!setupDatabase(centerDb) || !setupDatabase(edgeBDb))
        return 1;
    LOG("SETUP", "schema 建立完成");

    // ── 2. 打开 DataBridge ───────────────────────────────────────────────────
    dbridge::DataBridge centerBridge, edgeBridge;
    QString err;
    dbridge::ConnectionSpec centerSpec, edgeSpec;
    centerSpec.sqlitePath = centerDb;
    edgeSpec.sqlitePath = edgeBDb;
    centerSpec.enableWal = edgeSpec.enableWal = true;
    if (!centerBridge.open(centerSpec, &err)) {
        std::cerr << "center DataBridge open failed: " << err.toStdString() << "\n";
        return 1;
    }
    if (!edgeBridge.open(edgeSpec, &err)) {
        std::cerr << "edge_b DataBridge open failed: " << err.toStdString() << "\n";
        return 1;
    }
    LOG("SETUP", "DataBridge 已打开");

    // ── 3. 构建 SyncConfig ───────────────────────────────────────────────────
    dbridge::sync::SyncConfig centerCfg =
        dbridge::sync::SyncConfig::Builder()
            .nodeId(QStringLiteral("center"))
            .role(dbridge::sync::NodeRole::Center)
            .addPeerNode(QStringLiteral("edge_b"))
            .database(centerDb)
            .outboxDir(ws + "/center/outbox")
            .inboxDir(ws + "/center/inbox")
            .quarantineDir(ws + "/center/quarantine")
            .conflictPolicy(dbridge::sync::ConflictPolicy::SourceWins)
            .originPriority(QStringLiteral("center"), 100)
            .originPriority(QStringLiteral("edge_b"), 50)
            .broadcastIntervalMs(300)
            .ackMaxDelayMs(6000)
            .build(&err);
    if (!centerCfg.isValid()) {
        std::cerr << "center SyncConfig failed: " << err.toStdString() << "\n";
        return 1;
    }

    dbridge::sync::SyncConfig edgeCfg =
        dbridge::sync::SyncConfig::Builder()
            .nodeId(QStringLiteral("edge_b"))
            .role(dbridge::sync::NodeRole::Edge)
            .centerNodeId(QStringLiteral("center"))
            .addPeerNode(QStringLiteral("center"))
            .database(edgeBDb)
            .outboxDir(ws + "/edge_b/outbox")
            .inboxDir(ws + "/edge_b/inbox")
            .quarantineDir(ws + "/edge_b/quarantine")
            .conflictPolicy(dbridge::sync::ConflictPolicy::SourceWins)
            .originPriority(QStringLiteral("center"), 100)
            .originPriority(QStringLiteral("edge_b"), 50)
            .broadcastIntervalMs(300)
            .ackMaxDelayMs(6000)
            .build(&err);
    if (!edgeCfg.isValid()) {
        std::cerr << "edge_b SyncConfig failed: " << err.toStdString() << "\n";
        return 1;
    }
    LOG("SETUP", "SyncConfig 构建完成");

    // ── 4. 创建并初始化同步引擎 ──────────────────────────────────────────────
    auto centerEngine = dbridge::sync::createSyncEngine(centerBridge);
    auto edgeEngine = dbridge::sync::createSyncEngine(edgeBridge);

    if (!centerEngine->initialize(centerCfg, &err)) {
        std::cerr << "center initialize failed: " << err.toStdString() << "\n";
        return 1;
    }
    LOG("center", "同步引擎初始化成功");

    if (!edgeEngine->initialize(edgeCfg, &err)) {
        std::cerr << "edge_b initialize failed: " << err.toStdString() << "\n";
        return 1;
    }
    LOG("edge_b", "同步引擎初始化成功");

    // ── 5. 启动 UDP 传输层 ───────────────────────────────────────────────────
    //
    //  center  ←──UDP 15001──  edge_b  （edge_b 向 center 发送，包含 changeset + ACK）
    //  center  ──UDP 15002──→  edge_b  （center 向 edge_b 发送，包含 changeset + ACK）
    //
    //  每个 UdpFileTransport 后台线程：
    //    · 每 50ms 轮询 outboxDir，发现 .ready 标记后以 60KB/片发送 UDP 数据报
    //    · 同时侦听自己的端口，收到完整数据报后写入 inboxDir 并创建 .ready 标记
    //  SyncWorker 主循环每 broadcastIntervalMs=300ms 扫描 inbox + 广播 outbox，
    //  与 UDP 线程协作实现无手动干预的端对端数据流转。

    std::cout << "\n--- 启动 UDP 传输层 ---\n";
    const QHostAddress loopback(QHostAddress::LocalHost);

    UdpFileTransport centerTransport(15001, loopback, 15002, ws + "/center/outbox",
                                     ws + "/center/inbox");
    UdpFileTransport edgeTransport(15002, loopback, 15001, ws + "/edge_b/outbox",
                                   ws + "/edge_b/inbox");

    centerTransport.start();
    edgeTransport.start();
    QThread::msleep(100);  // 等待 socket bind 完成
    LOG("UDP", "center 侦听 :15001  edge_b 侦听 :15002  传输层就绪");

    // ════════════════════════════════════════════════════════════════════════
    // Phase 1: 下行初始同步（Center → Edge_b）
    //
    // center 通过 ISyncEngine::write() 写入全部基线数据。write() 经由
    // SyncWorker 的唯一写连接 + SessionRecorder 完成，每一行变更都被
    // SQLite session 捕获并写入 __sync_changelog。
    //
    // employees 与 assignments 分两次 write()，保证 edge_b 应用 changeset
    // 时 FK 父行（employees）先于子行（assignments）落库。
    //
    // center.sync() 将 changelog 打包成 changeset artifact，写入 center/outbox；
    // UDP 传输层将其送达 edge_b/inbox；edge_b 的 SyncWorker 扫描到 inbox
    // 后应用 changeset，将变更写入 edge_b.db，再回送 ACK；UDP 将 ACK
    // 送达 center/inbox；center SyncWorker 收到 ACK 后将前台 sync() 标记
    // 为 Completed，ACK 等待解除。
    //
    // 整个流程不依赖任何预填数据：edge_b 真正从零追齐 center。
    // ════════════════════════════════════════════════════════════════════════
    std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
              << " Phase 1: 下行初始同步（Center 基线 → 空白 Edge_b）\n"
              << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    // 1-a: 写入员工记录（employees 先写，session 捕获，进入 changelog）
    if (!centerEngine->write({empMut(1, QStringLiteral("张三"), QStringLiteral("研发"), 20000),
                              empMut(2, QStringLiteral("李四"), QStringLiteral("产品"), 18000),
                              empMut(3, QStringLiteral("王五"), QStringLiteral("研发"), 22000)},
                             &err)) {
        std::cerr << "center write employees failed: " << err.toStdString() << "\n";
        return 1;
    }
    LOG("center", "[1-a] 写入员工基线（id 1-3）→ changelog");

    // 1-b: 写入分配记录（assignments 后写，FK 父行已在 changelog 中）
    if (!centerEngine->write({asnMut(1, 1, QStringLiteral("ProjectA"), QStringLiteral("Lead")),
                              asnMut(2, 2, QStringLiteral("ProjectA"), QStringLiteral("Member")),
                              asnMut(3, 3, QStringLiteral("ProjectB"), QStringLiteral("Lead"))},
                             &err)) {
        std::cerr << "center write assignments failed: " << err.toStdString() << "\n";
        return 1;
    }
    LOG("center", "[1-b] 写入分配基线（id 1-3）→ changelog");

    // 1-c: center.sync() — 将两批 changelog 打包为 changeset artifact，写入 outbox，
    //      arm ACK wait；UDP 传输；edge_b 应用；edge_b 回送 ACK；center Completed。
    std::cout << "\n  [center] sync() → 打包 changelog → UDP 发往 edge_b ...\n";
    if (!centerEngine->sync(&err)) {
        std::cerr << "center sync failed: " << err.toStdString() << "\n";
        return 1;
    }
    if (!waitForEngine(centerEngine.get(), "center Phase 1 初始广播")) {
        std::cerr << "center Phase 1 sync 超时\n";
        return 1;
    }
    printSyncResult("center Phase 1", centerEngine->result());
    printLogs("center", centerEngine->logs());

    // edge_b 收到 changeset 后 SyncWorker 在下一个 broadcastIntervalMs 内自动扫描
    // inbox 并应用；ACK 已在上面的 waitForEngine 之前发出（apply 成功后立即调度）。
    // 此处稍等以确保 edge_b 日志已记录完毕，方便打印。
    QThread::msleep(500);
    printLogs("edge_b", edgeEngine->logs());

    std::cout << "\n===== Phase 1 完成：两端数据应完全一致 =====\n";
    dumpTable(centerDb, QStringLiteral("employees"), "center");
    dumpTable(edgeBDb, QStringLiteral("employees"), "edge_b");
    dumpTable(centerDb, QStringLiteral("assignments"), "center");
    dumpTable(edgeBDb, QStringLiteral("assignments"), "edge_b");

    // ════════════════════════════════════════════════════════════════════════
    // Phase 2: 上行增量同步（Edge_b → Center）
    //
    // edge_b 在本地执行业务写入（session 捕获），sync() 将变更打包为
    // changeset artifact 发往 center；center SyncWorker 应用后回送 ACK；
    // edge_b 收到 ACK 后前台 sync() 标记 Completed。
    //
    // 这是最典型的「边缘节点上报变更」路径，完整经历了：
    //   write() → changelog → sync() → outbox artifact → UDP → center inbox
    //   → ChangesetApplier.apply() → center DB 更新 → AckChannel.flush()
    //   → ACK artifact → UDP → edge_b inbox → processAckArtifact() → Completed
    // ════════════════════════════════════════════════════════════════════════
    std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
              << " Phase 2: 上行增量同步（Edge_b → Center）\n"
              << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    // 王五薪资 22000 → 25000；新增员工赵六
    if (!edgeEngine->write({empMut(3, QStringLiteral("王五"), QStringLiteral("研发"), 25000),
                            empMut(4, QStringLiteral("赵六"), QStringLiteral("市场"), 16000)},
                           &err)) {
        std::cerr << "edge_b write failed: " << err.toStdString() << "\n";
        return 1;
    }
    LOG("edge_b", "[2-a] 写入：王五薪资 22000→25000，新增赵六（市场部）→ changelog");

    std::cout << "\n  [edge_b] sync() → 打包 changelog → UDP 发往 center ...\n";
    if (!edgeEngine->sync(&err)) {
        std::cerr << "edge_b sync failed: " << err.toStdString() << "\n";
        return 1;
    }
    if (!waitForEngine(edgeEngine.get(), "edge_b Phase 2")) {
        std::cerr << "edge_b Phase 2 sync 超时\n";
    }
    printSyncResult("edge_b Phase 2", edgeEngine->result());

    // 给 center 足够时间处理并写 ACK 日志
    QThread::msleep(600);
    printLogs("center", centerEngine->logs());

    std::cout << "\n===== Phase 2 完成：center 已应用 edge_b 的变更 =====\n";
    dumpTable(centerDb, QStringLiteral("employees"), "center");
    dumpTable(edgeBDb, QStringLiteral("employees"), "edge_b");

    // ════════════════════════════════════════════════════════════════════════
    // Phase 3: 下行增量同步（Center → Edge_b 主动广播）
    //
    // center 主动写入新员工钱七，sync() 将变更广播给 edge_b；
    // edge_b SyncWorker 在 broadcastIntervalMs 内自动扫描 inbox 并应用，
    // 随后回送 ACK，center 前台 sync() 标记 Completed。
    //
    // 与 Phase 2 的区别：发起方是 center（高权威节点），接收方是 edge_b，
    // 演示了完整的「中心主动下推」协议路径。
    // ════════════════════════════════════════════════════════════════════════
    std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
              << " Phase 3: 下行增量同步（Center → Edge_b）\n"
              << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    if (!centerEngine->write({empMut(5, QStringLiteral("钱七"), QStringLiteral("运营"), 17000)},
                             &err)) {
        std::cerr << "center write failed: " << err.toStdString() << "\n";
        return 1;
    }
    LOG("center", "[3-a] 写入：新员工钱七（运营部，id=5）→ changelog");

    std::cout << "\n  [center] sync() → 打包 changelog → UDP 发往 edge_b ...\n";
    if (!centerEngine->sync(&err)) {
        std::cerr << "center sync failed: " << err.toStdString() << "\n";
        return 1;
    }
    if (!waitForEngine(centerEngine.get(), "center Phase 3")) {
        std::cerr << "center Phase 3 sync 超时\n";
    }
    printSyncResult("center Phase 3", centerEngine->result());

    QThread::msleep(600);
    printLogs("edge_b", edgeEngine->logs());

    std::cout << "\n===== Phase 3 完成：edge_b 已自动应用 center 的新员工 =====\n";
    dumpTable(centerDb, QStringLiteral("employees"), "center");
    dumpTable(edgeBDb, QStringLiteral("employees"), "edge_b");

    // ════════════════════════════════════════════════════════════════════════
    // Phase 4: 选择性推送（Edge_b syncSelected → Center）
    //
    // edge_b 为赵六新建分配记录（assignments.id=4），然后通过 syncSelected()
    // 精确指定推送该记录；框架自动补全 FK 父行（employees.id=4）并对
    // center 已拥有的一致父行执行剪枝（pruneConsistentDependencies=true），
    // 仅发送差异部分。
    //
    // 完整协议路径：
    //   syncSelected() → SelectionResolver → FkClosureBuilder → ChunkStreamer
    //   → OutboxWriter（SelectionPush artifact）→ UDP → center inbox
    //   → processSelectionPushArtifact() → SelectionPushApplier.apply()
    //   → center DB 更新 → PushChunkAck → UDP → edge_b inbox
    //   → processAckArtifact() → pendingPushId_ 清除 → Completed
    // ════════════════════════════════════════════════════════════════════════
    std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
              << " Phase 4: 选择性推送（Edge_b syncSelected → Center）\n"
              << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    // 4-a: 写入赵六的项目分配（session 捕获）
    if (!edgeEngine->write({asnMut(4, 4, QStringLiteral("ProjectC"), QStringLiteral("Member"))},
                           &err)) {
        std::cerr << "edge_b write assignment failed: " << err.toStdString() << "\n";
        return 1;
    }
    LOG("edge_b", "[4-a] 写入：赵六 → ProjectC（assignments.id=4）→ changelog");

    // 4-b: 构建选择集——精确选定 assignments.id=4，FK 闭包自动补全 employees.id=4
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

    // 4-c: syncSelected() — SelectionResolver 解析记录集 → ChunkStreamer 分块
    //       → OutboxWriter 写 SelectionPush artifact → UDP 传输 → center 应用
    std::cout << "\n  [edge_b] syncSelected(assignments.id=4, FK deps=true) ...\n";
    if (!edgeEngine->syncSelected(sel, &err)) {
        std::cerr << "syncSelected rejected: " << err.toStdString() << "\n";
        return 1;
    }
    LOG("edge_b", "[4-b] syncSelected() 已提交，等待 center ACK ...");

    // 轮询直到前台 syncSelected() 完成（ACK 到达）或超时
    if (!waitForEngine(edgeEngine.get(), "edge_b Phase 4 syncSelected")) {
        std::cerr << "edge_b Phase 4 syncSelected 超时\n";
    }
    printSyncResult("edge_b Phase 4 syncSelected", edgeEngine->result());

    // 给 center 处理并更新日志
    QThread::msleep(600);
    printLogs("center", centerEngine->logs());

    std::cout << "\n===== Phase 4 完成：center assignments 状态 =====\n";
    dumpTable(centerDb, QStringLiteral("assignments"), "center");
    dumpTable(edgeBDb, QStringLiteral("assignments"), "edge_b");

    // ════════════════════════════════════════════════════════════════════════
    // Phase 5: 冲突解决（SourceWins，center rank=100 胜出）
    //
    // 演示两端对同一行（employees.id=1，张三）几乎同时写入不同薪资值时，
    // ConflictArbiter 如何根据 RowWinnerStore 中的优先级记录做出仲裁：
    //
    // 仲裁依赖 __sync_row_winner 表（仅由 ChangesetApplier 的接收路径写入，
    // 本地 write() 不写 winner 表）。因此需要顺序触发：
    //
    //  ① edge_b 写 28000 → sync → center 收到并应用（winner ← {edge_b, rank=50}）
    //  ② center 写 30000 → sync → edge_b 收到时：
    //       incumbent = {center, 100}（已由 ①写入）→ 但等等……
    //       center 是 edge_b 收到 ① 时写的 winner，不是 center 自己 write() 写的。
    //
    // 正确顺序说明（见下方注释）：
    //   edge_b 先广播 28000 → center 接收应用，center winner←{edge_b,50}
    //   然后 center 广播 30000 → edge_b 接收时，incumbent=INT_MIN（本地 write 不写 winner），
    //   challenger={center,100}，100 > INT_MIN → edge_b 应用 30000 ✅
    //   center 自身已有 30000（本地写）✅ → 两端收敛到 30000
    // ════════════════════════════════════════════════════════════════════════
    std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
              << " Phase 5: 冲突解决（SourceWins, center rank=100）\n"
              << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    // 先刷清双端 changelog，确保上一阶段的所有变更已完全传播，
    // 避免历史 changeset 干扰本阶段的冲突检测序列。
    std::cout << "\n  [pre-flush] 清空双端待广播 changelog ...\n";
    edgeEngine->sync(&err);
    waitForEngine(edgeEngine.get(), "pre-conflict flush edge_b");
    centerEngine->sync(&err);
    waitForEngine(centerEngine.get(), "pre-conflict flush center");
    QThread::msleep(400);

    // 5-a: edge_b 先写 张三薪资 28000 并广播
    //       center 收到并应用后，winner 表记录 {origin=edge_b, rank=50}
    if (!edgeEngine->write({empMut(1, QStringLiteral("张三"), QStringLiteral("研发"), 28000)},
                           &err))
        std::cerr << "edge_b write failed: " << err.toStdString() << "\n";
    LOG("edge_b", "[5-a] 写入：张三薪资 → 28000");

    edgeEngine->sync(&err);
    if (!waitForEngine(edgeEngine.get(), "edge_b 广播 28000"))
        std::cerr << "edge_b 广播 28000 超时\n";

    // 等待 center 接收并应用 edge_b 的 28000，写入 winner={edge_b, rank=50}
    QThread::msleep(800);
    LOG("center", "[5-a] 应已收到 edge_b 的 28000，winner={edge_b, rank=50}");

    // 5-b: center 写 张三薪资 30000 并广播（rank=100 将赢得仲裁）
    //       edge_b 收到时：
    //         incumbent: 本地 write() 不更新 winner → INT_MIN
    //         challenger: {center, rank=100}
    //         100 > INT_MIN → 应用 30000 → edge_b = 30000 ✅
    //       center 自身已有 30000 ✅
    if (!centerEngine->write({empMut(1, QStringLiteral("张三"), QStringLiteral("研发"), 30000)},
                             &err))
        std::cerr << "center write failed: " << err.toStdString() << "\n";
    LOG("center", "[5-b] 写入：张三薪资 → 30000（将覆盖 edge_b 的 28000）");

    centerEngine->sync(&err);
    if (!waitForEngine(centerEngine.get(), "center 广播 30000"))
        std::cerr << "center 广播 30000 超时\n";

    // edge_b 收到 center 的 30000 并自动应用（broadcastIntervalMs 内触发）
    QThread::msleep(800);

    LOG("", "[5] 冲突解决完毕：期望两端 张三薪资 均为 30000");
    std::cout << "\n===== Phase 5 完成：冲突解决后两端数据状态 =====\n";
    dumpTable(centerDb, QStringLiteral("employees"), "center（冲突后）");
    dumpTable(edgeBDb, QStringLiteral("employees"), "edge_b（冲突后）");

    // ── 最终同步日志汇总 ─────────────────────────────────────────────────────
    std::cout << "\n===== 最终同步日志 =====\n";
    printLogs("center", centerEngine->logs());
    printLogs("edge_b", edgeEngine->logs());

    const auto centerErrs = centerEngine->errors();
    const auto edgeErrs = edgeEngine->errors();
    if (!centerErrs.isEmpty() || !edgeErrs.isEmpty()) {
        std::cout << "\n[WARNING] 同步引擎存在错误记录：\n";
        for (const auto& e : centerErrs)
            std::cout << "  [center] " << e.code.toStdString() << " " << e.message.toStdString()
                      << "\n";
        for (const auto& e : edgeErrs)
            std::cout << "  [edge_b] " << e.code.toStdString() << " " << e.message.toStdString()
                      << "\n";
    }

    // ── 清理 ─────────────────────────────────────────────────────────────────
    centerEngine.reset();
    edgeEngine.reset();
    centerBridge.close();
    edgeBridge.close();

    centerTransport.requestStop();
    edgeTransport.requestStop();
    centerTransport.wait();
    edgeTransport.wait();

    std::cout << "\n=================================================\n"
              << " sync-demo 完成\n"
              << "=================================================\n";
    return 0;
}
