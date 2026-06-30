// ============================================================================
// Scenario1Runner.cpp — 「场景1」后台剧本的实现
// ============================================================================
//
// 【本文件做什么】实现 Scenario1Runner.h 声明的编排线程。整体结构：
//   · 一组匿名命名空间里的「剧本道具」工具函数（建库、灌数、读数、造 RowMutation、
//     等待引擎）——它们刻意都用「即开即用、即用即弃」的临时 QSQLITE 连接，绝不复用
//     节点引擎内部的连接，以模拟「外部程序直接连库」的真实场景，且避免线程亲和性问题。
//   · 构造函数：注册队列连接所需的元类型。
//   · run()：四阶段剧本主体。
//
// 【贯穿全文件的临时连接套路（每个工具函数都重复，故在此统一说明）】
//   addDatabase("QSQLITE", connName) 用 UUID 拼出唯一连接名 → setDatabaseName → open
//   → 用完在内层 {} 作用域结束时让 QSqlDatabase 副本先析构 → 再 removeDatabase(connName)。
//   先析构后移除，是 Qt 的硬性要求（移除连接前不得有该连接的活动副本，否则告警+残留）。
//
// 【设计意图：为什么剧本里大量手动开临时连接而不走引擎】
//   场景要模拟「人/外部系统直接连物理库读写」，再观察同步引擎是否把这些「带外改动」
//   正确捕获、广播、仲裁。所以「写权威数据/读校验」走裸连接，而「应被同步出去的写」
//   则走 engine->write()（经 session 捕获）。两者刻意区分，正是剧本要演示的对照。
// ============================================================================
#include "Scenario1Runner.h"

#include "dbridge/DataBridge.h"
#include "dbridge/sync/ISyncEngine.h"
#include "dbridge/sync/SyncConfig.h"
#include "dbridge/sync/SyncTypes.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QMetaType>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>

#include "udp_transport.h"
#include <memory>

using namespace dbridge;
using namespace dbridge::sync;

