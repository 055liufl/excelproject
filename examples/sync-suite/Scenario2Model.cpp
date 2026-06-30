#include "Scenario2Model.h"

#include "dbridge/Types.h"

#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>

using namespace dbridge;
using namespace dbridge::sync;

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

// 构造：只记录 workspace 路径、推导出 A/B 两库文件路径、固定参与比对的 4 张表名；
// 不打开任何库、不建会话（重活留给 setup()，构造保持零成本不可失败）。
Scenario2Model::Scenario2Model(const QString& ws)
    : ws_(ws),
      centerDb_(ws + QStringLiteral("/center_A.db")),
      childDb_(ws + QStringLiteral("/child_B.db")),
      tables_{QStringLiteral("employee"), QStringLiteral("department"), QStringLiteral("project"),
              QStringLiteral("region")} {
}

Scenario2Model::~Scenario2Model() {
    // 先析构会话与引擎，再关闭 bridge，确保 SyncContext / 连接被正确释放。
    session_.reset();
    childEngine_.reset();
    if (engineReady_)
        childBridge_.close();
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

// readColumns —— 读任意库某表的列顺序（用 PRAGMA table_info，按建表时的列序返回）。
// 用途：双栏视图按这个列序逐列展示；A、B 两库 schema 同构，故取任一即可（这里用 B）。
QStringList Scenario2Model::readColumns(const QString& dbPath, const QString& table) {
    QStringList cols;
    const QString conn =
        QStringLiteral("s2_cols_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        if (db.open()) {
            QSqlQuery q(db);
            if (q.exec(QStringLiteral("PRAGMA table_info(\"%1\")").arg(QString(table)))) {
                while (q.next())
                    cols.append(q.value(QStringLiteral("name")).toString());
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(conn);
    return cols;
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

// 下面四个是给 GUI 用的便捷包装：固定到 B 库（列序/主键，A 与之同构）或分别指向 A/B 库读行。
QStringList Scenario2Model::columns(const QString& table) const {
    return readColumns(childDb_, table);
}
QString Scenario2Model::pkColumn(const QString& table) const {
    return readPkColumn(childDb_, table);
}
QList<QVariantMap> Scenario2Model::centerRows(const QString& table) const {
    return readRows(centerDb_, table);  // 中心A（远端，右栏）
}
QList<QVariantMap> Scenario2Model::childRows(const QString& table) const {
    return readRows(childDb_, table);  // 子节点B（本地，左栏）
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

// ── 远端快照构造（从 center_A 读全库）────────────────────────────────────────

// computeChecksum —— 为一张表的全部行算一个粗粒度内容指纹（顺序无关的累加 hash）。
// 用途：填充 RemoteTableSnapshot.meta.contentChecksum，供比对会话的 checksum 快路径参考。
// 为什么顺序无关（对每行 h 求和而非串接）：行序不应影响「整表内容是否相同」的判断。
// 说明：仅演示用途的简单实现，非密码学 hash；真实差异仍以行级 diff 为准（见 tableStatuses 注释）。
static QString computeChecksum(const QList<QVariantMap>& rows) {
    quint64 sum = 0;
    for (const auto& row : rows) {
        quint64 h = 0;
        for (auto it = row.begin(); it != row.end(); ++it)
            h = h * 31 + qHash(it.key()) + qHash(it.value().toString());
        sum += h;
    }
    return QString::number(sum, 16);
}

// buildRemoteSnapshots —— 从中心A 库读出全部参与表的快照，组装成 RemoteTableSnapshot 列表。
// 做什么：逐表 readRows(centerDb_) 取全行，填好 schema 指纹/内容 checksum/行数等 meta。
// 用途：作为 IComparisonSession::initialize 的入参——比对会话把「远端A 快照」与「本地B 库」逐表
//   做 DiffEngine 比对。参数 err 当前未用（读 A 失败时该表退化为空快照，不致命）。
QList<RemoteTableSnapshot> Scenario2Model::buildRemoteSnapshots(QString* /*err*/) const {
    QList<RemoteTableSnapshot> snaps;
    for (const QString& t : tables_) {
        RemoteTableSnapshot snap;
        snap.table = t;
        snap.rows = readRows(centerDb_, t);
        snap.meta.schemaFingerprint = QStringLiteral("fp_") + t;
        snap.meta.contentChecksum = computeChecksum(snap.rows);
        snap.meta.rowCount = snap.rows.size();
        snaps.append(snap);
    }
    return snaps;
}

// ── setup / reseed / rebuildSession ─────────────────────────────────────────

// setup —— 从零把场景2 带到「可比对」状态（可重入，重入时先彻底拆除旧资源）。
// 六步：拆旧 → 清 workspace → seed 两库 → 开 B 的 DataBridge → 建 B 的 SyncConfig
//        → 初始化 B 的 ISyncEngine（建 SyncContext）→ rebuildSession 建首个比对会话。
// 为什么引擎必须先初始化：createComparisonSession 依赖引擎建立的 SyncContext 与 __sync_* 元数据。
// 返回：任一步失败即 false（err 带原因）；成功则 engineReady_=true 且已有可用会话。
bool Scenario2Model::setup(QString* err) {
    // 0. 拆除既有会话/引擎/桥（重入 setup 时必须先释放 SyncContext 与连接）。
    session_.reset();
    childEngine_.reset();
    if (engineReady_) {
        childBridge_.close();
        engineReady_ = false;
    }
    stagedRows_.clear();
    stagedCells_.clear();

    // 1. 清理 workspace（删除旧库与 sync 目录，确保从干净状态出发）。
    QDir().mkpath(ws_);
    QFile::remove(centerDb_);
    QFile::remove(childDb_);
    for (const QString& sub :
         {QStringLiteral("outbox"), QStringLiteral("inbox"), QStringLiteral("quarantine")}) {
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
    cfg_ = SyncConfig::Builder()
               .nodeId(QStringLiteral("child_B"))
               .role(NodeRole::Edge)
               .centerNodeId(QStringLiteral("center_A"))
               .addPeerNode(QStringLiteral("center_A"))
               .database(childDb_)
               .syncTables(tables_)
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

    // 6. 建立首个比对会话。
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
    session_ = createComparisonSession(*cfg_, err);
    if (!session_)
        return false;

    QString e2;
    const QList<RemoteTableSnapshot> snaps = buildRemoteSnapshots(&e2);
    if (!session_->initialize(snaps, err)) {
        session_.reset();
        return false;
    }
    return true;
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
