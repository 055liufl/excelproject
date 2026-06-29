/**
 * diff-demo — dbridge Beyond Compare 范式深度可视化差异比对演示
 *
 * 本 demo 完整展示 IComparisonSession 的用法，模拟 Beyond Compare 风格的
 * 数据库差异比对与合并工作流：
 *
 *  工作流阶段：
 *   ┌─ 1. 构建 RemoteTableSnapshot ──────────────────────────────────────┐
 *   │   从"远端"获取表级元数据（schema 指纹 + content 校验和 + row 数量） │
 *   │   以及行级快照数据（QList<QVariantMap>）                            │
 *   └──────────────────────────────────────────────────────────────────┘
 *   ┌─ 2. IComparisonSession::initialize() ──────────────────────────────┐
 *   │   DiffEngine 做两级比对：                                           │
 *   │     ① 表级快速判等（checksum/fingerprint/rowCount 三元组）          │
 *   │     ② 行级 diff（Added / Deleted / Modified / Same）                │
 *   └──────────────────────────────────────────────────────────────────┘
 *   ┌─ 3. 可视化展示 diff 结果 ───────────────────────────────────────────┐
 *   │   tableDiffs() → 表级摘要                                           │
 *   │   rowDiffs()   → 行级明细（含逐列 CellDiff）                        │
 *   └──────────────────────────────────────────────────────────────────┘
 *   ┌─ 4. 交互式合并决策（Beyond Compare 核心） ──────────────────────────┐
 *   │   stageRow()      → 将远端版本暂存入 StagingBuffer（内存）          │
 *   │   acceptLocal()   → 保留本地版本（不写入）                          │
 *   │   acceptRemote()  → 采用远端版本（同 stageRow）                     │
 *   │   stageCell()     → 逐列精细合并（从远端取某列的值）                 │
 *   │   unstage()       → 撤销某行的暂存决策                              │
 *   └──────────────────────────────────────────────────────────────────┘
 *   ┌─ 5. 提交或丢弃 ────────────────────────────────────────────────────┐
 *   │   save()    → 将 StagingBuffer 经 UpsertExecutor 批量写库           │
 *   │   discard() → 丢弃所有暂存，释放内存                                │
 *   └──────────────────────────────────────────────────────────────────┘
 *
 * ──────────────────────────────────────────────────────────────────────────────
 * 运行方式（在项目构建目录下）：
 *   ./examples/diff-demo/diff-demo <workspace-dir>
 *
 * workspace-dir 下会自动创建 local.db。
 * ──────────────────────────────────────────────────────────────────────────────
 */

#include "dbridge/DataBridge.h"
#include "dbridge/sync/IComparisonSession.h"
#include "dbridge/sync/ISyncEngine.h"
#include "dbridge/sync/SyncConfig.h"
#include "dbridge/sync/SyncTypes.h"

#include <QCoreApplication>
#include <QDir>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QString>
#include <QVariantMap>

#include <iomanip>
#include <iostream>
#include <string>

using namespace dbridge::sync;

// ══════════════════════════════════════════════════════════════════════════════
// 终端渲染辅助：模拟 Beyond Compare 双栏对比界面
// ══════════════════════════════════════════════════════════════════════════════

// ANSI 颜色码（终端支持时显示）
namespace Color {
const char* Reset = "\033[0m";
const char* Red = "\033[31m";
const char* Green = "\033[32m";
const char* Yellow = "\033[33m";
const char* Cyan = "\033[36m";
const char* Bold = "\033[1m";
const char* Grey = "\033[90m";
}  // namespace Color

// 把 RowDiffKind 转为符号（类 BeyondCompare 风格）
static const char* kindSymbol(RowDiffKind k) {
    switch (k) {
        case RowDiffKind::Added:
            return "  + ";  // 远端有、本地没有
        case RowDiffKind::Deleted:
            return "  - ";  // 本地有、远端没有
        case RowDiffKind::Modified:
            return "  ~ ";  // 两端都有但不同
        case RowDiffKind::Same:
            return "    ";
    }
    return "  ? ";
}

