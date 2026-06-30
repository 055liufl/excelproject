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
static const QChar kSep1 = QChar(0x01);
static const QChar kSep2 = QChar(0x02);

QString Scenario2Model::rowKey(const QString& table, const QString& pk) {
    return table + kSep1 + pk;
}
QString Scenario2Model::cellKey(const QString& table, const QString& pk, const QString& column) {
    return table + kSep1 + pk + kSep2 + column;
}

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

QStringList Scenario2Model::columns(const QString& table) const {
    return readColumns(childDb_, table);
}
QString Scenario2Model::pkColumn(const QString& table) const {
    return readPkColumn(childDb_, table);
}
QList<QVariantMap> Scenario2Model::centerRows(const QString& table) const {
    return readRows(centerDb_, table);
}
QList<QVariantMap> Scenario2Model::childRows(const QString& table) const {
    return readRows(childDb_, table);
}

// ── seed：建表 + 灌入"有意制造差异"的数据 ──────────────────────────────────────
//
// center_A（远端）与 child_B（本地）的数据故意构造为：
//   employee  —— 不同(红)：id=1 薪资改、id=3 部门+薪资+职级改、id=5 仅A有、id=4 仅B有
//   department—— 相同(绿)
//   project   —— 不同(红)：id=1、id=3 状态改
//   region    —— 相同(绿)
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

// 行内容指纹（顺序无关的简单累加 hash；与 diff-demo 同思路，仅用于 meta 填充）。
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

bool Scenario2Model::reseed(QString* err) {
    return setup(err);  // 重置 = 重新 seed 全流程
}

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

QList<RowDiff> Scenario2Model::rowDiffs(const QString& table) const {
    if (!session_)
        return {};
    return session_->rowDiffs(table, 0, -1);
}

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

bool Scenario2Model::acceptLocalRow(const QString& table, const QString& pk) {
    if (!session_)
        return false;
    if (!session_->acceptLocal(table, pk))
        return false;
    stagedRows_.remove(rowKey(table, pk));
    const QString prefix = cellKey(table, pk, QString());
    for (auto it = stagedCells_.begin(); it != stagedCells_.end();) {
        if (it->startsWith(prefix))
            it = stagedCells_.erase(it);
        else
            ++it;
    }
    return true;
}

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

bool Scenario2Model::unstageRow(const QString& table, const QString& pk) {
    return acceptLocalRow(table, pk);  // 撤销 = 回到"保留本地"
}

bool Scenario2Model::isRowStaged(const QString& table, const QString& pk) const {
    return stagedRows_.contains(rowKey(table, pk));
}
bool Scenario2Model::isCellStaged(const QString& table, const QString& pk,
                                  const QString& column) const {
    return stagedCells_.contains(cellKey(table, pk, column));
}

// ── save / discard ──────────────────────────────────────────────────────────

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

void Scenario2Model::discard() {
    if (session_)
        session_->discard();
    QString err;
    rebuildSession(&err);
}

// ── headless 自检 ────────────────────────────────────────────────────────────

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
