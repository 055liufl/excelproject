/**
 * sync-demo — dbridge SQLite 多节点增量同步范式演示
 *
 * 本 demo 在同一进程内模拟 **中心节点（center）** 与 **子节点（edge）** 之间的
 * 增量同步全流程，演示以下代码范式：
 *
 *  1. SyncConfig::Builder  — 构建节点配置（角色/目录/冲突策略/origin 优先级）
 *  2. ISyncEngine          — 引擎生命周期：initialize → sync → 轮询进度/日志
 *  3. IBatchTransfer       — 在同步激活状态下写数据（同步感知导入）
 *  4. SyncSelection::Builder — 上行选择性推送：指定主键集合 + FK 闭包自动补全
 *  5. 冲突场景             — 两个节点独立修改同一行，观察 SourceWins 策略的结果
 *  6. 错误处理             — 逐个检查 SyncError / SyncLogEntry
 *
 * ──────────────────────────────────────────────────────────────────────────────
 * 运行方式（在项目构建目录下）：
 *   ./examples/sync-demo/sync-demo <workspace-dir>
 *
 * workspace-dir 下会自动创建：
 *   center.db   — 中心节点数据库
 *   edge_b.db   — 子节点 B 数据库
 *   center/outbox/  center/inbox/   center/quarantine/
 *   edge_b/outbox/  edge_b/inbox/   edge_b/quarantine/
 * ──────────────────────────────────────────────────────────────────────────────
 *
 * ⚠️  重要说明：
 *   传输层（文件从 outbox → inbox 的实际搬运）由**第三方工具**完成，dbridge 本身
 *   不内置网络/文件拷贝。本 demo 使用 QFile::copy 模拟"本机搬运"，仅供演示。
 *   真实部署中此处替换为 rsync / 消息队列 / 自研网关等。
 */

#include "dbridge/DataBridge.h"
#include "dbridge/Errors.h"
#include "dbridge/IBatchTransfer.h"
#include "dbridge/sync/ISyncEngine.h"
#include "dbridge/sync/SyncConfig.h"
#include "dbridge/sync/SyncSelection.h"
#include "dbridge/sync/SyncTypes.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QString>
#include <QThread>

#include <iostream>

// ── 工具宏：打印节点前缀 ──────────────────────────────────────────────────────
#define LOG(node, msg) std::cout << "[" << (node) << "] " << (msg) << std::endl

// ── 工具函数：打印 SyncResult ─────────────────────────────────────────────────
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

// ── 工具函数：打印同步引擎的日志环（SyncLogEntry） ───────────────────────────
static void printLogs(const std::string& node, const QList<dbridge::sync::SyncLogEntry>& logs) {
    for (const auto& entry : logs) {
        const char* sev = entry.severity == dbridge::sync::Severity::Warning ? "WARN"
                          : entry.severity == dbridge::sync::Severity::Error ? "ERR "
                          : entry.severity == dbridge::sync::Severity::Fatal ? "FATL"
                                                                             : "INFO";
        std::cout << "  [" << node << "|" << sev << "] "
                  << "[" << entry.phase.toStdString() << "] " << entry.message.toStdString()
                  << "\n";
    }
}

// ── 工具函数：建库 + 建表（center / edge 共用同一 schema）────────────────────
static bool setupDatabase(const QString& dbPath) {
    // 用独立连接名避免与 DataBridge 内部连接冲突
    const QString connName = QStringLiteral("setup_") + dbPath;
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(dbPath);
        if (!db.open()) {
            std::cerr << "Cannot open " << dbPath.toStdString() << ": "
                      << db.lastError().text().toStdString() << "\n";
            return false;
        }
        QSqlQuery q(db);
        // 员工表——主表
        q.exec(
            QStringLiteral("CREATE TABLE IF NOT EXISTS employees ("
                           "  id      INTEGER PRIMARY KEY,"
                           "  name    TEXT    NOT NULL,"
                           "  dept    TEXT    NOT NULL,"
                           "  salary  INTEGER NOT NULL"
                           ")"));
        // 项目分配表——外键引用 employees
        q.exec(
            QStringLiteral("CREATE TABLE IF NOT EXISTS assignments ("
                           "  id          INTEGER PRIMARY KEY,"
                           "  employee_id INTEGER NOT NULL REFERENCES employees(id),"
                           "  project     TEXT    NOT NULL,"
                           "  role        TEXT    NOT NULL"
                           ")"));
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);
    return true;
}

