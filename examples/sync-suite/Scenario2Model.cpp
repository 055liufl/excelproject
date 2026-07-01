#include "Scenario2Model.h"

#include "dbridge/SchemaInfo.h"
#include "dbridge/Types.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QThread>
#include <QUuid>

#include "Scenario2SnapshotService.h"  // 快照编解码 + CenterSnapshotResponder
#include "udp_transport.h"             // 复用场景1 的 UDP 文件传输层

using namespace dbridge;
using namespace dbridge::sync;

// ── 两类 UDP 数据的严格区分（重要设计约束）──────────────────────────────────
//
// 本 demo 里经 UDP 流动的数据有两类，必须在每一层都能区分、互不干扰：
//
//   ① 数据库同步数据（SyncEngine 的 changeset/baseline/ack 等变更集工件）
//        · 目录：ws/outbox、ws/inbox、ws/quarantine（SyncConfig 配置的引擎收发目录）
//        · 文件名：{origin}__…__changeset__… / baselineresponse / blreq / ack（SyncDDL 命名契约）
//        · 场景2 中 B 不与任何同步节点组网，故这些工件留在本地目录、不接传输层。
//
//   ② 远端快照数据（本文件新增，供"类 Beyond Compare"比对用）
//        · 目录：ws/snap_child/{outbox,inbox}（B 侧）、ws/snap_center/{outbox,inbox}（A 侧）
//        · 端口：A=15201 / B=15202（独立于场景1 的 15101-15104、sync-demo 的 15001-15004）
//        · 文件名：snapreq__<id>.payload（请求）/ snapresp__<id>.payload（响应）
//        · 由本文件专属的一对 UdpFileTransport 搬运，与 ① 的引擎目录/端口/传输实例完全隔离。
//
// 三重隔离（目录 + 端口/传输实例 + 文件名前缀）确保：同步变更集绝不会被当成快照、快照也绝不会
// 被同步引擎误当成变更集应用；UdpFileTransport::extractTargetPeer 对 snapreq/snapresp 返回空，
// 即便未来两类共用一个传输实例也不会被同步路由误投递。
namespace {
constexpr quint16 PORT_CENTER_A = 15201;  // 中心A 快照服务监听端口
constexpr quint16 PORT_CHILD_B = 15202;   // 子节点B 监听端口
constexpr int kSnapshotTimeoutMs = 6000;  // 等待 A 回传快照的超时（loopback 正常仅数十毫秒）

// 快照通道目录（相对 ws_）——集中定义，避免各处字符串拼写漂移。
inline QString snapChildOutbox(const QString& ws) {
    return ws + QStringLiteral("/snap_child/outbox");
}
inline QString snapChildInbox(const QString& ws) {
    return ws + QStringLiteral("/snap_child/inbox");
}
inline QString snapCenterOutbox(const QString& ws) {
    return ws + QStringLiteral("/snap_center/outbox");
}
inline QString snapCenterInbox(const QString& ws) {
    return ws + QStringLiteral("/snap_center/inbox");
}
}  // namespace

// ── 分隔符（行键/单元键编码用，普通数据不会包含 \x01 / \x02）──────────────────
// 为什么选控制字符 0x01/0x02 作分隔：表名/主键/列名等业务字符串几乎不可能含这类不可见控制符，
// 用它们拼成「行键 / 单元键」可避免与真实数据撞车，从而安全地作 QSet 的去重键。
static const QChar kSep1 = QChar(0x01);
static const QChar kSep2 = QChar(0x02);

// rowKey —— 把 (表, 主键) 编码为一个唯一字符串，作为 stagedRows_ 这个 QSet 的成员键。
QString Scenario2Model::rowKey(const QString& table, const QString& pk) {
    return table + kSep1 + pk;
}
// cellKey —— 把 (表, 主键, 列) 编码为单元格级唯一键，作为 stagedCells_ 的成员键。
QString Scenario2Model::cellKey(const QString& table, const QString& pk, const QString& column) {
    return table + kSep1 + pk + kSep2 + column;
}

// 构造：只记录 workspace 路径、推导出 A/B 两库文件路径；
// tables_（参与比对的表清单）不再硬编码——改由 rebuildSession() 从"A 经 UDP 回传的快照"里
// 动态填充（需求 #3：表名/数量来自数据库，不写死在代码里）。
// 不打开任何库、不建会话（重活留给 setup()，构造保持零成本不可失败）。
Scenario2Model::Scenario2Model(const QString& ws)
    : ws_(ws),
      centerDb_(ws + QStringLiteral("/center_A.db")),
      childDb_(ws + QStringLiteral("/child_B.db")) {
}