// 把 RowDiffKind 转为 ANSI 颜色
static const char* kindColor(RowDiffKind k) {
    switch (k) {
        case RowDiffKind::Added:
            return Color::Green;
        case RowDiffKind::Deleted:
            return Color::Red;
        case RowDiffKind::Modified:
            return Color::Yellow;
        case RowDiffKind::Same:
            return Color::Reset;
    }
    return Color::Reset;
}

// 打印分隔线
static void printSep(int width = 72) {
    std::cout << std::string(width, '-') << "\n";
}

// 打印表级摘要（仿 BeyondCompare 的 Folder Compare 视图）
static void printTableSummary(const QList<TableDiff>& diffs) {
    std::cout << Color::Bold << std::left << std::setw(20) << "表名" << std::setw(14) << "状态"
              << std::setw(8) << "+行" << std::setw(8) << "-行" << std::setw(8) << "~行"
              << Color::Reset << "\n";
    printSep();

    for (const auto& td : diffs) {
        const char* statusStr = "Identical";
        const char* statusColor = Color::Grey;
        switch (td.status) {
            case TableDiffStatus::Identical:
                statusStr = "Identical";
                statusColor = Color::Grey;
                break;
            case TableDiffStatus::Different:
                statusStr = "Different";
                statusColor = Color::Yellow;
                break;
            case TableDiffStatus::OnlyLocal:
                statusStr = "OnlyLocal";
                statusColor = Color::Red;
                break;
            case TableDiffStatus::OnlyRemote:
                statusStr = "OnlyRemote";
                statusColor = Color::Green;
                break;
        }
        std::cout << statusColor << std::left << std::setw(20) << td.table.toStdString()
                  << std::setw(14) << statusStr << std::setw(8) << td.addedRows << std::setw(8)
                  << td.deletedRows << std::setw(8) << td.modifiedRows << Color::Reset << "\n";
    }
    printSep();
}

// 打印行级 diff（仿 BeyondCompare 的 Table Compare 双栏视图）
static void printRowDiffs(const QString& table, const QList<RowDiff>& rows) {
    if (rows.isEmpty()) {
        std::cout << Color::Grey << "  （无差异行）" << Color::Reset << "\n";
        return;
    }

    // 收集所有出现过的列名（保持顺序）
    QStringList cols;
    for (const auto& row : rows) {
        for (const auto& cell : row.cells) {
            if (!cols.contains(cell.column))
                cols.append(cell.column);
        }
    }

    // 表头
    std::cout << Color::Bold << Color::Cyan << "  " << table.toStdString() << " 行级差异：\n"
              << Color::Reset;

    const int colW = 16;
    std::cout << Color::Bold << std::left << std::setw(4) << ""  // 状态符
              << std::setw(12) << "PrimaryKey";
    for (const auto& c : cols)
        std::cout << std::setw(colW) << c.left(colW - 1).toStdString();
    std::cout << Color::Reset << "\n";
    printSep();

    for (const auto& row : rows) {
        if (row.kind == RowDiffKind::Same)
            continue;  // 跳过相同行（减少噪音）

        std::cout << kindColor(row.kind) << kindSymbol(row.kind) << std::setw(12)
                  << row.primaryKey.toStdString();

        for (const auto& col : cols) {
            // 找到该列的 CellDiff
            const CellDiff* found = nullptr;
            for (const auto& cell : row.cells) {
                if (cell.column == col) {
                    found = &cell;
                    break;
                }
            }
            if (!found) {
                std::cout << std::setw(colW) << "";
                continue;
            }

            if (found->changed) {
                // 用 "本地→远端" 格式突出变化
                std::string local = found->localValue.toString().left(6).toStdString();
                std::string remote = found->remoteValue.toString().left(6).toStdString();
                std::string cell = local + "→" + remote;
                std::cout << std::setw(colW) << cell;
            } else {
                std::string val = found->localValue.toString().left(colW - 1).toStdString();
                std::cout << std::setw(colW) << val;
            }
        }
        std::cout << Color::Reset << "\n";
    }
    printSep();
}

