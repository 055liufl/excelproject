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
 * 运行方式
 *   构建产物：build_qmake_demos/examples/diff-demo/diff-demo
 *
 *   # 环境变量：本机 shell 的 LD_LIBRARY_PATH 指向 QtCreator 自带的 Qt 5.15.2，
 *   # 与项目使用的 Qt 5.12.12 冲突，运行前需把 5.12.12 的库路径前置（否则 abort：
 *   # "Cannot mix incompatible Qt library"）。本 demo 为控制台程序（QCoreApplication），
 *   # 不加载平台插件，故无需设置 QT_QPA_PLATFORM。
 *   export LD_LIBRARY_PATH=/opt/Qt5.12.12/5.12.12/gcc_64/lib:$LD_LIBRARY_PATH
 *
 *   cd build_qmake_demos/examples/diff-demo
 *   ./diff-demo <workspace-dir>
 *   # 例：./diff-demo /tmp/diff-demo-ws   （workspace-dir 下会自动创建 local.db）
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

// kindSymbol —— 把行差异种类 RowDiffKind 映射成一个 BeyondCompare 风格的状态符号。
//   Added(+ 仅远端有) / Deleted(- 仅本地有) / Modified(~ 两端不同) / Same(空白)。
//   末尾 "  ? " 是防御性兜底：理论上 switch 已覆盖全部枚举，万一出现未知值用 ? 标记。
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

// kindColor —— 把行差异种类映射成 ANSI 颜色码（绿=新增、红=删除、黄=修改、相同=无色）。
//   与 kindSymbol 配套，让终端输出在视觉上一眼区分四类差异。
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

// printSep —— 打印一条由 '-' 组成的分隔线（默认 72 列宽），纯排版辅助。
static void printSep(int width = 72) {
    std::cout << std::string(width, '-') << "\n";
}

// printTableSummary —— 打印「表级摘要」表格，仿 BeyondCompare 的 Folder Compare 视图。
// 做什么：表头一行（表名/状态/+行/-行/~行），随后逐表打印 TableDiff，并按
//   TableDiffStatus 上色（相同=灰、不同=黄、仅本地=红、仅远端=绿）。
// 参数：diffs —— session->tableDiffs() 的结果（每张表一条表级差异摘要）。
// 副作用：向 stdout 输出；不修改任何数据。
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

// printRowDiffs —— 打印某张表的「行级差异」明细，仿 BeyondCompare 的 Table Compare 视图。
// 做什么：
//   1) 先扫一遍所有 RowDiff，收齐出现过的列名 cols（保持首次出现顺序，作为表头列）。
//   2) 打印列表头（状态符 + PrimaryKey + 各列名，列名截断到 colW-1 列宽内）。
//   3) 逐行打印：跳过 Same 行（减少噪音）；对每个单元格——若该列 changed，用
//      "本地→远端" 的箭头格式突出变化，否则原样显示本地值；缺失该列的单元格留空。
// 参数：table=表名（仅用于表头标题）；rows=该表的 RowDiff 列表（含逐列 CellDiff）。
// 边界：rows 为空时直接打印「（无差异行）」并返回。值显示均做了 left(N) 截断以对齐列宽。
// 副作用：向 stdout 输出；不修改数据。
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

// execSqls —— 在指定 SQLite 文件上顺序执行一批 SQL 语句（建库/建表/插种子数据用）。
// 做什么：临时新建一个独立连接（连接名由 dbPath+tag 拼成以避免重名）→ 打开 → 逐条 exec；
//   任一条失败即打印错误（含语句前 80 字符与 lastError 文本）、置 ok=false 并中止剩余语句。
// 资源管理：用内层 {} 作用域把 QSqlDatabase 限定其内，作用域结束后句柄析构，再
//   removeDatabase——这是 Qt 的正确释放顺序（先放句柄再移除连接，否则会警告连接占用）。
// 参数：dbPath 数据库文件路径；sqls 待执行语句；tag 连接名后缀（区分不同批次）。
// 返回：全部成功 true；任一失败 false。副作用：可能创建/修改该 .db 文件。
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

// fetchAllRows —— 把一张表的全部行读成 QList<QVariantMap>（每行一个「列名→值」映射）。
// 用途：本 demo 用它模拟「从远端获取快照行数据」，以及合并后回读本地表做结果验证。
// 做什么：临时连接打开库 → SELECT * FROM table → 用 QSqlRecord 拿到列名，逐行逐列填进
//   QVariantMap。同样用作用域 + removeDatabase 规范释放连接。
// 参数：dbPath 库路径；table 表名。返回：该表所有行（顺序即 SQLite 返回顺序）。
// 注意：demo 简化未校验 SQL 注入/错误（table 来自硬编码常量，可控）。
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

// computeChecksum —— 计算一批行的「内容校验和」（demo 简化版）。
// 做什么：对每行把各列值字符串化、用 31 进制多项式滚动哈希成一个 quint64，再把所有行哈希
//   按模 2^64 累加（quint64 溢出即天然取模），最后转十六进制字符串返回。
// 为什么「累加」而非「拼接哈希」：累加满足交换律 → 顺序无关，与真实 TableStateStore 的
//   「顺序无关模加 hash」(见 §15.7.9) 思路一致——这样两端即便行顺序不同也能算出相同校验和。
// 与生产实现的差距：这里仅为演示，强度/列序处理都做了简化；真实实现见 TableStateStore。
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

// dumpTable —— 把一张表的全部行以「| 列名=值 |」的形式平铺打印（辅助人工核对合并结果）。
// 参数：dbPath 库路径；table 表名；label 打印时的标签（如 "local/products (合并后)"）。
// 做什么：调 fetchAllRows 读全表，逐行逐列输出。纯诊断用途，无副作用（除 stdout）。
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

// main —— diff-demo 的完整驱动：把文件头那张「工作流阶段图」从头跑到尾。
// 流程（与下方各 ── N. xxx 分节一一对应）：建库建表/种子数据 → 构造远端快照 →
//   打开 DataBridge、初始化同步引擎、建 ComparisonSession → initialize 触发双级 diff →
//   可视化表级/行级差异 → 模拟用户做合并决策(stageCell/acceptLocal/acceptRemote/
//   stageRow/unstage) → save 写库 → 回读验证 → 演示 discard → 清理。
// 参数：argv[1] = workspace 目录（其下自动建 local.db 及 outbox/inbox/quarantine）。
// 返回：0 成功；非 0 表示某一步失败（各失败点都打印原因后提前 return）。
int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    // Qt 5.12 appends applicationDirPath() at the END of libraryPaths(), so
    // the system QSQLITE plugin (without session support) wins. Put our
    // sqldrivers/ copy first by rebuilding the list with appDir at position 0.
    // 【译】Qt 5.12 会把 applicationDirPath() 追加到 libraryPaths() 的【末尾】，导致系统自带的
    //   QSQLITE 插件（不支持 SQLite session 扩展，无法捕获变更集）优先被加载。这里把程序目录
    //   重排到列表【首位】，让我们随程序部署的 sqldrivers/ 下那份带 session 支持的插件胜出。
    //   （此即 MEMORY 里记录的 setLibraryPaths vs addLibraryPath 陷阱：必须整列重排到 0 位。）
    {
        QStringList paths;
        paths.append(app.applicationDirPath());  // 程序目录置于首位
        for (const QString& p : QCoreApplication::libraryPaths())
            if (!paths.contains(p))
                paths.append(p);  // 其余原有路径去重后追加在后
        QCoreApplication::setLibraryPaths(paths);
    }

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