Scenario2Model::~Scenario2Model() {
    // 先析构会话与引擎，再关闭 bridge，确保 SyncContext / 连接被正确释放。
    session_.reset();
    childEngine_.reset();
    if (engineReady_)
        childBridge_.close();
    // 最后停掉 UDP 快照通道（responder + 两个传输线程），避免线程在库/对象销毁后仍在跑。
    stopSnapshotChannel();
}

// ── 公共连接工具：建库/读表/读列/读主键 ──────────────────────────────────────
// 共性：这几个 static 工具都「用一次性的唯一连接打开任意库 → 查 → 关 → removeDatabase」，
// 不依赖本对象状态，故可对 A 库或 B 库随意调用。每个都把 QSqlDatabase 句柄放在内层作用域里，
// 确保它先析构、再 removeDatabase（Qt 要求移除连接前不得有活动副本，否则告警且移除不彻底）。

// readRows —— 读取任意库某表的全部行，每行以「列名→值」的 QVariantMap 表示。
// 做什么：按主键 ORDER BY 取全表（主键存在时），逐行把每列塞进 QVariantMap。
// 为什么按主键排序：两栏对比视图要让 A、B 两侧行序稳定可对齐，否则左右行对不上。
// 参数：dbPath 库文件；table 表名。返回：行列表（库打不开/查询失败则返回空列表，调用方容错）。
QList<QVariantMap> Scenario2Model::readRows(const QString& dbPath, const QString& table) {
    QList<QVariantMap> rows;
    const QString conn =
        QStringLiteral("s2_read_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        if (db.open()) {
            QSqlQuery q(db);
            // 以主键排序，保证两栏视图行序稳定可对齐。
            const QString pk = readPkColumn(dbPath, table);
            QString sql = QStringLiteral("SELECT * FROM \"%1\"").arg(QString(table));
            if (!pk.isEmpty())
                sql += QStringLiteral(" ORDER BY \"%1\"").arg(pk);
            if (q.exec(sql)) {
                while (q.next()) {
                    QVariantMap row;
                    for (int i = 0; i < q.record().count(); ++i)
                        row.insert(q.record().fieldName(i), q.value(i));
                    rows.append(row);
                }
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(conn);
    return rows;
}

// readPkColumn —— 读任意库某表的主键列名（PRAGMA table_info 里 pk>0 的第一列）。
// 说明：本演示各表都是单列整型主键，故取第一个 pk>0 的列即可；找不到则返回空串。
QString Scenario2Model::readPkColumn(const QString& dbPath, const QString& table) {
    QString pk;
    const QString conn =
        QStringLiteral("s2_pk_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        if (db.open()) {
            QSqlQuery q(db);
            if (q.exec(QStringLiteral("PRAGMA table_info(\"%1\")").arg(QString(table)))) {
                while (q.next()) {
                    if (q.value(QStringLiteral("pk")).toInt() > 0) {
                        pk = q.value(QStringLiteral("name")).toString();
                        break;
                    }
                }
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(conn);
    return pk;
}

// columns / pkColumn —— 该表的列序 / 主键列名，改用 dbridge 公共接口 describeTable() 取得
//   （需求 #3：字段从数据库读取、且由 dbridge 对外提供接口，而非 demo 自己写 PRAGMA）。
//   取自 B 库的 schema（A 与之同构）：B 是当前节点、双栏对比以 B 的列序展示。
QStringList Scenario2Model::columns(const QString& table) {
    TableSchema ts;
    if (!childBridge_.describeTable(table, &ts))
        return {};
    QStringList cols;
    for (const ColumnDef& c : ts.columns)
        cols.append(c.name);
    return cols;
}
QString Scenario2Model::pkColumn(const QString& table) {
    TableSchema ts;
    if (!childBridge_.describeTable(table, &ts))
        return {};
    const QStringList pk = ts.primaryKeyColumns();
    return pk.isEmpty() ? QString() : pk.first();
}

// ── seed：建表 + 灌入"有意制造差异"的数据 ──────────────────────────────────────
//
// center_A（远端）与 child_B（本地）的数据故意构造为：
//   employee  —— 不同(红)：id=1 薪资改、id=3 部门+薪资+职级改、id=5 仅A有、id=4 仅B有
//   department—— 相同(绿)
//   project   —— 不同(红)：id=1、id=3 状态改
//   region    —— 相同(绿)

// execAll —— 在指定库上顺序执行一组 SQL（建表/灌数据用）；任一失败即带错误信息中止。
// 为什么先 PRAGMA foreign_keys=OFF：seed 时插入顺序未必满足外键依赖，关掉外键约束以免无谓失败
//   （本演示重点在数据差异而非引用完整性）。返回 true=全部成功。
static bool execAll(const QString& dbPath, const QStringList& sqls, QString* err) {
    const QString conn =
        QStringLiteral("s2_seed_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
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
            q.exec(QStringLiteral("PRAGMA foreign_keys=OFF"));
            for (const QString& sql : sqls) {
                if (!q.exec(sql)) {
                    if (err)
                        *err = q.lastError().text() + QStringLiteral("  SQL: ") + sql.left(120);
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

// seedDatabases —— 给 A、B 两库建表并灌入「有意制造差异」的初始数据。
// 做什么：先在两库各建相同 schema，再分别灌入 childData / centerData（差异点见上方表格注释）。
// 返回：任一库的建表或灌数据失败则 false（错误经 err 带出）。这是 setup/reseed 的数据基础。
bool Scenario2Model::seedDatabases(QString* err) {
    // 两库共享的 schema。
    const QStringList schema = {
        QStringLiteral("CREATE TABLE employee ("
                       "  id INTEGER PRIMARY KEY, name TEXT NOT NULL, dept TEXT NOT NULL,"
                       "  salary INTEGER NOT NULL, title TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE department ("
                       "  id INTEGER PRIMARY KEY, name TEXT NOT NULL, manager TEXT NOT NULL,"
                       "  budget INTEGER NOT NULL)"),
        QStringLiteral("CREATE TABLE project ("
                       "  id INTEGER PRIMARY KEY, name TEXT NOT NULL, owner TEXT NOT NULL,"
                       "  status TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE region ("
                       "  id INTEGER PRIMARY KEY, name TEXT NOT NULL, code TEXT NOT NULL)"),
    };

    // 子节点B（本地）数据。
    const QStringList childData = {
        QStringLiteral("INSERT INTO employee VALUES (1,'张三','研发',15000,'工程师')"),
        QStringLiteral("INSERT INTO employee VALUES (2,'李四','产品',18000,'经理')"),
        QStringLiteral("INSERT INTO employee VALUES (3,'王五','测试',20000,'工程师')"),
        QStringLiteral("INSERT INTO employee VALUES (4,'赵六','市场',16000,'专员')"),
        QStringLiteral("INSERT INTO department VALUES (1,'研发','张三',500000)"),
        QStringLiteral("INSERT INTO department VALUES (2,'产品','李四',300000)"),
        QStringLiteral("INSERT INTO department VALUES (3,'测试','王五',200000)"),
        QStringLiteral("INSERT INTO project VALUES (1,'Alpha','张三','进行中')"),
        QStringLiteral("INSERT INTO project VALUES (2,'Beta','李四','已完成')"),
        QStringLiteral("INSERT INTO project VALUES (3,'Gamma','王五','规划中')"),
        QStringLiteral("INSERT INTO region VALUES (1,'华东','CN-E')"),
        QStringLiteral("INSERT INTO region VALUES (2,'华北','CN-N')"),
    };

    // 中心节点A（远端）数据。
    const QStringList centerData = {
        QStringLiteral("INSERT INTO employee VALUES (1,'张三','研发',30000,'工程师')"),  // 薪资改
        QStringLiteral("INSERT INTO employee VALUES (2,'李四','产品',18000,'经理')"),    // 相同
        QStringLiteral(
            "INSERT INTO employee VALUES (3,'王五','研发',26000,'高级工程师')"),  // 部门/薪资/职级改
        QStringLiteral("INSERT INTO employee VALUES (5,'孙七','运营',17000,'专员')"),  // 仅A有
        // id=4 赵六：A 不存在 → 仅B有
        QStringLiteral("INSERT INTO department VALUES (1,'研发','张三',500000)"),
        QStringLiteral("INSERT INTO department VALUES (2,'产品','李四',300000)"),
        QStringLiteral("INSERT INTO department VALUES (3,'测试','王五',200000)"),
        QStringLiteral("INSERT INTO project VALUES (1,'Alpha','张三','已完成')"),  // 状态改
        QStringLiteral("INSERT INTO project VALUES (2,'Beta','李四','已完成')"),   // 相同
        QStringLiteral("INSERT INTO project VALUES (3,'Gamma','王五','进行中')"),  // 状态改
        QStringLiteral("INSERT INTO region VALUES (1,'华东','CN-E')"),
        QStringLiteral("INSERT INTO region VALUES (2,'华北','CN-N')"),
    };

    if (!execAll(childDb_, schema, err) || !execAll(childDb_, childData, err))
        return false;
    if (!execAll(centerDb_, schema, err) || !execAll(centerDb_, centerData, err))
        return false;
    return true;
}

// ── 远端快照获取：改为经 UDP 向中心A 请求（见 fetchRemoteSnapshotsOverUdp / 快照响应线程）──
//   原「buildRemoteSnapshots 直接读 center_A.db」的实现已删除——那不符合真实场景（B 不该持有
//   A 的库文件）。现在 A 的整库快照由 CenterSnapshotResponder 读 A 自己的库、序列化后经 UDP
//   回传给 B（需求 #2）。

// ── setup / reseed / rebuildSession ─────────────────────────────────────────

// setup —— 从零把场景2 带到「可比对」状态（可重入，重入时先彻底拆除旧资源）。
// 六步：拆旧 → 清 workspace → seed 两库 → 开 B 的 DataBridge → 建 B 的 SyncConfig
//        → 初始化 B 的 ISyncEngine（建 SyncContext）→ rebuildSession 建首个比对会话。
// 为什么引擎必须先初始化：createComparisonSession 依赖引擎建立的 SyncContext 与 __sync_* 元数据。
// 返回：任一步失败即 false（err 带原因）；成功则 engineReady_=true 且已有可用会话。
bool Scenario2Model::setup(QString* err) {
    // 0. 拆除既有会话/引擎/桥 + UDP 快照通道（重入 setup 时必须先释放线程/连接）。
    session_.reset();
    childEngine_.reset();
    if (engineReady_) {
        childBridge_.close();
        engineReady_ = false;
    }
    stopSnapshotChannel();
    stagedRows_.clear();
    stagedCells_.clear();

    // 1. 清理 workspace（删除旧库、引擎 sync 目录与 UDP 快照通道目录，确保从干净状态出发）。
    QDir().mkpath(ws_);
    QFile::remove(centerDb_);
    QFile::remove(childDb_);
    for (const QString& sub :
         {QStringLiteral("outbox"), QStringLiteral("inbox"), QStringLiteral("quarantine"),
          QStringLiteral("snap_child/outbox"), QStringLiteral("snap_child/inbox"),
          QStringLiteral("snap_center/outbox"), QStringLiteral("snap_center/inbox")}) {
        QDir(ws_ + QStringLiteral("/") + sub).removeRecursively();
        QDir().mkpath(ws_ + QStringLiteral("/") + sub);
    }

    // 2. seed 两个有意制造差异的库（direct SQL，先于引擎打开）。
    if (!seedDatabases(err))
        return false;

    // 3. 打开子节点B 的 DataBridge。
    ConnectionSpec spec;
    spec.sqlitePath = childDb_;
    spec.enableWal = true;
    if (!childBridge_.open(spec, err))
        return false;

    // 4. 构建子节点B 的 SyncConfig（比对场景 → 人工冲突策略）。
    //    syncTables 从 B 库动态发现（需求 #3：不硬编码，经 dbridge 公共接口 userTables 读取）。
    const QStringList syncTables = childBridge_.userTables(err);
    if (syncTables.isEmpty())
        return false;  // 空库/发现失败：无表可同步（err 已由 userTables 填充）
    cfg_ = SyncConfig::Builder()
               .nodeId(QStringLiteral("child_B"))
               .role(NodeRole::Edge)
               .centerNodeId(QStringLiteral("center_A"))
               .addPeerNode(QStringLiteral("center_A"))
               .database(childDb_)
               .syncTables(syncTables)
               .outboxDir(ws_ + QStringLiteral("/outbox"))
               .inboxDir(ws_ + QStringLiteral("/inbox"))
               .quarantineDir(ws_ + QStringLiteral("/quarantine"))
               .conflictPolicy(ConflictPolicy::Manual)  // 比对：由用户人工决策
               .originPriority(QStringLiteral("center_A"), 100)
               .originPriority(QStringLiteral("child_B"), 50)
               .build(err);
    if (!cfg_->isValid())
        return false;

    // 5. 初始化子节点B 的同步引擎（建立 __sync_* 元数据与 SyncContext，
    //    createComparisonSession 依赖该 SyncContext）。
    childEngine_ = createSyncEngine(childBridge_);
    if (!childEngine_->initialize(*cfg_, err))
        return false;
    engineReady_ = true;

    // 6. 启动 UDP 快照通道（A/B 各一个单-peer 传输 + 中心A 快照响应线程）。
    if (!startSnapshotChannel(err))
        return false;

    // 7. 建立首个比对会话（内部经 UDP 向 A 拉取快照）。
    return rebuildSession(err);
}

// reseed —— 「重置演示数据」：等价于整套 setup 重跑（把 A/B 两库恢复到初始差异态）。
bool Scenario2Model::reseed(QString* err) {
    return setup(err);  // 重置 = 重新 seed 全流程
}

// rebuildSession —— 释放旧比对会话并新建一个：读 A 库快照 → createComparisonSession(B)
//   → initialize（触发 DiffEngine 计算差异）。
// 为什么频繁重建：每次 save/discard 后旧会话已被消费（其 read 事务释放），需要一个反映「最新库
//   状态」的新会话来展示更新后的差异。同时清空内存暂存追踪（stagedRows_/stagedCells_）。
// 返回：会话创建或 initialize 失败则 false 并释放半成品会话。
bool Scenario2Model::rebuildSession(QString* err) {
    session_.reset();  // 释放旧会话（连同其 read 事务与 gate）
    stagedRows_.clear();
    stagedCells_.clear();

    if (!cfg_) {
        if (err)
            *err = QStringLiteral("SyncConfig 未构建");
        return false;
    }

    // 经 UDP 向中心A 请求整库快照（需求 #2：B 不直读 A 的库文件）。
    QList<RemoteTableSnapshot> snaps;
    if (!fetchRemoteSnapshotsOverUdp(&snaps, err))
        return false;

    // 参与比对的表清单/数量来自 A 回传的快照（需求 #3：来自数据库，非硬编码）。
    tables_.clear();
    for (const RemoteTableSnapshot& s : snaps)
        tables_.append(s.table);

    session_ = createComparisonSession(*cfg_, err);
    if (!session_)
        return false;

    if (!session_->initialize(snaps, err)) {
        session_.reset();
        return false;
    }

    // ④ 比对完成：汇总各表红/绿结论（由行级 diff 推导）。
    int diffTables = 0, sameTables = 0;
    for (const TableStatus& st : tableStatuses())
        (st.identical ? sameTables : diffTables) += 1;
    log(QStringLiteral("④ 差异比对完成：%1 张不同 / %2 张相同（共 %3 张表）")
            .arg(diffTables)
            .arg(sameTables)
            .arg(tables_.size()));
    return true;
}

// ── UDP 快照通道：启停 + 请求/等待 ───────────────────────────────────────────

// startSnapshotChannel —— 启动场景2 专属的 UDP 快照通道（与同步引擎目录/端口完全隔离）。
//   三个后台线程：A 侧传输、B 侧传输（各单-peer loopback），以及 A 侧的快照响应线程。
//   responder 在自己线程内 open 一个 DataBridge 读 center_A.db（用 dbridge 公共接口发现表/字段）。
bool Scenario2Model::startSnapshotChannel(QString* err) {
    const QHostAddress loopback(QHostAddress::LocalHost);

    // 确保四个快照目录存在（setup 已建；此处兜底，responder/transport 也会各自 mkpath）。
    for (const QString& d :
         {snapChildOutbox(ws_), snapChildInbox(ws_), snapCenterOutbox(ws_), snapCenterInbox(ws_)})
        QDir().mkpath(d);

    // 中心A 快照响应线程：监视 A 的 inbox 里的 snapreq、读 A 的库、把快照写回 A 的 outbox。
    responder_ = std::make_unique<CenterSnapshotResponder>(centerDb_, snapCenterInbox(ws_),
                                                           snapCenterOutbox(ws_));
    // A 侧传输：单-peer→B。把 A 的 outbox(snapresp) 经 UDP 送到 B、把收到的(snapreq)落 A 的 inbox。
    centerTransport_ = std::make_unique<UdpFileTransport>(
        PORT_CENTER_A, loopback, PORT_CHILD_B, snapCenterOutbox(ws_), snapCenterInbox(ws_));
    // B 侧传输：单-peer→A。把 B 的 outbox(snapreq) 经 UDP 送到 A、把收到的(snapresp)落 B 的 inbox。
    childTransport_ = std::make_unique<UdpFileTransport>(PORT_CHILD_B, loopback, PORT_CENTER_A,
                                                         snapChildOutbox(ws_), snapChildInbox(ws_));

    constexpr int kMaxUdpBytes = 800;
    centerTransport_->setMaxTransmitBytes(kMaxUdpBytes);
    childTransport_->setMaxTransmitBytes(kMaxUdpBytes);
    responder_->start();
    centerTransport_->start();
    childTransport_->start();
    QThread::msleep(120);  // 给三个线程 bind/起转留出时间，避免首个请求丢失（仿场景1）
    (void)err;  // 线程 start 本身不失败；bind 失败仅告警（见 UdpFileTransport::run）
    return true;
}

// stopSnapshotChannel —— 停止并回收快照通道的三个后台线程（幂等）。
//   次序：先停两个传输（不再收发），再停 responder（它用 center 库的连接随之释放）。
void Scenario2Model::stopSnapshotChannel() {
    if (childTransport_) {
        childTransport_->requestStop();
        childTransport_->wait();
        childTransport_.reset();
    }
    if (centerTransport_) {
        centerTransport_->requestStop();
        centerTransport_->wait();
        centerTransport_.reset();
    }
    if (responder_) {
        responder_->requestStop();
        responder_->wait();
        responder_.reset();
    }
}

// fetchRemoteSnapshotsOverUdp —— 经 UDP 向中心A 请求整库快照并等待回传（需求 #2 核心）。
//   步骤：① 生成唯一 reqId，按 outbox 两步协议写 snapreq 工件到 B 的 outbox；
//         ② 轮询 B 的 inbox 等 snapresp__<reqId>（带超时，不静默挂起）；
//         ③ 读取响应体 → 反序列化为 RemoteTableSnapshot 列表。
//   注意：B 全程只碰自己的 snap_child 目录，绝不读 center_A.db —— A 的数据完全经网络到达。
bool Scenario2Model::fetchRemoteSnapshotsOverUdp(QList<RemoteTableSnapshot>* out, QString* err) {
    const QString reqId = QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
    const QString outDir = snapChildOutbox(ws_);
    const QString inDir = snapChildInbox(ws_);

    log(QStringLiteral("① 子节点B → 中心A：发送快照请求（UDP %1→%2，reqId=%3）")
            .arg(PORT_CHILD_B)
            .arg(PORT_CENTER_A)
            .arg(reqId));

    // ① 写请求工件：先 payload（空体即可，A 自行发现要发哪些表），再 .ready 哨兵。
    const QString reqPayload = QStringLiteral("snapreq__%1.payload").arg(reqId);
    const QString reqPath = QDir(outDir).filePath(reqPayload);
    {
        QFile f(reqPath);
        if (!f.open(QIODevice::WriteOnly)) {
            if (err)
                *err = QStringLiteral("无法写快照请求工件：%1").arg(reqPath);
            return false;
        }
        f.close();  // 空体
        QFile ready(reqPath + QStringLiteral(".ready"));
        ready.open(QIODevice::WriteOnly);
        ready.close();
    }

    // ② 等待响应工件到达 B 的 inbox（snapresp__<reqId>.payload + .ready）。
    const QString respName = QStringLiteral("snapresp__%1.payload").arg(reqId);
    const QString respPath = QDir(inDir).filePath(respName);
    const QString respReady = respPath + QStringLiteral(".ready");
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + kSnapshotTimeoutMs;
    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        if (QFile::exists(respReady) && QFile::exists(respPath)) {
            QFile rf(respPath);
            if (!rf.open(QIODevice::ReadOnly)) {
                if (err)
                    *err = QStringLiteral("无法读取快照响应工件：%1").arg(respPath);
                return false;
            }
            const QByteArray body = rf.readAll();
            rf.close();
            // ② 抽干中心A 侧的处理日志（在 ① 与 ③ 之间按序输出，还原真实往返时序）。
            if (responder_)
                for (const QString& line : responder_->takeLog())
                    log(QStringLiteral("   ② [中心A] %1").arg(line));
            *out = s2snap::decodeSnapshots(body);
            if (out->isEmpty()) {
                if (err)
                    *err = QStringLiteral("快照响应解析为空（格式/版本不符或 A 库无用户表）");
                return false;
            }
            log(QStringLiteral("③ 子节点B ← 中心A：收到快照（%1 张表，%2 字节），开始差异比对")
                    .arg(out->size())
                    .arg(body.size()));
            return true;
        }
        QThread::msleep(20);  // 让出 CPU；loopback 正常仅需一两次轮询即命中
    }
    if (err)
        *err = QStringLiteral("等待中心A 快照超时（%1ms）——UDP 通道未回传").arg(kSnapshotTimeoutMs);
    return false;
}

// ── 状态查询 ────────────────────────────────────────────────────────────────

// rowDiffs —— 取某表的全部行级差异（含逐列 CellDiff）。(0, -1) 表示「从第 0 行到末尾」全取。
// 无会话时返回空列表（GUI 会显示为「无差异/未就绪」）。localValue=B 值、remoteValue=A 值。
QList<RowDiff> Scenario2Model::rowDiffs(const QString& table) const {
    if (!session_)
        return {};
    return session_->rowDiffs(table, 0, -1);
}

// tableStatuses —— 逐表汇总差异计数（新增/删除/修改），并据此判定「相同(绿)/不同(红)」。
// 为什么直接由行级 diff 推导而非读 __sync_table_state 的 checksum 快路径：行级 diff 始终准确，
//   不受 checksum 哈希碰撞或元数据滞后影响——演示要的是「肉眼可信」的红绿结论。
// 计数口径：Added=仅A有(B需新增)、Deleted=仅B有(A没有)、Modified=两端都有但字段不同、Same=不计。
// identical = 三类计数全为 0。
QList<Scenario2Model::TableStatus> Scenario2Model::tableStatuses() const {
    QList<TableStatus> out;
    for (const QString& t : tables_) {
        TableStatus st;
        st.table = t;
        const QList<RowDiff> diffs = rowDiffs(t);
        for (const RowDiff& rd : diffs) {
            switch (rd.kind) {
                case RowDiffKind::Added:
                    ++st.added;
                    break;
                case RowDiffKind::Deleted:
                    ++st.deleted;
                    break;
                case RowDiffKind::Modified:
                    ++st.modified;
                    break;
                case RowDiffKind::Same:
                    break;
            }
        }
        st.identical = (st.added == 0 && st.deleted == 0 && st.modified == 0);
        out.append(st);
    }
    return out;
}

// ── 合并决策 ────────────────────────────────────────────────────────────────
// 共性：这四个方法把「采用A / 保留B / 列级采用 / 撤销」的决策同时作用于两处——
//   ① 真正的比对会话 StagingBuffer（session_->accept*/stageCell，决定最终写回什么）；
//   ② 本类的内存追踪 stagedRows_/stagedCells_（仅供 GUI 高亮与 pendingCount 显示用）。
// 两者必须保持一致，否则界面高亮会与实际待写决策脱节。

// acceptRemoteRow —— 整行采用中心A：把该行交给会话 acceptRemote，并把它的所有差异列都标记为已采用。
// 为什么还要遍历差异列逐个标 stagedCells_：界面是「精确到列」高亮的，整行采用等价于「该行每个
//   变化列都采用A」，故把每个 changed 的列也记进单元格集合，使列级高亮与整行决策一致。
// 返回：无会话或 acceptRemote 失败则 false。
bool Scenario2Model::acceptRemoteRow(const QString& table, const QString& pk) {
    if (!session_)
        return false;
    if (!session_->acceptRemote(table, pk))
        return false;
    stagedRows_.insert(rowKey(table, pk));
    // 整行采用 → 标记该行所有差异列均已采用。
    for (const RowDiff& rd : rowDiffs(table)) {
        if (rd.primaryKey == pk) {
            for (const CellDiff& c : rd.cells)
                if (c.changed)
                    stagedCells_.insert(cellKey(table, pk, c.column));
            break;
        }
    }
    return true;
}

// acceptLocalRow —— 保留本地B（即撤销该行的所有「采用A」决策）：会话 acceptLocal +
//   从内存追踪里移除该行键、并清掉该行下所有单元格键。
// 实现细节：cellKey(table,pk,"") 构造出「该行所有单元键的公共前缀」，遍历 stagedCells_ 删掉所有
//   以此前缀打头的项（即把这一行的列级标记一并清除）。
bool Scenario2Model::acceptLocalRow(const QString& table, const QString& pk) {
    if (!session_)
        return false;
    if (!session_->acceptLocal(table, pk))
        return false;
    stagedRows_.remove(rowKey(table, pk));
    const QString prefix = cellKey(table, pk, QString());  // 该行所有单元键的公共前缀
    for (auto it = stagedCells_.begin(); it != stagedCells_.end();) {
        if (it->startsWith(prefix))
            it = stagedCells_.erase(it);  // erase 返回下一个迭代器，避免失效
        else
            ++it;
    }
    return true;
}

// stageCellValue —— 精确到列地采用中心A 的某一列值（可对同一行的多个列累积调用）。
// 同时把该行键与该单元键都记进内存追踪：行键使 pendingCount 把它计为「一行待保存」，
//   单元键驱动该列的高亮。返回：无会话或 stageCell 失败则 false。
bool Scenario2Model::stageCellValue(const QString& table, const QString& pk, const QString& column,
                                    const QVariant& value) {
    if (!session_)
        return false;
    if (!session_->stageCell(table, pk, column, value))
        return false;
    stagedRows_.insert(rowKey(table, pk));
    stagedCells_.insert(cellKey(table, pk, column));
    return true;
}

// unstageRow —— 撤销该行暂存：语义上等同「回到保留本地」，故直接复用 acceptLocalRow。
bool Scenario2Model::unstageRow(const QString& table, const QString& pk) {
    return acceptLocalRow(table, pk);  // 撤销 = 回到"保留本地"
}

// isRowStaged / isCellStaged —— 查询某行 / 某单元是否已被暂存（供界面决定是否高亮）。O(1) 查 QSet。
bool Scenario2Model::isRowStaged(const QString& table, const QString& pk) const {
    return stagedRows_.contains(rowKey(table, pk));
}
bool Scenario2Model::isCellStaged(const QString& table, const QString& pk,
                                  const QString& column) const {
    return stagedCells_.contains(cellKey(table, pk, column));
}

// ── save / discard ──────────────────────────────────────────────────────────

// save —— 把内存暂存的合并决策经 SyncWorker 写回子节点B 数据库（A→B 同步），随后重建会话。
// 为什么保存后要 rebuildSession：session_->save() 一旦提交，旧会话即被消费（read 事务释放），
//   必须重建一个反映「写回后最新状态」的新会话，让界面刷新出新的差异（理想情况大幅减少）。
// 返回：无会话 / save 失败 / 重建失败 → false（err 带原因）。
bool Scenario2Model::save(QString* err) {
    if (!session_) {
        if (err)
            *err = QStringLiteral("比对会话未建立");
        return false;
    }
    if (!session_->save(err))
        return false;
    // 保存后会话已消费（read 事务释放）；重建一个新会话以展示更新后的差异。
    return rebuildSession(err);
}

// discard —— 放弃所有内存暂存（不写库），并重建会话回到「全部保留本地」的初始决策态。
void Scenario2Model::discard() {
    if (session_)
        session_->discard();
    QString err;
    rebuildSession(&err);
}

// ── headless 自检 ────────────────────────────────────────────────────────────

// runHeadlessSelfTest —— 无界面地走通场景2 的核心路径，供 --selftest「编译后运行验证」调用。
// 它与 GUI 完全相同的代码路径，五步串成一个可断言的端到端用例：
//   1) 校验初始差异符合预期（employee/project=不同，department/region=相同）；
//   2) 列级采用：employee#1.salary 采用 A 的 30000；
//   3) 整行采用：employee#5（A 独有 → B 应新增）；
//   4) save 写回 B；
//   5) 重新读 B 库校验：#1.salary 已变 30000 且 #5 已存在。
// 返回：任一步不符预期即 false（err 带具体原因），全部通过返回 true（退出码 0 的依据）。
bool Scenario2Model::runHeadlessSelfTest(QString* err) {
    // 1) 校验初始差异：employee/project 为"不同"，department/region 为"相同"。
    const auto statuses = tableStatuses();
    auto statusOf = [&](const QString& t) -> TableStatus {
        for (const auto& s : statuses)
            if (s.table == t)
                return s;
        return {};
    };
    if (statusOf(QStringLiteral("employee")).identical ||
        statusOf(QStringLiteral("project")).identical) {
        if (err)
            *err = QStringLiteral("预期 employee/project 应为不同，但判定为相同");
        return false;
    }
    if (!statusOf(QStringLiteral("department")).identical ||
        !statusOf(QStringLiteral("region")).identical) {
        if (err)
            *err = QStringLiteral("预期 department/region 应为相同，但判定为不同");
        return false;
    }

    // 2) 列级采用：employee#1 的 salary 采用中心A（30000）。
    if (!stageCellValue(QStringLiteral("employee"), QStringLiteral("1"), QStringLiteral("salary"),
                        QVariant(30000))) {
        if (err)
            *err = QStringLiteral("stageCell(employee#1.salary) 失败");
        return false;
    }
    // 3) 整行采用：employee#5 孙七（A 独有 → B 新增）。
    if (!acceptRemoteRow(QStringLiteral("employee"), QStringLiteral("5"))) {
        if (err)
            *err = QStringLiteral("acceptRemote(employee#5) 失败");
        return false;
    }

    // 4) 保存（A→B 写回）。
    if (!save(err))
        return false;

    // 5) 校验 B 库已更新：employee#1.salary==30000 且 employee#5 存在。
    const auto rows = readRows(childDb_, QStringLiteral("employee"));
    bool ok1 = false, ok5 = false;
    for (const auto& r : rows) {
        if (r.value(QStringLiteral("id")).toInt() == 1 &&
            r.value(QStringLiteral("salary")).toInt() == 30000)
            ok1 = true;
        if (r.value(QStringLiteral("id")).toInt() == 5)
            ok5 = true;
    }
    if (!ok1 || !ok5) {
        if (err)
            *err = QStringLiteral("写回校验失败：employee#1.salary=30000? %1，employee#5 存在? %2")
                       .arg(ok1)
                       .arg(ok5);
        return false;
    }
    return true;
}