// ══════════════════════════════════════════════════════════════════════════════
// 数据库辅助
// ══════════════════════════════════════════════════════════════════════════════

static bool execSqls(const QString& dbPath, const QStringList& sqls, const std::string& tag = "") {
    const QString connName = QStringLiteral("exec_") + dbPath + tag.c_str();
    bool ok = true;
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(dbPath);
        if (!db.open()) {
            ok = false;
        } else {
            QSqlQuery q(db);
            for (const auto& sql : sqls) {
                if (!q.exec(sql)) {
                    std::cerr << "SQL failed [" << tag << "]: " << sql.left(80).toStdString()
                              << "\n"
                              << "  " << q.lastError().text().toStdString() << "\n";
                    ok = false;
                    break;
                }
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connName);
    return ok;
}

// 把一张表的全部行读成 QList<QVariantMap>（模拟"从远端获取快照"）
static QList<QVariantMap> fetchAllRows(const QString& dbPath, const QString& table) {
    QList<QVariantMap> result;
    const QString connName = QStringLiteral("fetch_") + dbPath + table;
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(dbPath);
        db.open();
        QSqlQuery q(db);
        q.exec(QStringLiteral("SELECT * FROM ") + table);
        while (q.next()) {
            QVariantMap row;
            for (int i = 0; i < q.record().count(); ++i)
                row[q.record().fieldName(i)] = q.value(i);
            result.append(row);
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);
    return result;
}

// 计算简单的行内容指纹（demo 用：行哈希之和的十六进制）
// 真实实现在 TableStateStore：顺序无关模加 hash（see §15.7.9）
static QString computeChecksum(const QList<QVariantMap>& rows) {
    quint64 sum = 0;
    for (const auto& row : rows) {
        quint64 h = 0;
        for (auto it = row.begin(); it != row.end(); ++it)
            h = h * 31 + qHash(it.value().toString());
        sum += h;
    }
    return QString::number(sum, 16);
}

// 打印表内容（辅助确认合并结果）
static void dumpTable(const QString& dbPath, const QString& table, const std::string& label) {
    auto rows = fetchAllRows(dbPath, table);
    std::cout << "\n  [" << label << " / " << table.toStdString() << "]\n";
    for (const auto& row : rows) {
        std::cout << "    |";
        for (auto it = row.begin(); it != row.end(); ++it)
            std::cout << " " << it.key().toStdString() << "=" << it.value().toString().toStdString()
                      << " |";
        std::cout << "\n";
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// main
// ══════════════════════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    if (argc < 2) {
        std::cerr << "Usage: diff-demo <workspace-dir>\n";
        return 1;
    }
    const QString ws = QString::fromLocal8Bit(argv[1]);
    const QString localDb = ws + "/local.db";
    QDir().mkpath(ws + "/outbox");
    QDir().mkpath(ws + "/inbox");
    QDir().mkpath(ws + "/quarantine");

    std::cout << Color::Bold << "=================================================\n"
              << " diff-demo: dbridge Beyond Compare 差异比对范式\n"
              << "=================================================\n"
              << Color::Reset << "  workspace: " << ws.toStdString() << "\n"
              << "  local.db : " << localDb.toStdString() << "\n\n";

    // ── 1. 建库建表 ──────────────────────────────────────────────────────────
    execSqls(localDb,
             {
                 QStringLiteral("CREATE TABLE IF NOT EXISTS products ("
                                "  id    INTEGER PRIMARY KEY,"
                                "  name  TEXT    NOT NULL,"
                                "  price INTEGER NOT NULL,"
                                "  stock INTEGER NOT NULL DEFAULT 0"
                                ")"),
                 QStringLiteral("CREATE TABLE IF NOT EXISTS orders ("
                                "  id         INTEGER PRIMARY KEY,"
                                "  product_id INTEGER NOT NULL REFERENCES products(id),"
                                "  qty        INTEGER NOT NULL,"
                                "  status     TEXT    NOT NULL"
                                ")"),
             },
             "schema");

    // 本地库初始数据
    execSqls(localDb,
             {
                 QStringLiteral("INSERT OR IGNORE INTO products VALUES (1,'苹果',5,100)"),
                 QStringLiteral("INSERT OR IGNORE INTO products VALUES (2,'香蕉',3,200)"),
                 QStringLiteral("INSERT OR IGNORE INTO products VALUES (3,'橙子',4,150)"),
                 QStringLiteral("INSERT OR IGNORE INTO products VALUES (4,'葡萄',8,80)"),
                 QStringLiteral("INSERT OR IGNORE INTO orders VALUES (1,1,10,'shipped')"),
                 QStringLiteral("INSERT OR IGNORE INTO orders VALUES (2,2,5,'pending')"),
                 QStringLiteral("INSERT OR IGNORE INTO orders VALUES (3,3,20,'shipped')"),
             },
             "local_seed");

    std::cout << "本地库初始状态：\n";
    dumpTable(localDb, QStringLiteral("products"), "local/products");
    dumpTable(localDb, QStringLiteral("orders"), "local/orders");

    // ── 2. 构建"远端快照"（模拟从远端节点获取数据）─────────────────────────
    //
    //  真实场景：远端通过 TableStateStore 导出 {schemaFingerprint, contentChecksum,
    //  rowCount}，宿主拉取后填入 RemoteTableSnapshot::meta；
    //  远端行数据通过 IComparisonSession::fetchRemoteRows() 按需懒加载或
    //  在 initialize() 时一次性提供。
    //
    //  本 demo 直接在内存中构造远端快照，模拟一个与本地有差异的远端版本：
    //   - products：苹果价格改为 6（涨价），新增 id=5 西瓜，删除 id=4 葡萄
    //   - orders  ：id=2 状态改为 shipped，新增 id=4

    std::cout << "\n--- 构建远端快照（模拟远端数据库） ---\n";

    // 远端 products 数据
    QList<QVariantMap> remoteProducts = {
        {{"id", QVariant(1)},
         {"name", QStringLiteral("苹果")},
         {"price", QVariant(6)},
         {"stock", QVariant(100)}},  // price: 5→6
        {{"id", QVariant(2)},
         {"name", QStringLiteral("香蕉")},
         {"price", QVariant(3)},
         {"stock", QVariant(200)}},  // 不变
        {{"id", QVariant(3)},
         {"name", QStringLiteral("橙子")},
         {"price", QVariant(4)},
         {"stock", QVariant(120)}},  // stock: 150→120
        // id=4 葡萄在远端被删除
        {{"id", QVariant(5)},
         {"name", QStringLiteral("西瓜")},
         {"price", QVariant(12)},
         {"stock", QVariant(60)}},  // 新增
    };

    // 远端 orders 数据
    QList<QVariantMap> remoteOrders = {
        {{"id", QVariant(1)},
         {"product_id", QVariant(1)},
         {"qty", QVariant(10)},
         {"status", QStringLiteral("shipped")}},
        {{"id", QVariant(2)},
         {"product_id", QVariant(2)},
         {"qty", QVariant(5)},
         {"status", QStringLiteral("shipped")}},  // status: pending→shipped
        {{"id", QVariant(3)},
         {"product_id", QVariant(3)},
         {"qty", QVariant(20)},
         {"status", QStringLiteral("shipped")}},
        {{"id", QVariant(4)},
         {"product_id", QVariant(5)},
         {"qty", QVariant(3)},
         {"status", QStringLiteral("pending")}},  // 新增（引用西瓜）
    };

    //  构建 RemoteTableSnapshot
    //  meta.schemaFingerprint : 真实实现为列名/类型/PK → SHA-256；此处演示用固定串
    //  meta.contentChecksum   : 真实实现为 TableStateStore 的顺序无关模加 hash

    RemoteTableSnapshot snapProducts;
    snapProducts.table = QStringLiteral("products");
    snapProducts.meta.schemaFingerprint = QStringLiteral("fp_products_v1");
    snapProducts.meta.contentChecksum = computeChecksum(remoteProducts);
    snapProducts.meta.rowCount = remoteProducts.size();
    snapProducts.rows = remoteProducts;  // 直接提供行数据

    RemoteTableSnapshot snapOrders;
    snapOrders.table = QStringLiteral("orders");
    snapOrders.meta.schemaFingerprint = QStringLiteral("fp_orders_v1");
    snapOrders.meta.contentChecksum = computeChecksum(remoteOrders);
    snapOrders.meta.rowCount = remoteOrders.size();
    snapOrders.rows = remoteOrders;

    std::cout << "  远端 products: " << remoteProducts.size() << " 行\n"
              << "  远端 orders  : " << remoteOrders.size() << " 行\n";

    // ── 3. 打开 DataBridge 并创建 ComparisonSession ──────────────────────────
    dbridge::DataBridge bridge;
    dbridge::ConnectionSpec cs;
    cs.sqlitePath = localDb;
    cs.enableWal = true;
    QString err;

    if (!bridge.open(cs, &err)) {
        std::cerr << "bridge open failed: " << err.toStdString() << "\n";
        return 1;
    }

    // 先要初始化同步引擎，ComparisonSession 依赖 SyncContext（含 TableStateStore）
    //
    //  createComparisonSession 需要 SyncConfig 来：
    //    ① 获取 syncTables（确认哪些表参与比对）
    //    ② 访问 TableStateStore（读取本地表态 checksum/fingerprint）
    //    ③ 创建 InboundTableGate（比对期间门控 inbox 应用）

    SyncConfig cfg = SyncConfig::Builder()
                         .nodeId(QStringLiteral("local"))
                         .role(NodeRole::Edge)
                         .centerNodeId(QStringLiteral("remote"))
                         .addPeerNode(QStringLiteral("remote"))
                         .database(localDb)
                         .outboxDir(ws + "/outbox")
                         .inboxDir(ws + "/inbox")
                         .quarantineDir(ws + "/quarantine")
                         .conflictPolicy(ConflictPolicy::Manual)  // 比对场景：人工决策
                         .originPriority(QStringLiteral("remote"), 100)
                         .originPriority(QStringLiteral("local"), 50)
                         .build(&err);

    if (!cfg.isValid()) {
        std::cerr << "SyncConfig build failed: " << err.toStdString() << "\n";
        return 1;
    }

    // 初始化同步引擎（建立 __sync_* 元数据表，挂 session，启动 SyncWorker）
    auto engine = createSyncEngine(bridge);
    if (!engine->initialize(cfg, &err)) {
        std::cerr << "initialize failed: " << err.toStdString() << "\n";
        return 1;
    }
    std::cout << "\n同步引擎初始化完成，SyncContext 已建立\n";

    // 创建比对会话
    //   createComparisonSession 内部会：
    //     ① 读取本地 __sync_table_state（快速判等的 checksum/fingerprint）
    //     ② 注册到 InboundTableGate（比对期间暂停对被比对表的 inbox 应用）
    auto session = createComparisonSession(cfg, &err);
    if (!session) {
        std::cerr << "createComparisonSession failed: " << err.toStdString() << "\n";
        return 1;
    }
    std::cout << "ComparisonSession 已创建\n";

    // ── 4. initialize：传入远端快照，触发 DiffEngine 计算 ─────────────────────
    //
    //  DiffEngine 两级比对流程：
    //   Level 1（表级）：比对 {schemaFingerprint, contentChecksum, rowCount}
    //     - 三元组全等 → TableDiffStatus::Identical，跳过行级 diff
    //     - 不等         → 进入 Level 2
    //   Level 2（行级）：以主键为 key 对比两端行集合
    //     - 只在本地 → Deleted（本地有，远端没有）
    //     - 只在远端 → Added  （远端有，本地没有）
    //     - 两端都有但内容不同 → Modified（逐列填充 CellDiff）

    std::cout << "\n--- 调用 initialize()，触发双级 diff ---\n";
    QList<RemoteTableSnapshot> snapshots = {snapProducts, snapOrders};
    if (!session->initialize(snapshots, &err)) {
        std::cerr << "session initialize failed: " << err.toStdString() << "\n";
        return 1;
    }
    std::cout << "DiffEngine 计算完成\n";

    // ── 5. 可视化展示：表级摘要 ──────────────────────────────────────────────
    std::cout << "\n"
              << Color::Bold << "════════════════════════════════════════════\n"
              << "  表级差异摘要（Folder Compare）\n"
              << "════════════════════════════════════════════\n"
              << Color::Reset;

    auto tableDiffs = session->tableDiffs();
    printTableSummary(tableDiffs);

    // ── 6. 可视化展示：行级 diff ──────────────────────────────────────────────
    std::cout << "\n"
              << Color::Bold << "════════════════════════════════════════════\n"
              << "  行级差异明细（Table Compare）\n"
              << "════════════════════════════════════════════\n"
              << Color::Reset << "  图例：+ 仅远端存在  - 仅本地存在  ~ 内容不同\n"
              << "        列值格式：本地→远端（仅变更列显示箭头）\n\n";

    for (const auto& td : tableDiffs) {
        if (td.status == TableDiffStatus::Identical) {
            std::cout << Color::Grey << "  " << td.table.toStdString() << "：完全相同，跳过\n"
                      << Color::Reset;
            continue;
        }
        // rowDiffs(table, offset, limit)：支持分页，-1 表示全部
        auto rowDiffs = session->rowDiffs(td.table, 0, -1);
        printRowDiffs(td.table, rowDiffs);
    }

    // ── 7. 交互式合并决策 ────────────────────────────────────────────────────
    //
    //  本 demo 硬编码以下合并策略（真实 GUI 由用户点击决定）：
    //   products:
    //     id=1（苹果价格 5→6）  → stageCell：只取远端的 price，保留本地 stock
    //     id=3（橙子 stock 变）  → acceptLocal：保留本地版本（不采用远端的 stock）
    //     id=4（葡萄只本地有）   → acceptLocal：保留本地，不从远端删除
    //     id=5（西瓜只远端有）   → acceptRemote：全部接受远端新行
    //   orders:
    //     id=2（status 变）      → acceptRemote：接受远端版本（shipped）
    //     id=4（只远端有）       → acceptRemote：接受远端新行（需先接受西瓜）

    std::cout << "\n"
              << Color::Bold << "════════════════════════════════════════════\n"
              << "  合并决策（模拟用户在 Beyond Compare 中点击）\n"
              << "════════════════════════════════════════════\n"
              << Color::Reset;

    // ──────────────────────────────────────────────────────────────────────────
    // stageCell：精细合并——只取远端 price，其余列保留本地
    //   stageCell(table, primaryKey, column, value)
    //   → 在 StagingBuffer 中为该行创建一个 RowMutation：
    //     仅 price 列用远端值，其余列保持本地现有值
    // ──────────────────────────────────────────────────────────────────────────
    std::cout << "  [products#1] stageCell: 只采用远端 price=6（本地 stock=100 保留）\n";
    session->stageCell(QStringLiteral("products"), QStringLiteral("1"), QStringLiteral("price"),
                       QVariant(6));

    // ──────────────────────────────────────────────────────────────────────────
    // acceptLocal：保留本地版本
    //   内部实现：从 StagingBuffer 中移除该行的 pending mutation（若有），
    //   相当于"不做任何变更"。
    // ──────────────────────────────────────────────────────────────────────────
    std::cout << "  [products#3] acceptLocal: 保留本地 stock=150（忽略远端 stock=120）\n";
    session->acceptLocal(QStringLiteral("products"), QStringLiteral("3"));

    // ──────────────────────────────────────────────────────────────────────────
    // acceptLocal：保留本地独有行（远端已删除 id=4，但我们选择不删）
    // ──────────────────────────────────────────────────────────────────────────
    std::cout << "  [products#4] acceptLocal: 保留本地独有行 葡萄（不做删除）\n";
    session->acceptLocal(QStringLiteral("products"), QStringLiteral("4"));

    // ──────────────────────────────────────────────────────────────────────────
    // acceptRemote：全部接受远端版本
    //   等价于 stageRow(table, pk)，把远端整行放入 StagingBuffer
    // ──────────────────────────────────────────────────────────────────────────
    std::cout << "  [products#5] acceptRemote: 接受远端新行 西瓜\n";
    session->acceptRemote(QStringLiteral("products"), QStringLiteral("5"));

    std::cout << "  [orders#2]   acceptRemote: status: pending→shipped\n";
    session->acceptRemote(QStringLiteral("orders"), QStringLiteral("2"));

    std::cout << "  [orders#4]   acceptRemote: 接受远端新行（引用 product_id=5）\n";
    session->acceptRemote(QStringLiteral("orders"), QStringLiteral("4"));

    // ──────────────────────────────────────────────────────────────────────────
    // stageRow / unstage 演示：先 stage 再 unstage（撤销）
    // ──────────────────────────────────────────────────────────────────────────
    std::cout << "  [products#1] stageRow（演示 stageRow API）→ 再 unstage 撤销\n";
    session->stageRow(QStringLiteral("products"), QStringLiteral("1"));
    // 撤销刚才的 stageRow（恢复为之前 stageCell 的精细合并状态）
    session->unstage(QStringLiteral("products"), QStringLiteral("1"));
    // 重新应用精细合并
    session->stageCell(QStringLiteral("products"), QStringLiteral("1"), QStringLiteral("price"),
                       QVariant(6));

    // ── 8. 提交合并结果 ──────────────────────────────────────────────────────
    //
    //  save() 内部流程：
    //    ① StagingBuffer → UpsertExecutor → 批量 UPSERT 到本地 .db
    //    ② 同步引擎处于活动状态时，写操作经 CapturedWriteTemplate（分支 B）
    //       同时捕获到 __sync_changelog，保证合并结果能向上游传播
    //    ③ InboundTableGate 释放，恢复被比对表的 inbox 应用

    std::cout << "\n--- 调用 save()，将合并决策写入本地数据库 ---\n";
    if (!session->save(&err)) {
        std::cerr << "session save failed: " << err.toStdString() << "\n";
        // 失败时 session 自动 discard，不留半写状态
    } else {
        std::cout << "save() 成功，StagingBuffer 已应用到本地库\n";
    }

    // ── 9. 展示合并后的本地数据库状态 ───────────────────────────────────────
    std::cout << "\n"
              << Color::Bold << "════════════════════════════════════════════\n"
              << "  合并后本地数据库状态\n"
              << "════════════════════════════════════════════\n"
              << Color::Reset;
    dumpTable(localDb, QStringLiteral("products"), "local/products (合并后)");
    dumpTable(localDb, QStringLiteral("orders"), "local/orders (合并后)");

    std::cout << "\n期望结果验证：\n"
              << "  products#1 price = 6   （stageCell 精细合并）\n"
              << "  products#1 stock = 100  （保留本地原值）\n"
              << "  products#3 stock = 150  （acceptLocal，保留本地）\n"
              << "  products#4 存在         （acceptLocal，不删除本地独有行）\n"
              << "  products#5 存在         （acceptRemote，新增西瓜）\n"
              << "  orders#2 status = shipped（acceptRemote）\n"
              << "  orders#4 存在           （acceptRemote，新增）\n";

    // ── 10. discard 演示（说明"放弃"路径）──────────────────────────────────
    //
    //  若用户在 GUI 中点"Cancel"，调 discard() 即可：
    //    ① 清空 StagingBuffer（内存操作，无 IO）
    //    ② InboundTableGate 释放，恢复 inbox 应用
    //    ③ 不写任何数据到 .db
    //
    //  此处 session 已 save，再调 discard 是 no-op（演示 API 用法）。
    std::cout << "\n（演示：调用 discard() — save 后为 no-op，不影响已写入数据）\n";
    session->discard();

    // ── 11. 清理 ────────────────────────────────────────────────────────────
    session.reset();
    engine.reset();
    bridge.close();

    std::cout << "\n"
              << Color::Bold << "=================================================\n"
              << " diff-demo 完成\n"
              << "=================================================\n"
              << Color::Reset;
    return 0;
}