// ── 常量 ─────────────────────────────────────────────────────────────────────
namespace {
constexpr int RANK_CENTER = 100;  // 中心A：全域最高优先级（承载指定库权威值）
constexpr int RANK_EDGE_B = 70;
constexpr int RANK_EDGE_C = 50;
constexpr int RANK_EDGE_D = 30;

// 独立端口段，避免与 sync-demo（15001-15004）冲突。
constexpr quint16 PORT_CENTER = 15101;
constexpr quint16 PORT_EDGE_B = 15102;
constexpr quint16 PORT_EDGE_C = 15103;
constexpr quint16 PORT_EDGE_D = 15104;

// ── 通用 SQL 执行 ────────────────────────────────────────────────────────────
// 在 dbPath 库上「按顺序」执行一批 SQL（建表/灌数/更新均可）。
//   · 任一条失败即短路返回 false，并把「错误文本 + 出错 SQL 前 100 字」写入 *err（便于定位）。
//   · 用临时 UUID 连接（套路见文件头），用完移除，不残留。
//   · 返回 true=全部成功。复杂度 O(语句数)。
bool execSqls(const QString& dbPath, const QStringList& sqls, QString* err = nullptr) {
    const QString conn =
        QStringLiteral("s1_exec_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool ok = true;
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        if (!db.open()) {
            if (err)
                *err = db.lastError().text();
            ok = false;
        } else {
            QSqlQuery q(db);
            for (const QString& s : sqls) {
                if (!q.exec(s)) {
                    // 失败时附上肇事 SQL 的前 100 字符，避免日志里只见错误码不见上下文。
                    if (err)
                        *err = q.lastError().text() + QStringLiteral(" SQL: ") + s.left(100);
                    ok = false;
                    break;  // 短路：一条失败即停，不再执行后续语句
                }
            }
            db.close();
        }
    }  // db 副本在此析构，满足「移除连接前不得有活动副本」的前置条件
    QSqlDatabase::removeDatabase(conn);
    return ok;
}

// 建节点/指定库 schema（employees + departments）。
// 用 CREATE TABLE IF NOT EXISTS：幂等，重复调用安全（剧本里四节点 + 指定库各建一次）。
// 两表刻意带 NOT NULL 约束与外键友好的简单结构，作为同步/冲突演示的最小业务模型。
bool setupSchema(const QString& dbPath, QString* err = nullptr) {
    return execSqls(dbPath,
                    {QStringLiteral("CREATE TABLE IF NOT EXISTS employees ("
                                    "  id INTEGER PRIMARY KEY, name TEXT NOT NULL,"
                                    "  dept TEXT NOT NULL, salary INTEGER NOT NULL)"),
                     QStringLiteral("CREATE TABLE IF NOT EXISTS departments ("
                                    "  id INTEGER PRIMARY KEY, name TEXT NOT NULL,"
                                    "  manager TEXT NOT NULL)")},
                    err);
}

// 读取一个标量整数（执行 sql，取结果集第 1 行第 1 列）。
//   找不到/查询失败 → 返回 INT_MIN 作为哨兵。剧本用它在各节点库上读同一字段做收敛比对：
//   各节点都返回相同整数即「已收敛」；某节点返回 INT_MIN 则说明该行尚未到达（也算不一致）。
int readInt(const QString& dbPath, const QString& sql) {
    int v = INT_MIN;  // 哨兵：未读到任何值
    const QString conn =
        QStringLiteral("s1_r_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        if (db.open()) {
            QSqlQuery q(db);
            if (q.exec(sql) && q.next())  // 执行成功且至少一行 → 取第 0 列
                v = q.value(0).toInt();
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(conn);
    return v;
}

// RowMutation 辅助：把一行 employees 数据封装成同步引擎可消费的「行级变更」。
//   mode=DoUpdate（UPSERT 语义）：主键已存在则更新、不存在则插入——这正是同步「以权威值
//   覆盖本地」所需。pkColumns 指明用 id 判定「是否同一行」。empMut/deptMut 是两表各一个版本。
RowMutation empMut(int id, const QString& name, const QString& dept, int salary) {
    RowMutation m;
    m.table = QStringLiteral("employees");
    m.columns = QStringList{QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("dept"),
                            QStringLiteral("salary")};
    m.values = QVariantList{id, name, dept, salary};
    m.pkColumns = QStringList{QStringLiteral("id")};
    m.mode = UpsertMode::DoUpdate;
    return m;
}
RowMutation deptMut(int id, const QString& name, const QString& manager) {
    RowMutation m;
    m.table = QStringLiteral("departments");
    m.columns =
        QStringList{QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("manager")};
    m.values = QVariantList{id, name, manager};
    m.pkColumns = QStringList{QStringLiteral("id")};
    m.mode = UpsertMode::DoUpdate;
    return m;
}

// 从指定库读取全部行 → RowMutation 列表（employees 先、departments 后）。
//   用途：把「指定库（权威源）」当前的全量数据物化成一批 UPSERT 变更，交给某节点
//   engine->write() 写入并经 session 捕获——这就是剧本里「连接指定库导入权威数据」的实现。
//   表顺序 employees→departments 是刻意的：保证有外键依赖时父/子写入顺序合理。
QList<RowMutation> readDesignatedMutations(const QString& designatedPath) {
    QList<RowMutation> muts;
    const QString conn =
        QStringLiteral("s1_imp_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(designatedPath);
        if (db.open()) {
            QSqlQuery q(db);
            if (q.exec(QStringLiteral("SELECT id,name,dept,salary FROM employees ORDER BY id")))
                while (q.next())
                    muts.append(empMut(q.value(0).toInt(), q.value(1).toString(),
                                       q.value(2).toString(), q.value(3).toInt()));
            QSqlQuery q2(db);
            if (q2.exec(QStringLiteral("SELECT id,name,manager FROM departments ORDER BY id")))
                while (q2.next())
                    muts.append(deptMut(q2.value(0).toInt(), q2.value(1).toString(),
                                        q2.value(2).toString()));
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(conn);
    return muts;
}

// 轮询引擎状态直至「落定」或超时。
//   ISyncEngine 的前台 sync() 是异步的（受理后还要等对端 ACK），故剧本每次 sync() 后都用
//   本函数自旋等待终态。每 80ms 轮询一次 state()：
//     · Completed —— 本轮收发成功收束 → 返回 true；
//     · Idle      —— 无事可做（视为成功，没有要等的 ACK）→ 返回 true；
//     · Failed    —— 出错 → 返回 false；
//     · 其它过渡态（Importing/Exporting…）—— 继续等。
//   超时（默认 12s）仍未落定 → 返回 false。返回值即「本轮是否顺利」。
bool waitForEngine(ISyncEngine* engine, int timeoutMs = 12000) {
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        const SyncState st = engine->state();
        if (st == SyncState::Completed || st == SyncState::Idle || st == SyncState::Failed)
            return st != SyncState::Failed;  // Failed 返回 false，其余终态返回 true
        QThread::msleep(80);                 // 让出 CPU，避免忙轮询空转
    }
    return false;  // 超时未落定
}
}  // namespace

// ── 构造 ─────────────────────────────────────────────────────────────────────
Scenario1Runner::Scenario1Runner(const QString& ws, QObject* parent) : QThread(parent), ws_(ws) {
    // gridReady 信号在工作线程 emit、GUI 线程接收 → 队列连接需对参数类型做元类型封送。
    // QString/QStringList/int/bool 为内置已注册类型，但 QVector<QStringList> 未注册，
    // 不注册则 Qt 会丢弃该队列调用（onGrid 永不触发，收敛网格空白）。此处注册之。
    qRegisterMetaType<QVector<QStringList>>("QVector<QStringList>");
}

// ── 编排主体 ─────────────────────────────────────────────────────────────────
// 四阶段剧本的总入口（在后台线程执行）。结构：准备(0~6) → Phase1 → Phase2 → Phase3 →
//   Phase4 收敛校验 → 清理。任一准备步失败即 emit runFinished(false) 提前收场。
// 全程靠 log()/phaseChanged()/gridReady() 信号向 GUI 播报，靠 QThread::msleep 等待真实
//   的文件/网络往返完成（剧本是「时序驱动」的，不做精确的事件等待，故各处 sleep 是经验值）。
void Scenario1Runner::run() {
    const QString designatedDb = ws_ + QStringLiteral("/designated.db");  // 指定数据库（权威源）
    const QString centerDb = ws_ + QStringLiteral("/center.db");
    const QString edgeBDb = ws_ + QStringLiteral("/edge_b.db");
    const QString edgeCDb = ws_ + QStringLiteral("/edge_c.db");
    const QString edgeDDb = ws_ + QStringLiteral("/edge_d.db");

    // —— 0. 清理 workspace ——
    // 每次运行都从干净状态开始：删掉旧库文件，并为每个节点重建 outbox/inbox/quarantine
    //   三个同步目录（传输层据此收发工件、隔离坏件）。先 removeRecursively 再 mkpath，
    //   保证残留的旧工件不会污染本轮剧本（否则上次的 changelog 会被误当成本次输入）。
    emit phaseChanged(QStringLiteral("准备：清理 workspace、建指定库与四节点"));
    QDir().mkpath(ws_);
    for (const QString& f : {designatedDb, centerDb, edgeBDb, edgeCDb, edgeDDb})
        QFile::remove(f);  // 删旧库文件
    for (const QString& node : {QStringLiteral("center"), QStringLiteral("edge_b"),
                                QStringLiteral("edge_c"), QStringLiteral("edge_d")}) {
        QDir(ws_ + "/" + node).removeRecursively();  // 清空该节点旧的收发目录
        for (const QString& sub :
             {QStringLiteral("outbox"), QStringLiteral("inbox"), QStringLiteral("quarantine")})
            QDir().mkpath(ws_ + "/" + node + "/" + sub);  // 重建三个空目录
    }
    log(QStringLiteral("workspace: %1").arg(ws_));

    // —— 1. 建指定数据库（全域权威数据源）——
    QString err;
    if (!setupSchema(designatedDb, &err)) {
        log(QStringLiteral("建指定库失败：%1").arg(err), 2);
        emit runFinished(false);
        return;
    }
    if (!execSqls(designatedDb,
                  {QStringLiteral("INSERT INTO employees VALUES (1,'张三','研发',30000)"),
                   QStringLiteral("INSERT INTO employees VALUES (2,'李四','产品',28000)"),
                   QStringLiteral("INSERT INTO employees VALUES (3,'王五','研发',32000)"),
                   QStringLiteral("INSERT INTO departments VALUES (1,'研发','张三')"),
                   QStringLiteral("INSERT INTO departments VALUES (2,'产品','李四')")},
                  &err)) {
        log(QStringLiteral("灌入指定库数据失败：%1").arg(err), 2);
        emit runFinished(false);
        return;
    }
    log(QStringLiteral("指定数据库就绪：张三=30000 李四=28000 王五=32000（初始权威基线）"));

    // —— 2. 建四节点 schema（edge 初始为空，模拟冷接入）——
    for (const QString& db : {centerDb, edgeBDb, edgeCDb, edgeDDb}) {
        if (!setupSchema(db, &err)) {
            log(QStringLiteral("建节点库失败：%1").arg(err), 2);
            emit runFinished(false);
            return;
        }
    }
    log(QStringLiteral("四节点 schema 建立完成（B/C/D 暂无数据）"));

    // —— 3. 打开 DataBridge ——
    DataBridge centerBridge, edgeBBridge, edgeCBridge, edgeDBridge;
    auto openBridge = [&](DataBridge& b, const QString& path, const char* name) -> bool {
        ConnectionSpec spec;
        spec.sqlitePath = path;
        spec.enableWal = true;
        if (!b.open(spec, &err)) {
            log(QStringLiteral("%1 DataBridge 打开失败：%2").arg(name, err), 2);
            return false;
        }
        return true;
    };
    if (!openBridge(centerBridge, centerDb, "center") ||
        !openBridge(edgeBBridge, edgeBDb, "edge_b") ||
        !openBridge(edgeCBridge, edgeCDb, "edge_c") ||
        !openBridge(edgeDBridge, edgeDDb, "edge_d")) {
        emit runFinished(false);
        return;
    }

    // —— 4. 构建 SyncConfig（SourceWins + 全局唯一 rank）——
    // 用 Builder 链式构建每个节点的同步配置。关键项：
    //   · conflictPolicy(SourceWins)：冲突时「按来源优先级裁决」（配合下面的 originPriority）。
    //   · originPriority(node, rank)：四个节点的全局 rank 表，center=100 最高 → 指定库/中心
    //     的值在冲突中恒胜出，这正是「以指定数据为准」的机制根基。四节点配的是同一张表。
    //   · outbox/inbox/quarantineDir：该节点的工件收发与隔离目录（与 UDP 传输层约定一致）。
    //   · broadcastIntervalMs(300)/ackMaxDelayMs(10000)：广播节拍与 ACK 最长等待。
    //   centerId 非空者会设 centerNodeId（edge 节点指向 center）；peers 为其可达对端。
    auto buildCfg = [&](const QString& nodeId, NodeRole role, const QString& centerId,
                        const QStringList& peers, const QString& dbPath) -> SyncConfig {
        auto b = SyncConfig::Builder()
                     .nodeId(nodeId)
                     .role(role)
                     .database(dbPath)
                     .outboxDir(ws_ + "/" + nodeId + "/outbox")
                     .inboxDir(ws_ + "/" + nodeId + "/inbox")
                     .quarantineDir(ws_ + "/" + nodeId + "/quarantine")
                     .conflictPolicy(ConflictPolicy::SourceWins)
                     .originPriority(QStringLiteral("center"), RANK_CENTER)
                     .originPriority(QStringLiteral("edge_b"), RANK_EDGE_B)
                     .originPriority(QStringLiteral("edge_c"), RANK_EDGE_C)
                     .originPriority(QStringLiteral("edge_d"), RANK_EDGE_D)
                     .broadcastIntervalMs(300)
                     .ackMaxDelayMs(10000);
        if (!centerId.isEmpty())
            b.centerNodeId(centerId);
        for (const QString& p : peers)
            b.addPeerNode(p);
        return b.build(&err);
    };

    SyncConfig centerCfg = buildCfg(
        QStringLiteral("center"), NodeRole::Center, {},
        {QStringLiteral("edge_b"), QStringLiteral("edge_c"), QStringLiteral("edge_d")}, centerDb);
    SyncConfig edgeBCfg = buildCfg(QStringLiteral("edge_b"), NodeRole::Edge,
                                   QStringLiteral("center"), {QStringLiteral("center")}, edgeBDb);
    SyncConfig edgeCCfg = buildCfg(QStringLiteral("edge_c"), NodeRole::Edge,
                                   QStringLiteral("center"), {QStringLiteral("center")}, edgeCDb);
    SyncConfig edgeDCfg = buildCfg(QStringLiteral("edge_d"), NodeRole::Edge,
                                   QStringLiteral("center"), {QStringLiteral("center")}, edgeDDb);
    if (!centerCfg.isValid() || !edgeBCfg.isValid() || !edgeCCfg.isValid() || !edgeDCfg.isValid()) {
        log(QStringLiteral("SyncConfig 构建失败：%1").arg(err), 2);
        emit runFinished(false);
        return;
    }

    // —— 5. 初始化引擎 ——
    auto centerEngine = createSyncEngine(centerBridge);
    auto edgeBEngine = createSyncEngine(edgeBBridge);
    auto edgeCEngine = createSyncEngine(edgeCBridge);
    auto edgeDEngine = createSyncEngine(edgeDBridge);
    struct EnginePair {
        ISyncEngine* e;
        const SyncConfig* c;
        const char* n;
    };
    for (const EnginePair& ep : {EnginePair{centerEngine.get(), &centerCfg, "center"},
                                 EnginePair{edgeBEngine.get(), &edgeBCfg, "edge_b"},
                                 EnginePair{edgeCEngine.get(), &edgeCCfg, "edge_c"},
                                 EnginePair{edgeDEngine.get(), &edgeDCfg, "edge_d"}}) {
        if (!ep.e->initialize(*ep.c, &err)) {
            log(QStringLiteral("%1 引擎初始化失败：%2").arg(ep.n, err), 2);
            emit runFinished(false);
            return;
        }
    }
    log(QStringLiteral(
        "四节点同步引擎初始化完成（SourceWins，rank：center100 / B70 / C50 / D30）"));

    // —— 6. 启动 UDP 传输层 ——
    // 每个节点一个 UdpFileTransport：它监听本节点端口、把 outbox 里的工件经 UDP 发到对端、
    //   收到的工件落到本节点 inbox 供引擎应用。全部走回环地址 127.0.0.1（同机多节点模拟）。
    //   中心 A 的对端是一张表（B/C/D 各自 ip:port）；各 edge 的对端只有 center（星型拓扑）。
    //   start() 后 sleep 150ms 给套接字绑定/线程起转留出时间，再开始剧本，避免首批工件丢失。
    const QHostAddress loopback(QHostAddress::LocalHost);
    UdpFileTransport centerTransport(PORT_CENTER,
                                     QHash<QString, UdpPeerEndpoint>{
                                         {QStringLiteral("edge_b"), {loopback, PORT_EDGE_B}},
                                         {QStringLiteral("edge_c"), {loopback, PORT_EDGE_C}},
                                         {QStringLiteral("edge_d"), {loopback, PORT_EDGE_D}},
                                     },
                                     ws_ + "/center/outbox", ws_ + "/center/inbox");
    UdpFileTransport edgeBTransport(PORT_EDGE_B, loopback, PORT_CENTER, ws_ + "/edge_b/outbox",
                                    ws_ + "/edge_b/inbox");
    UdpFileTransport edgeCTransport(PORT_EDGE_C, loopback, PORT_CENTER, ws_ + "/edge_c/outbox",
                                    ws_ + "/edge_c/inbox");
    UdpFileTransport edgeDTransport(PORT_EDGE_D, loopback, PORT_CENTER, ws_ + "/edge_d/outbox",
                                    ws_ + "/edge_d/inbox");
    centerTransport.start();
    edgeBTransport.start();
    edgeCTransport.start();
    edgeDTransport.start();
    QThread::msleep(150);
    log(QStringLiteral("UDP 传输层就绪：center:%1 ⇄ B:%2 / C:%3 / D:%4")
            .arg(PORT_CENTER)
            .arg(PORT_EDGE_B)
            .arg(PORT_EDGE_C)
            .arg(PORT_EDGE_D));

    // ════════════════════════════════════════════════════════════════════════
    // Phase 1：指定库基线下发（中心A 连接指定库导入 → 广播 → B/C/D 追平）
    // ════════════════════════════════════════════════════════════════════════
    emit phaseChanged(QStringLiteral("Phase 1 · 指定库基线下发（中心A → B/C/D）"));
    log(QStringLiteral("中心A 连接指定数据库，导入全部权威数据 → changelog ..."));
    // write() 把权威数据写进中心 A 的库并经 session 捕获进 changelog（这是「能被同步出去」
    //   的正确写法，区别于裸 SQL 直写）。失败即中止剧本。
    if (!centerEngine->write(readDesignatedMutations(designatedDb), &err)) {
        log(QStringLiteral("中心A 导入指定库失败：%1").arg(err), 2);
        emit runFinished(false);
        return;
    }
    centerEngine->sync(&err);  // 触发一轮广播：把刚捕获的 changelog 打包发给 B/C/D
    waitForEngine(centerEngine.get());  // 等本轮广播落定（收齐 ACK 或超时）
    QThread::msleep(900);  // 再额外等三个 edge 完成应用并回 ACK（真实文件/网络往返的经验等待）
    log(QStringLiteral("中心A 广播完成；B/C/D 已从指定库基线追平"));
    log(QStringLiteral("当前 张三薪资 —— A:%1 B:%2 C:%3 D:%4")
            .arg(readInt(centerDb, "SELECT salary FROM employees WHERE id=1"))
            .arg(readInt(edgeBDb, "SELECT salary FROM employees WHERE id=1"))
            .arg(readInt(edgeCDb, "SELECT salary FROM employees WHERE id=1"))
            .arg(readInt(edgeDDb, "SELECT salary FROM employees WHERE id=1")));

    // 刷清四端 changelog，确保进入 Phase 2 时全域处于一致基线（张三=30000）。
    //   依次让每个节点再 sync 一轮，把各自 inbox/outbox 里残留的待办收发干净，避免上一阶段
    //   未抽干的工件在 Phase 2 里迟到、干扰冲突仲裁的观察。顺序 B/C/D 后 center 无特殊含义。
    for (ISyncEngine* e :
         {edgeBEngine.get(), edgeCEngine.get(), edgeDEngine.get(), centerEngine.get()}) {
        e->sync(&err);
        waitForEngine(e);
    }
    QThread::msleep(500);

    // ════════════════════════════════════════════════════════════════════════
    // Phase 2：并发冲突 · 以指定为准（子节点B 离线改小 → 指定库更新 → 全域以指定值收敛）
    // ════════════════════════════════════════════════════════════════════════
    emit phaseChanged(QStringLiteral("Phase 2 · 并发冲突，以指定数据为准（rank 仲裁）"));
    // 第①步：子节点 B（rank70）本地把张三薪资改成 15000 并上行推给中心 A。
    //   这模拟「边缘节点的本地误改」——它会与稍后中心 A 的权威值在同一行(id=1)上撞车。
    log(QStringLiteral("子节点B 离线把 张三薪资 改为 15000（与指定库冲突的本地误改）"));
    edgeBEngine->write({empMut(1, QStringLiteral("张三"), QStringLiteral("研发"), 15000)}, &err);
    edgeBEngine->sync(&err);
    waitForEngine(edgeBEngine.get());
    QThread::msleep(700);  // 等中心A 接收并暂存 B 的离线值（制造「并发两版本」的局面）

    // 第②步：指定库（权威源）把张三薪资更新为 33000（新权威值）。裸 SQL 直改指定库——
    //   指定库不是同步节点，它只是中心 A 即将导入的数据来源。
    log(QStringLiteral("指定数据库更新：张三薪资 → 33000（新权威值）"));
    execSqls(designatedDb, {QStringLiteral("UPDATE employees SET salary=33000 WHERE id=1")}, &err);

    // 第③步：中心 A（rank100）从指定库导入 33000 并广播。此时同一行存在两个竞争版本：
    //   B 的 15000（rank70）与 A 的 33000（rank100）。SourceWins 策略按 rank 裁决 → A 胜出。
    log(QStringLiteral("中心A 重新连接指定库，导入新权威值 33000 并向全域广播 ..."));
    centerEngine->write({empMut(1, QStringLiteral("张三"), QStringLiteral("研发"), 33000)}, &err);
    centerEngine->sync(&err);
    waitForEngine(centerEngine.get());
    QThread::msleep(1100);  // 等三端各自完成冲突仲裁（center rank100 胜出，覆盖 B 的 15000）

    log(QStringLiteral("冲突仲裁完成：中心A（指定库权威，rank100）胜出，覆盖子节点B 的 15000"));
    log(QStringLiteral("当前 张三薪资 —— A:%1 B:%2 C:%3 D:%4（应均为 33000）")
            .arg(readInt(centerDb, "SELECT salary FROM employees WHERE id=1"))
            .arg(readInt(edgeBDb, "SELECT salary FROM employees WHERE id=1"))
            .arg(readInt(edgeCDb, "SELECT salary FROM employees WHERE id=1"))
            .arg(readInt(edgeDDb, "SELECT salary FROM employees WHERE id=1")));

    // ════════════════════════════════════════════════════════════════════════
    // Phase 3：子节点重连指定库自我纠正（节点处亦以指定数据为准）
    // ════════════════════════════════════════════════════════════════════════
    emit phaseChanged(QStringLiteral("Phase 3 · 子节点C 重连指定库重导入，本地被指定值覆盖"));
    // 第①步：子节点 C 本地把李四薪资误改为 9999（暂不 sync，纯本地脏值）。
    log(QStringLiteral("子节点C 离线把 李四薪资 改为 9999（本地误改）"));
    edgeCEngine->write({empMut(2, QStringLiteral("李四"), QStringLiteral("产品"), 9999)}, &err);
    QThread::msleep(200);
    // 第②步：C 重新连接指定库、重导入全量权威数据——指定库里李四仍是 28000，于是本地的
    //   9999 被权威值 28000 覆盖（节点处「以指定数据为准」的自我纠正）。
    log(QStringLiteral("子节点C 重新连接指定数据库重导入 → 李四 9999 被指定值 28000 覆盖"));
    edgeCEngine->write(readDesignatedMutations(designatedDb), &err);
    edgeCEngine->sync(&err);  // C 把纠正后的值上行
    waitForEngine(edgeCEngine.get());
    QThread::msleep(700);
    centerEngine->sync(&err);  // 中心再广播一轮，确保全域看到一致结果
    waitForEngine(centerEngine.get());
    QThread::msleep(700);
    log(QStringLiteral("子节点C 已自我纠正并与全域保持一致"));

    // ════════════════════════════════════════════════════════════════════════
    // Phase 4：收敛校验
    // ════════════════════════════════════════════════════════════════════════
    emit phaseChanged(QStringLiteral("Phase 4 · 全域收敛校验"));
    // 收敛校验：对若干「探测字段」逐个在 指定库/A/B/C/D 五处读同一查询，看五个值是否全等。
    //   全部字段在五处都一致 → allConverged=true → 剧本成功。结果组装成网格经 gridReady 给 GUI。
    struct Row {
        QString field;  // 字段中文名（展示用）
        QString sql;    // 读该字段的 SQL（在每个库上各执行一次）
    };
    const QList<Row> probe = {
        {QStringLiteral("张三 薪资"), QStringLiteral("SELECT salary FROM employees WHERE id=1")},
        {QStringLiteral("李四 薪资"), QStringLiteral("SELECT salary FROM employees WHERE id=2")},
        {QStringLiteral("王五 薪资"), QStringLiteral("SELECT salary FROM employees WHERE id=3")},
    };
    QVector<QStringList> grid;  // 收敛网格的行集合（每个探测字段一行）
    bool allConverged = true;   // 全字段、全节点是否都一致
    for (const Row& r : probe) {
        // 在五个库上各读一次同一字段（指定库 d / 中心 a / 边缘 b,c,d）。
        const int d = readInt(designatedDb, r.sql);
        const int a = readInt(centerDb, r.sql);
        const int b = readInt(edgeBDb, r.sql);
        const int c = readInt(edgeCDb, r.sql);
        const int e = readInt(edgeDDb, r.sql);
        // 链式相等：五个值（含权威指定库）必须全等才算该字段收敛。任一为 INT_MIN（未读到）
        //   也会因不等而判为不一致。
        const bool same = (d == a && a == b && b == c && c == e);
        allConverged = allConverged && same;  // 累积「与」：有一个字段不收敛则整体失败
        grid.append(QStringList{r.field, QString::number(d), QString::number(a), QString::number(b),
                                QString::number(c), QString::number(e),
                                same ? QStringLiteral("✓ 一致") : QStringLiteral("✗ 不一致")});
    }
    emit gridReady(
        QStringList{QStringLiteral("字段"), QStringLiteral("指定库"), QStringLiteral("中心A"),
                    QStringLiteral("子节点B"), QStringLiteral("子节点C"), QStringLiteral("子节点D"),
                    QStringLiteral("收敛")},
        grid,
        allConverged ? QStringLiteral("全域已收敛到指定数据库的权威值（以指定数据为准 ✓）")
                     : QStringLiteral("存在未收敛字段（请查看日志）"));
    log(allConverged ? QStringLiteral("✅ 全域收敛成功：指定库 = A = B = C = D")
                     : QStringLiteral("❌ 存在未收敛字段"),
        allConverged ? 0 : 2);

    succeeded_ = allConverged;  // 落定剧本最终结论，供 succeeded() 读取

    // —— 清理 ——
    // 拆解次序很重要：先 reset 同步引擎（停各自后台 worker、解除对库的占用），再 close
    //   DataBridge（关物理库连接），最后请求停止并 join 四个 UDP 传输线程。顺序保证「先停
    //   用库的人、再关库、最后关网络」，避免传输线程在引擎/库已销毁后还往里投递工件。
    centerEngine.reset();
    edgeBEngine.reset();
    edgeCEngine.reset();
    edgeDEngine.reset();
    centerBridge.close();
    edgeBBridge.close();
    edgeCBridge.close();
    edgeDBridge.close();
    centerTransport.requestStop();  // 置停止标志（协作式）
    edgeBTransport.requestStop();
    edgeCTransport.requestStop();
    edgeDTransport.requestStop();
    centerTransport.wait();  // 阻塞等四个传输线程真正退出
    edgeBTransport.wait();
    edgeCTransport.wait();
    edgeDTransport.wait();

    emit runFinished(succeeded_);  // 通知 GUI 剧本结束及成败
}