// ── 工具函数：向数据库直接写入数据（绕过同步，用于演示初始状态）──────────────
static bool seedData(const QString& dbPath, const QStringList& sqls) {
    const QString connName = QStringLiteral("seed_") + dbPath;
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(dbPath);
        if (!db.open())
            return false;
        QSqlQuery q(db);
        for (const auto& sql : sqls) {
            if (!q.exec(sql)) {
                std::cerr << "SQL failed: " << sql.toStdString() << " — "
                          << q.lastError().text().toStdString() << "\n";
                return false;
            }
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);
    return true;
}

// ── 工具函数：查询并打印一张表的全部行 ───────────────────────────────────────
static void dumpTable(const QString& dbPath, const QString& table, const std::string& label) {
    const QString connName = QStringLiteral("dump_") + dbPath + table;
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
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
    QSqlDatabase::removeDatabase(connName);
}

// ── 工具函数："搬运" outbox 到对端 inbox（模拟传输层）────────────────────────
// 真实场景：此处换成 rsync / sftp / 消息队列投递等。
static void transferArtifacts(const QString& fromOutbox, const QString& toInbox) {
    QDir outDir(fromOutbox);
    QStringList readyFiles =
        outDir.entryList(QStringList() << QStringLiteral("*.ready"), QDir::Files);

    for (const QString& readyName : readyFiles) {
        // 每个 .ready 哨兵对应一个同名 .payload 主文件
        QString baseName = readyName;
        baseName.remove(readyName.size() - 6, 6);  // 去掉 ".ready"

        // 只搬运同名的 .payload（若尚未到对端）
        QString payloadName = baseName;  // 格式已含后缀
        QString srcPayload = fromOutbox + "/" + payloadName;
        QString dstPayload = toInbox + "/" + payloadName;
        QString srcReady = fromOutbox + "/" + readyName;
        QString dstReady = toInbox + "/" + readyName;

        if (!QFile::exists(srcPayload))
            continue;
        if (QFile::exists(dstPayload))
            continue;  // 幂等：已搬就跳过

        QFile::copy(srcPayload, dstPayload);

        // 先落主文件再落哨兵，与 OutboxWriter 的原子发布协议一致
        QFile::copy(srcReady, dstReady);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// 第一阶段：演示 "基线同步" ——
//   center 有初始数据，edge_b 从空库冷启动，拿到完整基线。
//
// （本 demo 简化：两端从相同 seed 数据起步，跳过实际 Baseline 报文交换，
//   直接 seed edge_b 为与 center 相同内容，然后演示增量同步。
//   真实冷启动：调 BaselineManager，通过 BaselineRequest/Response 报文传输全量。）
// ──────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    // ── 0. 解析工作目录 ──────────────────────────────────────────────────────
    if (argc < 2) {
        std::cerr << "Usage: sync-demo <workspace-dir>\n";
        return 1;
    }
    const QString ws = QString::fromLocal8Bit(argv[1]);
    QDir().mkpath(ws + "/center/outbox");
    QDir().mkpath(ws + "/center/inbox");
    QDir().mkpath(ws + "/center/quarantine");
    QDir().mkpath(ws + "/edge_b/outbox");
    QDir().mkpath(ws + "/edge_b/inbox");
    QDir().mkpath(ws + "/edge_b/quarantine");

    const QString centerDb = ws + "/center.db";
    const QString edgeBDb = ws + "/edge_b.db";

    std::cout << "=================================================\n"
              << " sync-demo: dbridge 多节点增量同步范式\n"
              << "=================================================\n"
              << "  workspace : " << ws.toStdString() << "\n"
              << "  center.db : " << centerDb.toStdString() << "\n"
              << "  edge_b.db : " << edgeBDb.toStdString() << "\n\n";

    // ── 1. 建库建表 ──────────────────────────────────────────────────────────
    LOG("SETUP", "建库建表...");
    if (!setupDatabase(centerDb) || !setupDatabase(edgeBDb))
        return 1;

    // 初始数据（center 和 edge_b 以相同内容起步，模拟基线已对齐）
    QStringList initSqls = {
        QStringLiteral("INSERT OR IGNORE INTO employees VALUES (1,'张三','研发',20000)"),
        QStringLiteral("INSERT OR IGNORE INTO employees VALUES (2,'李四','产品',18000)"),
        QStringLiteral("INSERT OR IGNORE INTO employees VALUES (3,'王五','研发',22000)"),
        QStringLiteral("INSERT OR IGNORE INTO assignments VALUES (1,1,'ProjectA','Lead')"),
        QStringLiteral("INSERT OR IGNORE INTO assignments VALUES (2,2,'ProjectA','Member')"),
        QStringLiteral("INSERT OR IGNORE INTO assignments VALUES (3,3,'ProjectB','Lead')"),
    };
    if (!seedData(centerDb, initSqls))
        return 1;
    if (!seedData(edgeBDb, initSqls))
        return 1;
    LOG("SETUP", "初始数据就绪（center 与 edge_b 内容相同，模拟基线对齐完毕）");

    // ── 2. 打开 DataBridge ───────────────────────────────────────────────────
    dbridge::DataBridge centerBridge, edgeBridge;
    QString err;

    dbridge::ConnectionSpec centerSpec, edgeSpec;
    centerSpec.sqlitePath = centerDb;
    edgeSpec.sqlitePath = edgeBDb;
    centerSpec.enableWal = true;
    edgeSpec.enableWal = true;

    if (!centerBridge.open(centerSpec, &err)) {
        std::cerr << "center open failed: " << err.toStdString() << "\n";
        return 1;
    }
    if (!edgeBridge.open(edgeSpec, &err)) {
        std::cerr << "edge_b open failed: " << err.toStdString() << "\n";
        return 1;
    }
    LOG("SETUP", "DataBridge 已打开");

    // ── 3. 构建 SyncConfig：中心节点 ─────────────────────────────────────────
    //
    //  SyncConfig::Builder 采用 Fluent 链式调用风格。
    //  所有必填项（nodeId / database / outboxDir / inboxDir）缺失时
    //  build() 会写 *err 并返回 isValid()==false 的对象。
    //
    //  关键配置说明：
    //  - role(Center)         ：该节点是权威源，负责裁决冲突并向全域广播
    //  - originPriority       ：rank 越大优先级越高，用于 (rank,seq) 冲突全序
    //  - conflictPolicy       ：SourceWins = 高 rank origin 的变更赢得冲突
    //  - broadcastThreshold(1)：攒 1 条变更即触发广播（演示用，生产建议 ≥100）
    //  - syncTables           ：空列表 = 监控所有用户表

    dbridge::sync::SyncConfig centerCfg =
        dbridge::sync::SyncConfig::Builder()
            .nodeId(QStringLiteral("center"))
            .role(dbridge::sync::NodeRole::Center)
            .addPeerNode(QStringLiteral("edge_b"))  // 中心知道自己有哪些子节点
            .database(centerDb)
            .outboxDir(ws + "/center/outbox")
            .inboxDir(ws + "/center/inbox")
            .quarantineDir(ws + "/center/quarantine")
            .conflictPolicy(dbridge::sync::ConflictPolicy::SourceWins)
            // center rank=100, edge_b rank=50 → center 的变更在冲突时赢
            .originPriority(QStringLiteral("center"), 100)
            .originPriority(QStringLiteral("edge_b"), 50)
            .broadcastThreshold(1)     // 演示：攒 1 条即广播
            .broadcastIntervalMs(500)  // 演示：500ms 最大等待
            .ackMaxDelayMs(2000)       // ACK 最大等待 2s
            .build(&err);

    if (!centerCfg.isValid()) {
        std::cerr << "center SyncConfig build failed: " << err.toStdString() << "\n";
        return 1;
    }

    // ── 4. 构建 SyncConfig：子节点 B ─────────────────────────────────────────
    //
    //  Edge 节点必须指定 centerNodeId。
    //  rank 与 center 保持全局一致（均在此处配置）。

    dbridge::sync::SyncConfig edgeCfg =
        dbridge::sync::SyncConfig::Builder()
            .nodeId(QStringLiteral("edge_b"))
            .role(dbridge::sync::NodeRole::Edge)
            .centerNodeId(QStringLiteral("center"))  // 指定权威源
            .addPeerNode(QStringLiteral("center"))   // edge_b 只与 center 通信
            .database(edgeBDb)
            .outboxDir(ws + "/edge_b/outbox")
            .inboxDir(ws + "/edge_b/inbox")
            .quarantineDir(ws + "/edge_b/quarantine")
            .conflictPolicy(dbridge::sync::ConflictPolicy::SourceWins)
            .originPriority(QStringLiteral("center"), 100)
            .originPriority(QStringLiteral("edge_b"), 50)
            .broadcastThreshold(1)
            .broadcastIntervalMs(500)
            .ackMaxDelayMs(2000)
            .build(&err);

    if (!edgeCfg.isValid()) {
        std::cerr << "edge_b SyncConfig build failed: " << err.toStdString() << "\n";
        return 1;
    }
    LOG("SETUP", "SyncConfig 构建完成（center + edge_b）");

    // ── 5. 创建并初始化同步引擎 ──────────────────────────────────────────────
    //
    //  createSyncEngine(bridge) 以 DataBridge 实例为 key，按物理文件标识
    //  (st_dev, st_ino) 共享 SyncContext，保证同一 .db 只有一个写线程。
    //
    //  initialize() 做：
    //    ① SchemaEligibility 校验（拒绝虚表/无 PK 表等）
    //    ② 创建/迁移 11 张 __sync_* 元数据表
    //    ③ 挂载 SQLite session（SQLITE_ENABLE_SESSION 必须开启）
    //    ④ 启动 SyncWorker 后台写线程

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

    // ── 6. 在 edge_b 上本地修改数据 ──────────────────────────────────────────
    //
    //  同步引擎激活后，所有对同步表的直接 importExcel() 会被拦截。
    //  通过 IBatchTransfer 写入，写操作会经由 SyncWorker 的唯一写连接
    //  完成，同时 SQLite session 捕获变更写入 __sync_changelog。

    std::cout << "\n--- 步骤 A：edge_b 本地修改员工薪资 ---\n";

    // 直接用 SQL（seed 级辅助函数）在 edge_b 上修改薪资
    // 注意：生产中应通过 IBatchTransfer / DataBridge 导入（带 session 捕获）。
    // 此处为演示简化，直接写 DB；真实增量由 session 捕获。
    if (!seedData(
            edgeBDb,
            {
                QStringLiteral("UPDATE employees SET salary=25000 WHERE id=3"),  // 王五涨薪
                QStringLiteral(
                    "INSERT OR REPLACE INTO employees VALUES (4,'赵六','市场',16000)"),  // 新员工
            }))
        return 1;
    LOG("edge_b", "本地变更：王五薪资 22000→25000，新增赵六");

    // ── 7. edge_b 手动触发 sync()——将本地变更打包到 outbox ──────────────────
    //
    //  sync() 是"手动 drain"：
    //    ① 扫描 inbox，接收并应用来自 center 的下行变更
    //    ② 将 changelog 里新产生的变更打包成制品写到 outbox
    //  非阻塞：不等第三方工具把文件搬到 center 的 inbox。

    std::cout << "\n--- 步骤 B：edge_b sync()——打包 outbox ---\n";
    if (!edgeEngine->sync(&err)) {
        std::cerr << "edge_b sync failed: " << err.toStdString() << "\n";
    }
    LOG("edge_b", "sync() 完成，变更已写入 outbox");
    printLogs("edge_b", edgeEngine->logs());

    // ── 8. 模拟传输层：把 edge_b outbox → center inbox ───────────────────────
    std::cout << "\n--- 步骤 C：传输层 edge_b/outbox → center/inbox ---\n";
    transferArtifacts(ws + "/edge_b/outbox", ws + "/center/inbox");
    LOG("transport", "edge_b outbox → center inbox 搬运完成");

    // ── 9. center sync()——接收 edge_b 发来的变更 ─────────────────────────────
    std::cout << "\n--- 步骤 D：center sync()——应用入站变更并广播 ---\n";
    if (!centerEngine->sync(&err)) {
        std::cerr << "center sync failed: " << err.toStdString() << "\n";
    }
    LOG("center", "sync() 完成");
    printLogs("center", centerEngine->logs());
    printSyncResult("center", centerEngine->result());

    // center 把裁决后的变更广播回 edge_b（防回声：不会把 edge_b 原来的包再发回去）
    std::cout << "\n--- 步骤 E：传输层 center/outbox → edge_b/inbox ---\n";
    transferArtifacts(ws + "/center/outbox", ws + "/edge_b/inbox");
    LOG("transport", "center outbox → edge_b inbox 搬运完成");

    // edge_b 再次 sync()，接收 center 的 ACK 及下行广播（本次内容应已有）
    if (!edgeEngine->sync(&err)) {
        std::cerr << "edge_b sync(2) failed: " << err.toStdString() << "\n";
    }
    LOG("edge_b", "sync(2) 完成");

    // ── 10. 打印同步后两端的数据状态 ─────────────────────────────────────────
    std::cout << "\n===== 自动增量同步后的数据状态 =====\n";
    dumpTable(centerDb, QStringLiteral("employees"), "center");
    dumpTable(edgeBDb, QStringLiteral("employees"), "edge_b");

    // ── 11. 演示"上行选择性推送"（syncSelected）────────────────────────────
    //
    //  场景：edge_b 离线期间新增了一批项目分配记录，现在手动选择推给 center。
    //  syncSelected 会：
    //    ① SelectionResolver 解析 PK 集合
    //    ② FkClosureBuilder  自动求外键依赖闭包（assignments 引用 employees）
    //    ③ ConsistencyCache  剪枝已与 center 一致的依赖父行（节省带宽）
    //    ④ ChunkStreamer      按 pushChunkBudgetBytes 分片发送
    //    ⑤ 等待全片 ACK      超时报 E_SYNC_ACK_TIMEOUT

    std::cout << "\n--- 步骤 F：edge_b syncSelected()——上行选择推送 ---\n";

    // 先在 edge_b 新增一条分配记录（员工 4 / 赵六 → ProjectC）
    if (!seedData(edgeBDb,
                  {
                      QStringLiteral(
                          "INSERT OR IGNORE INTO assignments VALUES (4,4,'ProjectC','Member')"),
                  }))
        return 1;
    LOG("edge_b", "本地新增分配：赵六 → ProjectC");

    // 构建 SyncSelection：选中 assignments 主键 "4"
    //   includeFkDependencies(true)  → 自动带上 employees.id=4 的父行
    //   pruneConsistentDependencies  → 若 center 已有该父行则剪枝
    dbridge::sync::SyncSelection sel =
        dbridge::sync::SyncSelection::Builder()
            .addRecord(QStringLiteral("assignments"), QStringLiteral("4"))
            .includeFkDependencies(true)        // 自动补全 FK 父行
            .pruneConsistentDependencies(true)  // 剪枝已一致的父行
            .build(&err);

    if (sel.isEmpty()) {
        std::cerr << "SyncSelection build failed: " << err.toStdString() << "\n";
        // 非致命：继续演示
    } else {
        if (!edgeEngine->syncSelected(sel, &err)) {
            // syncSelected 的受理前校验失败（空选择/非法表名）从 *err 同步返回
            // 后台失败（FK闭包/分片超时）则通过 errors()/result() 异步上报
            std::cerr << "syncSelected rejected: " << err.toStdString() << "\n";
        } else {
            LOG("edge_b", "syncSelected() 已提交，等待 ACK...");
            // 轮询状态（真实 UI 中可用 QTimer 定期刷新）
            for (int i = 0; i < 20; ++i) {
                QThread::msleep(300);
                auto st = edgeEngine->state();
                if (st == dbridge::sync::SyncState::Completed ||
                    st == dbridge::sync::SyncState::Failed || st == dbridge::sync::SyncState::Idle)
                    break;
                auto prog = edgeEngine->progress();
                std::cout << "  [edge_b] state=" << static_cast<int>(st)
                          << "  percent=" << prog.percent << "\n";
            }
            printSyncResult("edge_b syncSelected", edgeEngine->result());
        }
    }

    // 搬运：edge_b outbox → center inbox（分片推送报文）
    transferArtifacts(ws + "/edge_b/outbox", ws + "/center/inbox");
    centerEngine->sync(&err);
    LOG("center", "接收到 syncSelected 分片并应用");
    transferArtifacts(ws + "/center/outbox", ws + "/edge_b/inbox");
    edgeEngine->sync(&err);

    // ── 12. 演示冲突场景 ────────────────────────────────────────────────────
    //
    //  两端同时修改同一行 employees.id=1（张三的薪资），
    //  center 改为 30000，edge_b 改为 28000。
    //  冲突策略：SourceWins + center rank=100 > edge_b rank=50
    //  → center 的值 30000 最终胜出。

    std::cout << "\n--- 步骤 G：冲突场景演示 ---\n";
    seedData(centerDb, {QStringLiteral("UPDATE employees SET salary=30000 WHERE id=1")});
    seedData(edgeBDb, {QStringLiteral("UPDATE employees SET salary=28000 WHERE id=1")});
    LOG("center", "张三薪资改为 30000");
    LOG("edge_b", "张三薪资改为 28000（将与 center 冲突）");

    // 双向 sync：center 先发，edge_b 接收后冲突由 center rank 100 胜出
    centerEngine->sync(&err);
    transferArtifacts(ws + "/center/outbox", ws + "/edge_b/inbox");
    edgeEngine->sync(&err);
    transferArtifacts(ws + "/edge_b/outbox", ws + "/center/inbox");
    centerEngine->sync(&err);
    transferArtifacts(ws + "/center/outbox", ws + "/edge_b/inbox");
    edgeEngine->sync(&err);

    LOG("", "冲突解决后，期望张三薪资在两端均为 30000（center 赢）");
    dumpTable(centerDb, QStringLiteral("employees"), "center (冲突后)");
    dumpTable(edgeBDb, QStringLiteral("employees"), "edge_b (冲突后)");

    // ── 13. 打印最终日志环 ───────────────────────────────────────────────────
    std::cout << "\n===== 最终同步日志 =====\n";
    printLogs("center", centerEngine->logs());
    printLogs("edge_b", edgeEngine->logs());

    // ── 14. 检查是否有未处理错误 ────────────────────────────────────────────
    auto centerErrs = centerEngine->errors();
    auto edgeErrs = edgeEngine->errors();
    if (!centerErrs.isEmpty() || !edgeErrs.isEmpty()) {
        std::cout << "\n[WARNING] 同步引擎存在错误：\n";
        for (const auto& e : centerErrs)
            std::cout << "  [center] " << e.code.toStdString() << " " << e.message.toStdString()
                      << "\n";
        for (const auto& e : edgeErrs)
            std::cout << "  [edge_b] " << e.code.toStdString() << " " << e.message.toStdString()
                      << "\n";
    }

    // ── 15. 清理（引擎析构时自动 stop + setSyncActive(false)）───────────────
    centerEngine.reset();
    edgeEngine.reset();
    centerBridge.close();
    edgeBridge.close();

    std::cout << "\n=================================================\n"
              << " sync-demo 完成\n"
              << "=================================================\n";
    return 0;
}
