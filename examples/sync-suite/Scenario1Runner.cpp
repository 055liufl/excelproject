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
                    if (err)
                        *err = q.lastError().text() + QStringLiteral(" SQL: ") + s.left(100);
                    ok = false;
                    break;
                }
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(conn);
    return ok;
}

// 建节点/指定库 schema（employees + departments）。
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

// 读取标量整数（找不到返回 INT_MIN）。
int readInt(const QString& dbPath, const QString& sql) {
    int v = INT_MIN;
    const QString conn =
        QStringLiteral("s1_r_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        if (db.open()) {
            QSqlQuery q(db);
            if (q.exec(sql) && q.next())
                v = q.value(0).toInt();
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(conn);
    return v;
}

// RowMutation 辅助。
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

// 轮询引擎直至终止或超时。
bool waitForEngine(ISyncEngine* engine, int timeoutMs = 12000) {
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        const SyncState st = engine->state();
        if (st == SyncState::Completed || st == SyncState::Idle || st == SyncState::Failed)
            return st != SyncState::Failed;
        QThread::msleep(80);
    }
    return false;
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
void Scenario1Runner::run() {
    const QString designatedDb = ws_ + QStringLiteral("/designated.db");  // 指定数据库（权威源）
    const QString centerDb = ws_ + QStringLiteral("/center.db");
    const QString edgeBDb = ws_ + QStringLiteral("/edge_b.db");
    const QString edgeCDb = ws_ + QStringLiteral("/edge_c.db");
    const QString edgeDDb = ws_ + QStringLiteral("/edge_d.db");

    // —— 0. 清理 workspace ——
    emit phaseChanged(QStringLiteral("准备：清理 workspace、建指定库与四节点"));
    QDir().mkpath(ws_);
    for (const QString& f : {designatedDb, centerDb, edgeBDb, edgeCDb, edgeDDb})
        QFile::remove(f);
    for (const QString& node : {QStringLiteral("center"), QStringLiteral("edge_b"),
                                QStringLiteral("edge_c"), QStringLiteral("edge_d")}) {
        QDir(ws_ + "/" + node).removeRecursively();
        for (const QString& sub :
             {QStringLiteral("outbox"), QStringLiteral("inbox"), QStringLiteral("quarantine")})
            QDir().mkpath(ws_ + "/" + node + "/" + sub);
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
    if (!centerEngine->write(readDesignatedMutations(designatedDb), &err)) {
        log(QStringLiteral("中心A 导入指定库失败：%1").arg(err), 2);
        emit runFinished(false);
        return;
    }
    centerEngine->sync(&err);
    waitForEngine(centerEngine.get());
    QThread::msleep(900);  // 等三个 edge 应用并回 ACK
    log(QStringLiteral("中心A 广播完成；B/C/D 已从指定库基线追平"));
    log(QStringLiteral("当前 张三薪资 —— A:%1 B:%2 C:%3 D:%4")
            .arg(readInt(centerDb, "SELECT salary FROM employees WHERE id=1"))
            .arg(readInt(edgeBDb, "SELECT salary FROM employees WHERE id=1"))
            .arg(readInt(edgeCDb, "SELECT salary FROM employees WHERE id=1"))
            .arg(readInt(edgeDDb, "SELECT salary FROM employees WHERE id=1")));

    // 刷清四端 changelog，确保进入 Phase 2 时全域处于一致基线（张三=30000）。
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
    log(QStringLiteral("子节点B 离线把 张三薪资 改为 15000（与指定库冲突的本地误改）"));
    edgeBEngine->write({empMut(1, QStringLiteral("张三"), QStringLiteral("研发"), 15000)}, &err);
    edgeBEngine->sync(&err);
    waitForEngine(edgeBEngine.get());
    QThread::msleep(700);  // 等中心A 接收 B 的离线值

    log(QStringLiteral("指定数据库更新：张三薪资 → 33000（新权威值）"));
    execSqls(designatedDb, {QStringLiteral("UPDATE employees SET salary=33000 WHERE id=1")}, &err);

    log(QStringLiteral("中心A 重新连接指定库，导入新权威值 33000 并向全域广播 ..."));
    centerEngine->write({empMut(1, QStringLiteral("张三"), QStringLiteral("研发"), 33000)}, &err);
    centerEngine->sync(&err);
    waitForEngine(centerEngine.get());
    QThread::msleep(1100);  // 等三端各自完成冲突仲裁（center rank100 胜出）

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
    log(QStringLiteral("子节点C 离线把 李四薪资 改为 9999（本地误改）"));
    edgeCEngine->write({empMut(2, QStringLiteral("李四"), QStringLiteral("产品"), 9999)}, &err);
    QThread::msleep(200);
    log(QStringLiteral("子节点C 重新连接指定数据库重导入 → 李四 9999 被指定值 28000 覆盖"));
    edgeCEngine->write(readDesignatedMutations(designatedDb), &err);
    edgeCEngine->sync(&err);
    waitForEngine(edgeCEngine.get());
    QThread::msleep(700);
    centerEngine->sync(&err);
    waitForEngine(centerEngine.get());
    QThread::msleep(700);
    log(QStringLiteral("子节点C 已自我纠正并与全域保持一致"));

    // ════════════════════════════════════════════════════════════════════════
    // Phase 4：收敛校验
    // ════════════════════════════════════════════════════════════════════════
    emit phaseChanged(QStringLiteral("Phase 4 · 全域收敛校验"));
    struct Row {
        QString field;
        QString sql;
    };
    const QList<Row> probe = {
        {QStringLiteral("张三 薪资"), QStringLiteral("SELECT salary FROM employees WHERE id=1")},
        {QStringLiteral("李四 薪资"), QStringLiteral("SELECT salary FROM employees WHERE id=2")},
        {QStringLiteral("王五 薪资"), QStringLiteral("SELECT salary FROM employees WHERE id=3")},
    };
    QVector<QStringList> grid;
    bool allConverged = true;
    for (const Row& r : probe) {
        const int d = readInt(designatedDb, r.sql);
        const int a = readInt(centerDb, r.sql);
        const int b = readInt(edgeBDb, r.sql);
        const int c = readInt(edgeCDb, r.sql);
        const int e = readInt(edgeDDb, r.sql);
        const bool same = (d == a && a == b && b == c && c == e);
        allConverged = allConverged && same;
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

    succeeded_ = allConverged;

    // —— 清理 ——
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

    emit runFinished(succeeded_);
}
