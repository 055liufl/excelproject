#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <QtTest>

#include "mapping/RowPayload.h"
#include "profile/ProfileSpec.h"
#include "service/ErrorCollector.h"
#include "validation/ForeignKeyPreflight.h"

// ============================================================================
// tst_fk_preflight.cpp — ForeignKeyPreflight（导入前的外键预检）的单元测试
// ============================================================================
//
// 【被测对象解决什么问题】
//   ETL 把一批 Excel 行写入有外键约束的表时，子表行(order_items)引用的父表行(orders)
//   可能：① 就在同一批待导入数据里(in-batch)；② 已经在库里(in-DB)；③ 两处都没有(缺失)。
//   情形 ③ 直接 INSERT 会触发数据库外键错误、且报错信息晦涩、定位困难。ForeignKeyPreflight
//   在真正写库「之前」先把每个子行的父引用查一遍，把「父不存在」提前变成一条精确的、带
//   行号/列名/原值的业务错误(E_VALIDATE_FK)，让用户一眼定位是哪一行的哪个键引用了不存在的父。
//
// 【两条优化路径（用例重点验证的「为什么不查库」）】
//   · in-batch 命中：父行就在同批负载里 → 无需发 SQL 探针(probe)，直接判通过（省一次 DB 往返）。
//   · §7.5 lookup-derived 跳过：若父表的「冲突键值」是由 lookup 派生出来的(运行时才解析)，
//     则该 FK 组在预检阶段无法可靠判断，整组跳过、不发探针、不报错——留给后续真实写入处理。
//   onProbe 回调是观测「到底发没发 SQL 探针」的探针计数器，用例据此断言走了哪条路径。
//
// 【混合模式(mixed) 回归点（§7.7 / testMixedParentInBatch）】
//   多分类(classId)混合导入时，payload.routeKey 形如 "A:orders"（带 classId 前缀），但
//   ForeignKeyPreflight 在「同批找父」时必须按裸表名 "orders" 匹配，否则前缀会让它找不到
//   本就在批内的父行而误报缺失。这条回归用例专门钉住这个易错点。
//
// 【夹具组织】下方匿名命名空间里集中了所有「造数据/造规格」的工厂函数（makeXxx）与 DB 辅助，
//   让每个测试槽只描述「场景 + 断言」，可读性更高。openMemoryDb() 建一张最小的 orders 父表。
// ============================================================================

using namespace dbridge::detail;

namespace {

// makeOrdersRoute —— 造父表 orders 的路由规格：冲突键=order_no，一列映射 order_no←OrderNo。
//   RouteSpec 描述「某张表怎么从 Excel 列映射、按什么键 UPSERT」，是预检判断 FK 的依据之一。
RouteSpec makeOrdersRoute() {
    RouteSpec r;
    r.table = QStringLiteral("orders");
    r.conflict.columns << QStringLiteral("order_no");
    ColumnSpec c;
    c.dbColumn = QStringLiteral("order_no");
    c.source = QStringLiteral("OrderNo");
    r.columns << c;
    return r;
}

// makeOrderItemsRoute —— 造子表 order_items 的路由规格：声明它的父是 orders，并通过
//   fkInject 描述「子表 order_no 列引用父表 orders 的 order_no 列」这条外键关系。
//   冲突键是复合键 (order_no, line_no)。预检正是据 fkInject 知道「该去 orders 找 order_no」。
RouteSpec makeOrderItemsRoute() {
    RouteSpec r;
    r.table = QStringLiteral("order_items");
    r.parent = QStringLiteral("orders");
    r.conflict.columns << QStringLiteral("order_no") << QStringLiteral("line_no");
    FkInjectSpec fk;
    fk.fromTable = QStringLiteral("orders");  // 外键指向的父表
    fk.pairs.append({QStringLiteral("order_no"), QStringLiteral("order_no")});  // 子列→父列 对
    r.fkInject.append(fk);
    ColumnSpec lineNo;
    lineNo.dbColumn = QStringLiteral("line_no");
    lineNo.source = QStringLiteral("LineNo");
    r.columns << lineNo;
    return r;
}

// makeParentPayload —— 造一条父表 orders 的行负载(RoutePayload)，带给定 order_no 冲突键值。
// routeKey 刻意复刻 Mapper 的真实产物：classId 为空时是裸表名 "orders"，否则是 "<classId>:orders"。
//   这点对 testMixedParentInBatch 至关重要——预检必须能透过 "A:orders" 前缀按裸表名找到父。
// Build a parent payload for table "orders" with the given conflict key value.
// routeKey mirrors what Mapper would produce: bare table for classId="",
// "<classId>:<table>" otherwise.
RoutePayload makeParentPayload(const QString& classId, const QString& orderNo) {
    RoutePayload p;
    p.table = QStringLiteral("orders");
    p.routeKey = classId.isEmpty() ? p.table : classId + QLatin1Char(':') + p.table;
    p.dbColumns << QStringLiteral("order_no");
    p.binds << QVariant(orderNo);
    p.conflictKey << QStringLiteral("order_no");
    p.conflictVals << QVariant(orderNo);
    return p;
}

// makeChildPayload —— 造一条子表 order_items 的行负载，FK 列 order_no 已注入(带值)。
//   它持有对父行的引用值 orderNo；预检会拿这个值去「批内 / 库里」找对应的父 orders 行。
// Build a child payload for table "order_items" with the FK already injected.
RoutePayload makeChildPayload(const QString& classId, const QString& orderNo, int lineNo) {
    RoutePayload p;
    p.table = QStringLiteral("order_items");
    p.routeKey = classId.isEmpty() ? p.table : classId + QLatin1Char(':') + p.table;
    p.dbColumns << QStringLiteral("line_no") << QStringLiteral("order_no");
    p.binds << QVariant(lineNo) << QVariant(orderNo);
    p.conflictKey << QStringLiteral("order_no") << QStringLiteral("line_no");
    p.conflictVals << QVariant(orderNo) << QVariant(lineNo);
    return p;
}

// uniqueConn —— 用 UUID 生成唯一连接名，保证各用例的内存库彼此隔离、互不串数据。
QString uniqueConn() {
    return QStringLiteral("fkpreflight_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// openMemoryDb —— 开一张内存库并建最小父表 orders(order_no PRIMARY KEY)。
//   有的用例预先往里 INSERT 父行以模拟「父已在库」，有的故意不插以模拟「父缺失」。
QSqlDatabase openMemoryDb() {
    auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), uniqueConn());
    db.setDatabaseName(QStringLiteral(":memory:"));
    db.open();
    QSqlQuery q(db);
    q.exec(QStringLiteral("CREATE TABLE orders (order_no TEXT PRIMARY KEY)"));
    return db;
}

}  // namespace

class TstFkPreflight : public QObject {
    Q_OBJECT

   private slots:
    // ── testNonMixedParentInBatch —— 父子同批(非混合)，父在批内 → 通过、无错误 ──
    // GIVEN 同一 RowContext 里既有父 orders(SO-001) 又有子 order_items(引用 SO-001)；
    // WHEN check；THEN 返回 true 且 errors 为空——父就在批内，预检命中即放行。
    // Non-mixed multiTable: child FK references parent in same batch.
    void testNonMixedParentInBatch() {
        QSqlDatabase db = openMemoryDb();

        RowContext ctx;
        ctx.excelRow = 2;
        ctx.classId = QString();
        ctx.payloads << makeParentPayload(QString(), QStringLiteral("SO-001"))
                     << makeChildPayload(QString(), QStringLiteral("SO-001"), 1);

        QVector<RouteSpec> routes;
        routes << makeOrdersRoute() << makeOrderItemsRoute();

        ErrorCollector errors;
        ForeignKeyPreflight fk;
        QVector<RowContext> ctxList = {ctx};
        QVERIFY(fk.check(ctxList, routes, db, QStringLiteral("Orders"), &errors));
        QVERIFY(errors.empty());
    }

    // ── testMixedParentInBatch —— 回归：混合模式下仍按裸表名找批内父 ───────────
    // GIVEN classId="A"，故 payload.routeKey 是 "A:orders"/"A:order_items"（带前缀）；
    // WHEN check；THEN 仍应找到批内父并通过。这是回归保护：若预检错误地用带前缀的 routeKey
    //   去匹配父，会找不到本就在批内的父行而误报 FK 缺失。QVERIFY2 在失败时打印首条错误文本，
    //   便于直接看到「误报了什么」。
    // Regression: mixed mode. payload.routeKey is "A:orders" / "A:order_items",
    // but ForeignKeyPreflight must still find the parent by bare table name.
    void testMixedParentInBatch() {
        QSqlDatabase db = openMemoryDb();

        RowContext ctx;
        ctx.excelRow = 2;
        ctx.classId = QStringLiteral("A");
        ctx.payloads << makeParentPayload(QStringLiteral("A"), QStringLiteral("SO-001"))
                     << makeChildPayload(QStringLiteral("A"), QStringLiteral("SO-001"), 1);

        QVector<RouteSpec> routes;
        routes << makeOrdersRoute() << makeOrderItemsRoute();

        ErrorCollector errors;
        ForeignKeyPreflight fk;
        QVector<RowContext> ctxList = {ctx};
        QVERIFY(fk.check(ctxList, routes, db, QStringLiteral("Mixed"), &errors));
        QVERIFY2(errors.empty(), errors.list().isEmpty()
                                     ? "no errors"
                                     : errors.list().first().message.toUtf8().constData());
    }

    // ── testParentInDb —— 父不在批内但已在库 → 通过 ──────────────────────────
    // GIVEN 库里预先 INSERT 了父行 SO-999，本批只有子行(引用 SO-999)；
    // WHEN check；THEN 通过、无错误——预检发探针查库，命中已存在的父即放行。
    // Parent not in batch but already in DB.
    void testParentInDb() {
        QSqlDatabase db = openMemoryDb();
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral("INSERT INTO orders(order_no) VALUES('SO-999')")));

        RowContext ctx;
        ctx.excelRow = 3;
        ctx.classId = QString();
        ctx.payloads << makeChildPayload(QString(), QStringLiteral("SO-999"), 1);

        QVector<RouteSpec> routes;
        routes << makeOrdersRoute() << makeOrderItemsRoute();

        ErrorCollector errors;
        ForeignKeyPreflight fk;
        QVector<RowContext> ctxList = {ctx};
        QVERIFY(fk.check(ctxList, routes, db, QStringLiteral("Orders"), &errors));
        QVERIFY(errors.empty());
    }

    // ── testParentMissing —— 父批内、库里都没有 → 报 E_VALIDATE_FK 且错误信息精确 ─
    // GIVEN 本批只有引用 "SO-MISSING" 的子行，库里也没有该父；
    // WHEN check；THEN 返回 false 且收集到一条错误。断言不止「有错」，还逐字段核对错误的
    //   业务可读性：code=E_VALIDATE_FK、row=4（Excel 行号，便于用户定位）、column=order_no
    //   （是哪个外键列）、rawValue=SO-MISSING（用户填的原值）。这正是预检相较「让 DB 直接报错」
    //   的核心价值：把晦涩的约束异常变成可定位、可读的业务错误。
    // Parent neither in batch nor in DB -> E_VALIDATE_FK.
    void testParentMissing() {
        QSqlDatabase db = openMemoryDb();

        RowContext ctx;
        ctx.excelRow = 4;
        ctx.classId = QString();
        ctx.payloads << makeChildPayload(QString(), QStringLiteral("SO-MISSING"), 1);

        QVector<RouteSpec> routes;
        routes << makeOrdersRoute() << makeOrderItemsRoute();

        ErrorCollector errors;
        ForeignKeyPreflight fk;
        QVector<RowContext> ctxList = {ctx};
        QVERIFY(!fk.check(ctxList, routes, db, QStringLiteral("Orders"), &errors));
        QVERIFY(!errors.empty());
        QCOMPARE(errors.list().first().code, QStringLiteral("E_VALIDATE_FK"));
        QCOMPARE(errors.list().first().row, 4);
        QCOMPARE(errors.list().first().column, QStringLiteral("order_no"));
        QCOMPARE(errors.list().first().rawValue, QStringLiteral("SO-MISSING"));
    }

    // ── testInBatchHitNoProbe —— §7.7 优化：批内命中父则「不发」SQL 探针 ────────
    // GIVEN 父子同批；WHEN check（onProbe 回调每发一次库探针 +1）；
    // THEN probeCount==0——父在批内即可断定，无需查库，省一次 DB 往返。
    //   这条用例不验证「对错」，而验证「走的是哪条路径」(性能契约)。
    // §7.7 onProbe counter: parent found in-batch → NO SQL probe issued
    void testInBatchHitNoProbe() {
        QSqlDatabase db = openMemoryDb();

        RowContext ctx;
        ctx.excelRow = 2;
        ctx.classId = QString();
        ctx.payloads << makeParentPayload(QString(), QStringLiteral("SO-001"))
                     << makeChildPayload(QString(), QStringLiteral("SO-001"), 1);

        QVector<RouteSpec> routes;
        routes << makeOrdersRoute() << makeOrderItemsRoute();

        int probeCount = 0;  // 探针计数器：记录预检向库发了几次「查父是否存在」
        ErrorCollector errors;
        ForeignKeyPreflight fk;
        fk.onProbe = [&](const QString&) { ++probeCount; };  // 安装观测回调
        QVector<RowContext> ctxList = {ctx};
        QVERIFY(fk.check(ctxList, routes, db, QStringLiteral("Orders"), &errors));
        QCOMPARE(probeCount, 0);  // in-batch → no DB probe（批内命中 → 零探针）
    }

    // ── testMissParentTriggersProbe —— §7.7：父不在批内则「发」一次探针查库 ─────
    // GIVEN 库里有父 SO-999，但本批只有子行(父不在批内)；
    // WHEN check；THEN probeCount==1（恰好发一次库探针）且无错误（库里查到了父）。
    //   与上一条互补：批内未命中才退而查库，且只查必要的一次。
    // §7.7 onProbe counter: parent not in batch → SQL probe issued
    void testMissParentTriggersProbe() {
        QSqlDatabase db = openMemoryDb();
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral("INSERT INTO orders(order_no) VALUES('SO-999')")));

        RowContext ctx;
        ctx.excelRow = 3;
        ctx.classId = QString();
        ctx.payloads << makeChildPayload(QString(), QStringLiteral("SO-999"), 1);

        QVector<RouteSpec> routes;
        routes << makeOrdersRoute() << makeOrderItemsRoute();

        int probeCount = 0;
        ErrorCollector errors;
        ForeignKeyPreflight fk;
        fk.onProbe = [&](const QString&) { ++probeCount; };
        QVector<RowContext> ctxList = {ctx};
        QVERIFY(fk.check(ctxList, routes, db, QStringLiteral("Orders"), &errors));
        QCOMPARE(probeCount, 1);  // not in batch → 1 DB probe（不在批内 → 恰一次库探针）
        QVERIFY(errors.empty());
    }

    // ── testLookupDerivedGroupSkipsProbe —— §7.5：父键由 lookup 派生则整组跳过 ──
    // GIVEN 父表 orders 的冲突键 order_no 是由一个 lookup(ref_table.ref_col→order_no) 派生的；
    //   子行引用 SO-999，且故意「不」往库里插该父行。
    // WHEN check；THEN 既不发探针(probeCount==0)，也不报错——因为该 FK 组的父键要到运行时
    //   解析 lookup 才知道，预检阶段无法可靠判断「父是否存在」，按 §7.5 规则整组跳过，
    //   把判断留给后续真实写入。
    // 关键对照：这里父并不在库里(若真去探针查会查不到 → 本会报缺失)，断言「无错误」恰恰
    //   证明预检确实「跳过」了这一组、没有误把 lookup 派生场景判成 FK 失败。
    // §7.5 group-level skip: lookup-derived group → no probe even if parent not in batch
    void testLookupDerivedGroupSkipsProbe() {
        QSqlDatabase db = openMemoryDb();
        // Do NOT insert SO-999 — if probe fires, it would return false
        // 故意不插 SO-999：一旦探针真的触发，就会查不到父而返回 false——以此反证「确实跳过了」。

        // Simulate a parent route that has a lookup output "order_no"
        // 构造一个「父键来自 lookup 输出」的父路由：lookup 从 ref_table.ref_col 取值，产出
        // order_no。
        RouteSpec ordersWithLookup;
        ordersWithLookup.table = QStringLiteral("orders");
        ordersWithLookup.conflict.columns << QStringLiteral("order_no");
        LookupSpec lk;
        lk.name = QStringLiteral("ref");
        lk.fromTable = QStringLiteral("ref_table");
        lk.select.append({QStringLiteral("ref_col"),
                          QStringLiteral("order_no")});  // ← produces order_no（派生出 order_no）
        ordersWithLookup.lookups << lk;

        RouteSpec items = makeOrderItemsRoute();
        // fkInject pair.first = "order_no" → lookup-derived → group should be skipped
        // 子表 FK 的父列 order_no 正是上面 lookup 派生出的列 → 该 FK 组被判为 lookup-derived →
        // 跳过。

        RowContext ctx;
        ctx.excelRow = 5;
        ctx.classId = QString();
        ctx.payloads << makeChildPayload(QString(), QStringLiteral("SO-999"), 1);

        QVector<RouteSpec> routes;
        routes << ordersWithLookup << items;

        int probeCount = 0;
        ErrorCollector errors;
        ForeignKeyPreflight fk;
        fk.onProbe = [&](const QString&) { ++probeCount; };
        // Result: group is lookup-derived → skip → check returns true, no error
        QVector<RowContext> ctxList = {ctx};
        QVERIFY(fk.check(ctxList, routes, db, QStringLiteral("Orders"), &errors));
        QCOMPARE(probeCount, 0);  // §7.5: lookup-derived skip（lookup 派生 → 跳过，不探针）
        QVERIFY(errors.empty());
    }
};

QTEST_MAIN(TstFkPreflight)
#include "tst_fk_preflight.moc"
