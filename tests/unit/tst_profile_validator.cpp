// ============================================================================
// tst_profile_validator.cpp — ProfileValidator（导入/导出配置校验器）的单元测试
// ============================================================================
//
// 【被测对象】ProfileValidator::validate(profile, catalog, headers, &errors)：
//   在「真正跑导入/导出之前」把用户写的 Profile（路由/外键注入/查找表/导出选项等）
//   与「数据库 schema 目录（SchemaCatalog）+ Excel 表头（headers）」对照检查，
//   提前拦下所有配置错误，避免运行期才暴雷。它把问题分两类报告：
//     · 致命错误 → 写入 ErrorCollector 的 errors 列表，validate 返回 false；
//     · 警告     → 写入 warnings 列表，validate 仍可返回 true（不阻断）。
//
// 【这些测试在守护哪些「配置契约」（按 §编号对应规格条款）】
//   §3.x 外键注入(fkInject)/查找表(lookup) 的合法性：from 必须是已声明路由、不许
//     混用 Excel 派生列与 lookup 派生列、不许重复 child 列、lookup 名唯一、不许级联、
//     目标列不得与已有列三方冲突。
//   orderBy 时间列可排序性：dbFormat 以 yyyy 起头或物理类型为 epochSec 才「字典序可排」，
//     否则发 W_TIME_ORDERBY_NONSORTABLE 警告（否则导出按字符串排序会得到错误时序）。
//   columnOrder 导出列序：列名必须是已知表头、不得重复、不得与原生 SQL 并用；
//     Mixed 模式下类别列(classColumn)允许出现在 columnOrder。
//
// 【测试夹具】两个 static 工厂把样板 SchemaCatalog / RouteSpec 的搭建收口，使每个用例
//   只聚焦自己要构造的「异常点」，可读性更高（见 makeBasicCatalog / makeRoute）。
// 【框架】Qt Test：每个 private slot 是一个用例；QVERIFY/QCOMPARE 是断言宏；
//   QTEST_MAIN 生成 main()。无需 init/cleanup（每个用例自建 catalog/profile）。
// ============================================================================
#include <QtTest>

#include "profile/ProfileSpec.h"
#include "profile/ProfileValidator.h"
#include "schema/SchemaCatalog.h"
#include "service/ErrorCollector.h"

using namespace dbridge::detail;

// ── makeBasicCatalog —— 夹具：搭一个最小可用的 schema 目录（三张表）─────────────
// 做什么：构造 orders / items / ref_customers 三张表，含列、主键、唯一索引，供各用例复用。
//   · orders：单列主键 order_no（故 conflict:{columns:["order_no"]} 能干净通过校验）。
//   · items ：无显式 PK，但有唯一索引 uq_items(order_no,line_no)，模拟「子表」。
//   · ref_customers：单列主键 c_no，用作 lookup 的来源参照表。
// 为什么要这样的最小集：校验器要对照 schema 判断「列是否存在/是否主键/唯一性」，
//   故必须喂一个真实结构的目录；把它收进工厂，让用例只改自己关心的部分。
// 列元组字段约定：{列名, 类型, [notNull, isPk, pkOrder]}（后三个省略即默认普通列）。
static SchemaCatalog makeBasicCatalog() {
    SchemaCatalog cat;

    TableInfo orders;
    orders.name = QStringLiteral("orders");
    // Single-column PK so conflict:{columns:["order_no"]} validates cleanly
    orders.columns.append({QStringLiteral("order_no"), QStringLiteral("TEXT"), false, true, 1});
    orders.columns.append({QStringLiteral("tenant_id"), QStringLiteral("TEXT"), false, false, 0});
    orders.columns.append({QStringLiteral("total"), QStringLiteral("REAL")});
    cat.addTable(orders);

    TableInfo items;
    items.name = QStringLiteral("items");
    items.columns.append({QStringLiteral("order_no"), QStringLiteral("TEXT")});
    items.columns.append({QStringLiteral("tenant_id"), QStringLiteral("TEXT")});
    items.columns.append({QStringLiteral("line_no"), QStringLiteral("INTEGER")});
    IndexInfo uq;
    uq.name = QStringLiteral("uq_items");
    uq.unique = true;
    uq.columns << QStringLiteral("order_no") << QStringLiteral("line_no");
    items.indexes.append(uq);
    cat.addTable(items);

    TableInfo ref;
    ref.name = QStringLiteral("ref_customers");
    ref.columns.append({QStringLiteral("c_no"), QStringLiteral("TEXT"), false, true, 1});
    ref.columns.append({QStringLiteral("c_name"), QStringLiteral("TEXT")});
    ref.columns.append({QStringLiteral("c_tier"), QStringLiteral("TEXT")});
    cat.addTable(ref);

    return cat;
}

// ── makeRoute —— 夹具：快速搭一条 RouteSpec（路由 = 写一张目标表的规则）──────────
// 做什么：给定 表名 / 冲突键列 / 「数据库列名 ↔ Excel 表头名」配对，组出一条 RouteSpec。
// 为什么：路由是 Profile 的核心单元，几乎每个用例都要建；用工厂把「逐列填 ColumnSpec」
//   这段样板收掉。dbCols 与 excelCols 按下标一一对应（取两者较短长度，多余忽略）。
// Helper: build a RouteSpec with the given table, conflict columns, and column mappings
static RouteSpec makeRoute(const QString& table, const QStringList& conflictCols,
                           const QStringList& dbCols, const QStringList& excelCols) {
    RouteSpec r;
    r.table = table;
    r.conflict.columns = conflictCols;
    for (int i = 0; i < dbCols.size() && i < excelCols.size(); ++i) {
        ColumnSpec cs;
        cs.dbColumn = dbCols[i];
        cs.source = excelCols[i];
        r.columns.append(cs);
    }
    return r;
}

class TstProfileValidator : public QObject {
    Q_OBJECT

   private slots:
    // ── §3.7 fkInject empty / missing → valid (no-op) ────────────────────────
    // 契约：没有声明任何外键注入(fkInject)是合法的——不该因「缺省」而报错。
    // GIVEN items 路由的冲突键 order_no/line_no 都从 Excel 列直接映射、fkInject 为空；
    // WHEN  校验该 Profile；
    // THEN  validate 返回 true 且无任何错误（空 fkInject 是合法的 no-op）。
    void testFkInjectEmptyIsValid() {
        SchemaCatalog cat = makeBasicCatalog();
        QStringList headers = {QStringLiteral("OrderNo"), QStringLiteral("LineNo")};

        RouteSpec r = makeRoute(QStringLiteral("items"),
                                {QStringLiteral("order_no"), QStringLiteral("line_no")},
                                {QStringLiteral("order_no"), QStringLiteral("line_no")},
                                {QStringLiteral("OrderNo"), QStringLiteral("LineNo")});
        // fkInject is empty by default — should validate fine
        ProfileSpec profile;
        profile.name = QStringLiteral("test");
        profile.sheet = QStringLiteral("S");
        profile.headerRow = 1;
        profile.routes = {r};

        ErrorCollector errors;
        ProfileValidator v;
        QVERIFY(v.validate(profile, cat, headers, &errors));
        QVERIFY(errors.empty());
    }

    // ── §3.3 fkInject from must be a declared route ───────────────────────────
    // 契约：fkInject.fromTable 必须是 Profile 里【已声明的路由】，否则父表无人写入、
    //   注入无从取值。校验器应拒绝并提示改用 lookup（从既有表查值，而非注入路由父键）。
    // GIVEN items 从「orders」注入外键，但 orders 并未作为路由声明；
    // WHEN  校验；
    // THEN  返回 false、有错误，且错误文案提到「lookup」作为替代方案（引导用户改用查找表）。
    void testFkInjectFromMustBeRoute() {
        SchemaCatalog cat = makeBasicCatalog();
        QStringList headers = {QStringLiteral("OrderNo"), QStringLiteral("LineNo")};

        RouteSpec items = makeRoute(QStringLiteral("items"),
                                    {QStringLiteral("order_no"), QStringLiteral("line_no")},
                                    {QStringLiteral("order_no"), QStringLiteral("line_no")},
                                    {QStringLiteral("OrderNo"), QStringLiteral("LineNo")});
        // Inject from "orders" which is NOT declared as a route
        FkInjectSpec fk;
        fk.fromTable = QStringLiteral("orders");
        fk.pairs.append({QStringLiteral("order_no"), QStringLiteral("order_no")});
        items.fkInject.append(fk);

        ProfileSpec profile;
        profile.name = QStringLiteral("test");
        profile.sheet = QStringLiteral("S");
        profile.headerRow = 1;
        profile.routes = {items};  // no "orders" route

        ErrorCollector errors;
        ProfileValidator v;
        QVERIFY(!v.validate(profile, cat, headers, &errors));
        QVERIFY(!errors.empty());
        // Should mention "lookups" as an alternative
        bool mentionsLookup = false;
        for (const auto& e : errors.list()) {
            if (e.message.contains(QStringLiteral("lookup"))) {
                mentionsLookup = true;
                break;
            }
        }
        QVERIFY(mentionsLookup);
    }

    // 契约（§3.3 的正面）：当 fromTable 确实是已声明路由（且建立 parent 父子关系）时，
    //   fkInject 合法。这是上一个用例的「对照组」——证明拒绝是因「未声明」而非「不许注入」。
    // GIVEN orders 作为路由声明、items.parent=orders、items 从 orders 注入 order_no，
    //   并把 items 自己的 order_no Excel 映射清掉（改由注入提供，避免「同列两个来源」冲突）；
    // WHEN  校验；
    // THEN  返回 true（若失败，QVERIFY2 把首条错误文案打印出来便于定位）。
    void testFkInjectFromAsRouteIsValid() {
        SchemaCatalog cat = makeBasicCatalog();
        QStringList headers = {QStringLiteral("OrderNo"), QStringLiteral("LineNo")};

        RouteSpec orders = makeRoute(QStringLiteral("orders"), {QStringLiteral("order_no")},
                                     {QStringLiteral("order_no")}, {QStringLiteral("OrderNo")});

        RouteSpec items = makeRoute(QStringLiteral("items"),
                                    {QStringLiteral("order_no"), QStringLiteral("line_no")},
                                    {QStringLiteral("order_no"), QStringLiteral("line_no")},
                                    {QStringLiteral("OrderNo"), QStringLiteral("LineNo")});
        items.parent = QStringLiteral("orders");  // 建立父子关系：注入须沿父子链取值
        FkInjectSpec fk;
        fk.fromTable = QStringLiteral("orders");
        fk.pairs.append({QStringLiteral("order_no"), QStringLiteral("order_no")});  // 父→子 列配对
        items.fkInject.append(fk);
        // items already maps order_no from Excel — remove it to avoid conflict
        // items 原本也从 Excel 映射 order_no；既然改由注入提供，就清掉原映射，避免
        // 「同一 db 列既来自 Excel、又来自注入」的双来源冲突（否则校验会另判一类错误）。
        items.columns.clear();
        ColumnSpec cs;
        cs.dbColumn = QStringLiteral("line_no");
        cs.source = QStringLiteral("LineNo");
        items.columns.append(cs);

        ProfileSpec profile;
        profile.name = QStringLiteral("test");
        profile.sheet = QStringLiteral("S");
        profile.headerRow = 1;
        profile.routes = {orders, items};

        ErrorCollector errors;
        ProfileValidator v;
        QVERIFY2(v.validate(profile, cat, headers, &errors),
                 errors.empty() ? "" : errors.list()[0].message.toUtf8());
    }

    // ── §3.5 Mixed lookup/Excel group rejected ────────────────────────────────
    // 契约：同一组 fkInject 的各 child 列，其来源必须「同质」——要么都来自 Excel 列、
    //   要么都来自 lookup 产出，不许在一组里混用。混用会让注入时机/取值语义不一致。
    // GIVEN items 一组 fkInject 同时注入 order_no（Excel 派生）与 tenant_id（由 orders 上
    //   的 lookup 产出）；
    // WHEN  校验；
    // THEN  返回 false、有错误（识别出该组「Excel 派生 + lookup 派生」混合，予以拒绝）。
    void testFkInjectMixedGroupRejected() {
        SchemaCatalog cat = makeBasicCatalog();
        QStringList headers = {QStringLiteral("OrderNo"), QStringLiteral("TenantId")};

        RouteSpec orders = makeRoute(QStringLiteral("orders"), {QStringLiteral("order_no")},
                                     {QStringLiteral("order_no")}, {QStringLiteral("OrderNo")});
        // Add a lookup that produces tenant_id
        LookupSpec lk;
        lk.name = QStringLiteral("ref");
        lk.fromTable = QStringLiteral("ref_customers");
        lk.match.append({QStringLiteral("c_no"), QStringLiteral("OrderNo")});
        lk.select.append({QStringLiteral("c_tier"), QStringLiteral("tenant_id")});
        orders.lookups.append(lk);

        RouteSpec items = makeRoute(QStringLiteral("items"),
                                    {QStringLiteral("order_no"), QStringLiteral("line_no")},
                                    {QStringLiteral("line_no")}, {QStringLiteral("OrderNo")});
        items.parent = QStringLiteral("orders");
        FkInjectSpec fk;
        fk.fromTable = QStringLiteral("orders");
        // pair.first = "order_no" is Excel-derived, "tenant_id" is lookup-derived → mixed!
        fk.pairs.append({QStringLiteral("order_no"), QStringLiteral("order_no")});
        fk.pairs.append({QStringLiteral("tenant_id"), QStringLiteral("tenant_id")});
        items.fkInject.append(fk);

        ProfileSpec profile;
        profile.name = QStringLiteral("test");
        profile.sheet = QStringLiteral("S");
        profile.headerRow = 1;
        profile.routes = {orders, items};

        ErrorCollector errors;
        ProfileValidator v;
        QVERIFY(!v.validate(profile, cat, headers, &errors));
        QVERIFY(!errors.empty());
    }

    // ── §3.6 Duplicate child_column across groups rejected ────────────────────
    // 契约：同一子表里，同一个 child 列不能被「多组 fkInject」重复注入——否则该列会被
    //   两组同时写，结果不确定。
    // GIVEN items 声明两组 fkInject，且两组都注入同一个 child 列 order_no；
    // WHEN  校验；
    // THEN  返回 false、有错误（检测到重复 child 列）。
    void testFkInjectDuplicateChildColRejected() {
        SchemaCatalog cat = makeBasicCatalog();
        QStringList headers = {QStringLiteral("OrderNo")};

        RouteSpec orders = makeRoute(QStringLiteral("orders"), {QStringLiteral("order_no")},
                                     {QStringLiteral("order_no")}, {QStringLiteral("OrderNo")});

        RouteSpec items;
        items.table = QStringLiteral("items");
        items.conflict.columns << QStringLiteral("order_no") << QStringLiteral("line_no");
        items.parent = QStringLiteral("orders");
        // No Excel columns — inject everything via fkInject
        FkInjectSpec fk1;
        fk1.fromTable = QStringLiteral("orders");
        fk1.pairs.append({QStringLiteral("order_no"), QStringLiteral("order_no")});
        FkInjectSpec fk2;
        fk2.fromTable = QStringLiteral("orders");
        fk2.pairs.append({QStringLiteral("order_no"), QStringLiteral("order_no")});  // duplicate!
        items.fkInject.append(fk1);
        items.fkInject.append(fk2);

        ProfileSpec profile;
        profile.name = QStringLiteral("test");
        profile.sheet = QStringLiteral("S");
        profile.headerRow = 1;
        profile.routes = {orders, items};

        ErrorCollector errors;
        ProfileValidator v;
        QVERIFY(!v.validate(profile, cat, headers, &errors));
        QVERIFY(!errors.empty());
    }

    // ── §3.9 Lookup name uniqueness ───────────────────────────────────────────
    // 契约：同一路由内 lookup 的 name 必须唯一（name 是后续引用该查找结果的标识）。
    // GIVEN 一条 orders 路由声明两个 lookup，二者同名「ref」（仅 select 目标列不同）；
    // WHEN  校验；
    // THEN  返回 false（同名 lookup 被拒）。
    void testLookupDuplicateNameRejected() {
        SchemaCatalog cat = makeBasicCatalog();
        QStringList headers = {QStringLiteral("CustNo"), QStringLiteral("OrderNo")};

        RouteSpec r;
        r.table = QStringLiteral("orders");
        r.conflict.columns << QStringLiteral("order_no");
        ColumnSpec cs;
        cs.dbColumn = QStringLiteral("order_no");
        cs.source = QStringLiteral("OrderNo");
        r.columns.append(cs);

        LookupSpec lk1;
        lk1.name = QStringLiteral("ref");
        lk1.fromTable = QStringLiteral("ref_customers");
        lk1.match.append({QStringLiteral("c_no"), QStringLiteral("CustNo")});
        lk1.select.append({QStringLiteral("c_name"), QStringLiteral("customer_name")});

        LookupSpec lk2 = lk1;
        lk2.select[0].second = QStringLiteral("customer_tier");  // different target

        r.lookups.append(lk1);
        r.lookups.append(lk2);  // same name "ref" → duplicate

        ProfileSpec profile;
        profile.name = QStringLiteral("test");
        profile.sheet = QStringLiteral("S");
        profile.headerRow = 1;
        profile.routes = {r};

        ErrorCollector errors;
        ProfileValidator v;
        QVERIFY(!v.validate(profile, cat, headers, &errors));
    }

    // ── §3.11 dbColumn three-source uniqueness ────────────────────────────────
    // 契约：同一个数据库列只能有「唯一一个值来源」。一个 db 列的值可能来自三个渠道——
    //   ① Excel 列映射、② fkInject 注入、③ lookup 产出——三者对同一 db 列不得冲突。
    // GIVEN orders 路由把 Excel 列映射到 db 列 total，同时一个 lookup 的 select 目标也是
    //   total（即 total 同时来自 Excel 映射与 lookup 产出 → 三方冲突）；
    // WHEN  校验；
    // THEN  返回 false（同一 db 列出现多来源被拒）。
    void testLookupTargetConflictsWithExcelCol() {
        SchemaCatalog cat = makeBasicCatalog();
        QStringList headers = {QStringLiteral("CustNo"), QStringLiteral("OrderNo")};

        RouteSpec r;
        r.table = QStringLiteral("orders");
        r.conflict.columns << QStringLiteral("order_no");
        ColumnSpec cs;
        cs.dbColumn = QStringLiteral("order_no");
        cs.source = QStringLiteral("OrderNo");
        r.columns.append(cs);
        // Also mapping "total" from Excel:
        ColumnSpec cs2;
        cs2.dbColumn = QStringLiteral("total");
        cs2.source = QStringLiteral("CustNo");  // reusing CustNo as source for illustration
        r.columns.append(cs2);

        LookupSpec lk;
        lk.name = QStringLiteral("ref");
        lk.fromTable = QStringLiteral("ref_customers");
        lk.match.append({QStringLiteral("c_no"), QStringLiteral("CustNo")});
        lk.select.append({QStringLiteral("c_name"), QStringLiteral("total")});  // conflict!
        r.lookups.append(lk);

        ProfileSpec profile;
        profile.name = QStringLiteral("test");
        profile.sheet = QStringLiteral("S");
        profile.headerRow = 1;
        profile.routes = {r};

        ErrorCollector errors;
        ProfileValidator v;
        QVERIFY(!v.validate(profile, cat, headers, &errors));
    }

    // ── §3.4 Lookup cascade not allowed ──────────────────────────────────────
    // 契约：不允许 lookup 级联——即一个 lookup 的产出列不得再被另一个 lookup 当作匹配
    //   输入(match.second)。级联会引入「查找依赖查找」的求值顺序问题，故一律禁止。
    // GIVEN lk1 产出 resolved_name；lk2 又用 resolved_name 作为它的匹配输入（级联）；
    // WHEN  校验；
    // THEN  返回 false（检测到级联依赖，拒绝）。
    void testLookupCascadeRejected() {
        SchemaCatalog cat = makeBasicCatalog();
        // "c_name" would be a lookup output from lk1, used as match.second in lk2 → cascade
        // lk1 产出 resolved_name；若 lk2 再拿它当 match 输入，就是「查找套查找」的级联。
        QStringList headers = {QStringLiteral("CustNo"), QStringLiteral("OrderNo")};

        RouteSpec r;
        r.table = QStringLiteral("orders");
        r.conflict.columns << QStringLiteral("order_no");
        ColumnSpec cs;
        cs.dbColumn = QStringLiteral("order_no");
        cs.source = QStringLiteral("OrderNo");
        r.columns.append(cs);

        LookupSpec lk1;
        lk1.name = QStringLiteral("step1");
        lk1.fromTable = QStringLiteral("ref_customers");
        lk1.match.append({QStringLiteral("c_no"), QStringLiteral("CustNo")});
        lk1.select.append({QStringLiteral("c_name"), QStringLiteral("resolved_name")});

        LookupSpec lk2;
        lk2.name = QStringLiteral("step2");
        lk2.fromTable = QStringLiteral("ref_customers");
        lk2.match.append({QStringLiteral("c_no"), QStringLiteral("resolved_name")});  // cascade!
        lk2.select.append({QStringLiteral("c_tier"), QStringLiteral("resolved_tier")});

        r.lookups.append(lk1);
        r.lookups.append(lk2);

        ProfileSpec profile;
        profile.name = QStringLiteral("test");
        profile.sheet = QStringLiteral("S");
        profile.headerRow = 1;
        profile.routes = {r};

        ErrorCollector errors;
        ProfileValidator v;
        QVERIFY(!v.validate(profile, cat, headers, &errors));
    }

    // ---- orderBy warning tests ----
    // 背景：导出时按某个时间列 ORDER BY，实际是按它在库里的「字符串/数值表示」排序。
    //   只有当该表示是「字典序即时间序」时，排序结果才正确。校验器据 dbFormat 判断：
    //   以 yyyy 起头的格式（如 yyyy-MM-dd）字典序==时间序，安全；d/M/yyyy 这类则不安全，
    //   会发 W_TIME_ORDERBY_NONSORTABLE 警告；epochSec（整数时间戳）天然数值可排，安全。

    // 契约：dbFormat 以 yyyy 起头 → 字典序可排 → 不发警告。
    // GIVEN order_date 列声明时间槽，db 格式 "yyyy-MM-dd"，且 orderBy 引用它；
    // WHEN  校验；
    // THEN  warnings 为空（可安全按字符串排序）。
    void testOrderByTemporalSortableNoWarning() {
        // dbFormat starts with yyyy → dict-sort safe, no warning
        // db 格式以 yyyy 起头 → 字典序即时间序，可安全排序 → 无警告。
        auto cat = makeBasicCatalog();
        // Add order_date column to catalog
        // 向目录里的 orders 表临时追加 order_date 列。const_cast 去掉 table() 返回的 const，
        // 直接改目录——测试场景下可接受的写法（仅为构造被测结构，非生产代码）。
        TableInfo& tbl = *const_cast<TableInfo*>(cat.table(QStringLiteral("orders")));
        tbl.columns.append({QStringLiteral("order_date"), QStringLiteral("TEXT")});
        QStringList headers;
        headers << QStringLiteral("OrderNo") << QStringLiteral("OrderDate");

        ColumnSpec idCol;
        idCol.dbColumn = QStringLiteral("order_no");
        idCol.source = QStringLiteral("OrderNo");

        ColumnSpec dateCol;  // 声明一个带「日期时间槽」的列：excel 输入格式 + db 输出格式
        dateCol.dbColumn = QStringLiteral("order_date");
        dateCol.source = QStringLiteral("OrderDate");
        dateCol.dateFormat.declared = true;
        dateCol.dateFormat.excel.declared = true;
        dateCol.dateFormat.excel.format = QStringLiteral("yyyy/M/d");
        dateCol.dateFormat.db.declared = true;
        dateCol.dateFormat.db.format = QStringLiteral("yyyy-MM-dd");  // 关键：db 格式以 yyyy 起头

        RouteSpec r;
        r.table = QStringLiteral("orders");
        r.conflict.columns << QStringLiteral("order_no");
        r.columns << idCol << dateCol;

        ProfileSpec profile;
        profile.name = QStringLiteral("test");
        profile.sheet = QStringLiteral("S");
        profile.headerRow = 1;
        profile.routes << r;
        profile.exportSpec.orderBy << QStringLiteral("order_date");

        ErrorCollector errors;
        ProfileValidator v;
        v.validate(profile, cat, headers, &errors);
        QVERIFY(errors.warnings().isEmpty());
    }

    // 契约：dbFormat 不以 yyyy 起头（如 d/M/yyyy）→ 字典序≠时间序 → 必须发警告。
    // GIVEN order_date 的 db 格式为 "d/M/yyyy"，orderBy 引用它；
    // WHEN  校验；
    // THEN  warnings 非空，且首条警告码为 W_TIME_ORDERBY_NONSORTABLE（提醒用户排序不可靠）。
    void testOrderByTemporalNonSortableWarning() {
        // dbFormat = "d/M/yyyy" does not start with yyyy → W_TIME_ORDERBY_NONSORTABLE
        // db 格式 "d/M/yyyy" 不以 yyyy 起头 → 按字符串排会得到错误时序 → 发该警告。
        auto cat = makeBasicCatalog();
        TableInfo& tbl = *const_cast<TableInfo*>(cat.table(QStringLiteral("orders")));
        tbl.columns.append({QStringLiteral("order_date"), QStringLiteral("TEXT")});
        QStringList headers;
        headers << QStringLiteral("OrderNo") << QStringLiteral("OrderDate");

        ColumnSpec idCol;
        idCol.dbColumn = QStringLiteral("order_no");
        idCol.source = QStringLiteral("OrderNo");

        ColumnSpec dateCol;
        dateCol.dbColumn = QStringLiteral("order_date");
        dateCol.source = QStringLiteral("OrderDate");
        dateCol.dateFormat.declared = true;
        dateCol.dateFormat.excel.declared = true;
        dateCol.dateFormat.excel.format = QStringLiteral("d/M/yyyy");
        dateCol.dateFormat.db.declared = true;
        dateCol.dateFormat.db.format = QStringLiteral("d/M/yyyy");

        RouteSpec r;
        r.table = QStringLiteral("orders");
        r.conflict.columns << QStringLiteral("order_no");
        r.columns << idCol << dateCol;

        ProfileSpec profile;
        profile.name = QStringLiteral("test");
        profile.sheet = QStringLiteral("S");
        profile.headerRow = 1;
        profile.routes << r;
        profile.exportSpec.orderBy << QStringLiteral("order_date");

        ErrorCollector errors;
        ProfileValidator v;
        v.validate(profile, cat, headers, &errors);
        QVERIFY(!errors.warnings().isEmpty());
        QCOMPARE(errors.warnings()[0].code, QStringLiteral("W_TIME_ORDERBY_NONSORTABLE"));
    }

    // 7.5.1: orderBy column with db.type=epochSec → no W_TIME_ORDERBY_NONSORTABLE warning
    // 契约：db 物理类型为 epochSec（整数 Unix 时间戳）的时间列，天然按数值即时间序，
    //   故对其 orderBy 不该发可排序性警告。
    // GIVEN happen_at 列 db 物理类型 = EpochSec，orderBy 引用它；
    // WHEN  校验；
    // THEN  warnings 为空。
    void testOrderByEpochSecNoWarning() {
        auto cat = makeBasicCatalog();
        TableInfo& tbl = *const_cast<TableInfo*>(cat.table(QStringLiteral("orders")));
        tbl.columns.append({QStringLiteral("happen_at"), QStringLiteral("INTEGER")});
        QStringList headers;
        headers << QStringLiteral("OrderNo") << QStringLiteral("HappenAt");

        ColumnSpec idCol;
        idCol.dbColumn = QStringLiteral("order_no");
        idCol.source = QStringLiteral("OrderNo");

        ColumnSpec tsCol;
        tsCol.dbColumn = QStringLiteral("happen_at");
        tsCol.source = QStringLiteral("HappenAt");
        tsCol.datetimeFormat.declared = true;
        tsCol.datetimeFormat.excel.declared = true;
        tsCol.datetimeFormat.excel.format = QStringLiteral("yyyy-MM-dd HH:mm:ss");
        tsCol.datetimeFormat.db.declared = true;
        tsCol.datetimeFormat.db.type = TemporalPhysType::EpochSec;

        RouteSpec r;
        r.table = QStringLiteral("orders");
        r.conflict.columns << QStringLiteral("order_no");
        r.columns << idCol << tsCol;

        ProfileSpec profile;
        profile.name = QStringLiteral("test");
        profile.sheet = QStringLiteral("S");
        profile.headerRow = 1;
        profile.routes << r;
        profile.exportSpec.orderBy << QStringLiteral("happen_at");

        ErrorCollector errors;
        ProfileValidator v;
        v.validate(profile, cat, headers, &errors);
        QVERIFY(errors.warnings().isEmpty());
    }

    // 契约：orderBy 落在普通（非时间）列上时，与时间可排序性无关，不发时间类警告。
    // GIVEN orderBy 引用普通文本列 order_no（未声明任何时间槽）；
    // WHEN  校验；
    // THEN  warnings 为空（时间可排序性检查只针对时间列）。
    void testOrderByNonTemporalNoWarning() {
        // orderBy on a plain text column → no temporal warning
        // orderBy 落在普通文本列上 → 不触发任何时间相关警告。
        auto cat = makeBasicCatalog();
        QStringList headers;
        headers << QStringLiteral("OrderNo");

        ColumnSpec idCol;
        idCol.dbColumn = QStringLiteral("order_no");
        idCol.source = QStringLiteral("OrderNo");

        RouteSpec r;
        r.table = QStringLiteral("orders");
        r.conflict.columns << QStringLiteral("order_no");
        r.columns << idCol;

        ProfileSpec profile;
        profile.name = QStringLiteral("test");
        profile.sheet = QStringLiteral("S");
        profile.headerRow = 1;
        profile.routes << r;
        profile.exportSpec.orderBy << QStringLiteral("order_no");

        ErrorCollector errors;
        ProfileValidator v;
        v.validate(profile, cat, headers, &errors);
        QVERIFY(errors.warnings().isEmpty());
    }

    // ---- columnOrder validation tests ----
    // 背景：exportSpec.columnOrder 让用户指定导出时「哪些列排在前、按什么次序」。校验器
    //   要保证其中每个名字都是已知表头、不重复、不与原生 SQL 并用（原生 SQL 已自定列序）。

    // 契约：columnOrder 里出现「未知表头」→ E_EXPORT_UNKNOWN_HEADER。
    // GIVEN columnOrder 含 "NoSuchCol"（不是任何 ColumnSpec.source）；
    // WHEN  校验；
    // THEN  错误列表里出现 E_EXPORT_UNKNOWN_HEADER，且其文案点名 "NoSuchCol"（便于用户定位）。
    void testColumnOrderUnknownHeaderError() {
        // "NoSuchCol" is not a known ColumnSpec.source → E_EXPORT_UNKNOWN_HEADER
        // "NoSuchCol" 不是任何已映射表头 → 报未知表头错误。
        auto cat = makeBasicCatalog();
        QStringList headers;
        headers << QStringLiteral("OrderNo");

        RouteSpec r = makeRoute(QStringLiteral("orders"), {QStringLiteral("order_no")},
                                {QStringLiteral("order_no")}, {QStringLiteral("OrderNo")});

        ProfileSpec profile;
        profile.name = QStringLiteral("test");
        profile.sheet = QStringLiteral("S");
        profile.headerRow = 1;
        profile.routes << r;
        profile.exportSpec.columnOrder << QStringLiteral("OrderNo") << QStringLiteral("NoSuchCol");

        ErrorCollector errors;
        ProfileValidator v;
        v.validate(profile, cat, headers, &errors);

        bool found = false;
        for (const auto& e : errors.list()) {
            if (e.code == QStringLiteral("E_EXPORT_UNKNOWN_HEADER")) {
                found = true;
                QVERIFY(e.message.contains(QStringLiteral("NoSuchCol")));
            }
        }
        QVERIFY(found);
    }

    // 契约：表头匹配「区分大小写」——"orderno" 不等于 "OrderNo"，按未知表头处理。
    // GIVEN columnOrder 含小写 "orderno"，而实际表头是 "OrderNo"；
    // WHEN  校验；
    // THEN  报 E_EXPORT_UNKNOWN_HEADER（证明匹配大小写敏感，避免悄悄「猜中」错误列）。
    void testColumnOrderCaseSensitivityError() {
        // "orderno" (lowercase) does not match source "OrderNo" → E_EXPORT_UNKNOWN_HEADER
        // 小写 "orderno" 与 "OrderNo" 大小写不符 → 视为未知表头。
        auto cat = makeBasicCatalog();
        QStringList headers;
        headers << QStringLiteral("OrderNo");

        RouteSpec r = makeRoute(QStringLiteral("orders"), {QStringLiteral("order_no")},
                                {QStringLiteral("order_no")}, {QStringLiteral("OrderNo")});

        ProfileSpec profile;
        profile.name = QStringLiteral("test");
        profile.sheet = QStringLiteral("S");
        profile.headerRow = 1;
        profile.routes << r;
        profile.exportSpec.columnOrder << QStringLiteral("orderno");

        ErrorCollector errors;
        ProfileValidator v;
        v.validate(profile, cat, headers, &errors);

        bool found = false;
        for (const auto& e : errors.list())
            if (e.code == QStringLiteral("E_EXPORT_UNKNOWN_HEADER"))
                found = true;
        QVERIFY(found);
    }

    // 契约：Mixed（多类别）模式下，类别判定列 classColumn 本身也是一个有效表头，允许
    //   出现在 columnOrder 中而不报「未知表头」（它虽不来自某条路由的列映射，却是真实表头）。
    // GIVEN Mixed 模式，classColumn="Type"，columnOrder 含 "Type" 与 "OrderNo"；
    // WHEN  校验；
    // THEN  错误列表里【没有】E_EXPORT_UNKNOWN_HEADER（classColumn 被接纳）。
    void testColumnOrderClassColumnAcceptedInMixed() {
        // Mixed mode: classColumn = "Type" in columnOrder → should be accepted (no error)
        // Mixed 模式下，类别列 "Type" 出现在 columnOrder 中应被接受（不报未知表头）。
        auto cat = makeBasicCatalog();
        QStringList headers;
        headers << QStringLiteral("OrderNo") << QStringLiteral("Type");

        RouteSpec r = makeRoute(QStringLiteral("orders"), {QStringLiteral("order_no")},
                                {QStringLiteral("order_no")}, {QStringLiteral("OrderNo")});

        ClassSpec cls;
        cls.id = QStringLiteral("A");
        cls.matchEquals = QStringLiteral("A");
        cls.routes << r;

        ProfileSpec profile;
        profile.name = QStringLiteral("test");
        profile.sheet = QStringLiteral("S");
        profile.headerRow = 1;
        profile.mode = ProfileMode::Mixed;
        profile.classes << cls;
        profile.exportSpec.classColumn = QStringLiteral("Type");
        profile.exportSpec.columnOrder << QStringLiteral("Type") << QStringLiteral("OrderNo");

        ErrorCollector errors;
        ProfileValidator v;
        v.validate(profile, cat, headers, &errors);

        bool hasUnknownHeader = false;
        for (const auto& e : errors.list())
            if (e.code == QStringLiteral("E_EXPORT_UNKNOWN_HEADER"))
                hasUnknownHeader = true;
        QVERIFY(!hasUnknownHeader);
    }

    // 契约：columnOrder 中同一表头不得出现两次 → E_EXPORT_DUPLICATE_ORDER。
    // GIVEN columnOrder 把 "OrderNo" 列了两遍；
    // WHEN  校验；
    // THEN  报 E_EXPORT_DUPLICATE_ORDER（重复列序无意义，且会让最终列序歧义）。
    void testColumnOrderDuplicateError() {
        // "OrderNo" appears twice → E_EXPORT_DUPLICATE_ORDER
        // "OrderNo" 重复出现 → 报重复列序错误。
        auto cat = makeBasicCatalog();
        QStringList headers;
        headers << QStringLiteral("OrderNo");

        RouteSpec r = makeRoute(QStringLiteral("orders"), {QStringLiteral("order_no")},
                                {QStringLiteral("order_no")}, {QStringLiteral("OrderNo")});

        ProfileSpec profile;
        profile.name = QStringLiteral("test");
        profile.sheet = QStringLiteral("S");
        profile.headerRow = 1;
        profile.routes << r;
        profile.exportSpec.columnOrder << QStringLiteral("OrderNo") << QStringLiteral("OrderNo");

        ErrorCollector errors;
        ProfileValidator v;
        v.validate(profile, cat, headers, &errors);

        bool found = false;
        for (const auto& e : errors.list())
            if (e.code == QStringLiteral("E_EXPORT_DUPLICATE_ORDER"))
                found = true;
        QVERIFY(found);
    }

    // 契约：当导出使用原生 SQL(explicitSql) 时，列序已由该 SQL 的 SELECT 决定，不允许
    //   再叠加 columnOrder（二者会冲突）→ E_EXPORT_ORDER_WITH_RAW_SQL。
    // GIVEN exportSpec 同时设置 explicitSql 与 columnOrder；
    // WHEN  校验；
    // THEN  报 E_EXPORT_ORDER_WITH_RAW_SQL。
    void testColumnOrderWithExplicitSqlError() {
        // explicitSql + columnOrder → E_EXPORT_ORDER_WITH_RAW_SQL
        // 原生 SQL 与 columnOrder 不可并用（列序冲突）。
        auto cat = makeBasicCatalog();
        QStringList headers;
        headers << QStringLiteral("OrderNo");

        RouteSpec r = makeRoute(QStringLiteral("orders"), {QStringLiteral("order_no")},
                                {QStringLiteral("order_no")}, {QStringLiteral("OrderNo")});

        ProfileSpec profile;
        profile.name = QStringLiteral("test");
        profile.sheet = QStringLiteral("S");
        profile.headerRow = 1;
        profile.routes << r;
        profile.exportSpec.explicitSql = QStringLiteral("SELECT * FROM orders");
        profile.exportSpec.columnOrder << QStringLiteral("OrderNo");

        ErrorCollector errors;
        ProfileValidator v;
        v.validate(profile, cat, headers, &errors);

        bool found = false;
        for (const auto& e : errors.list())
            if (e.code == QStringLiteral("E_EXPORT_ORDER_WITH_RAW_SQL"))
                found = true;
        QVERIFY(found);
    }

    // 契约（正面/对照组）：columnOrder 只列出已知表头的「子集」、无重复、无原生 SQL →
    //   完全合法（未列出的列由 reorderHeaders 按自然序补在后面，见 ExportHelpers.h）。
    // GIVEN columnOrder = ["Total","OrderNo"]，都是已知表头、不重复、无 explicitSql；
    // WHEN  校验；
    // THEN  错误列表为空（部分指定列序是允许的常规用法）。
    void testColumnOrderValidSubsetIsOk() {
        // subset of known headers, no duplicates → no errors
        // 已知表头的子集、无重复 → 合法，无错误。
        auto cat = makeBasicCatalog();
        QStringList headers;
        headers << QStringLiteral("OrderNo") << QStringLiteral("TenantId")
                << QStringLiteral("Total");

        RouteSpec r = makeRoute(
            QStringLiteral("orders"), {QStringLiteral("order_no")},
            {QStringLiteral("order_no"), QStringLiteral("tenant_id"), QStringLiteral("total")},
            {QStringLiteral("OrderNo"), QStringLiteral("TenantId"), QStringLiteral("Total")});

        ProfileSpec profile;
        profile.name = QStringLiteral("test");
        profile.sheet = QStringLiteral("S");
        profile.headerRow = 1;
        profile.routes << r;
        profile.exportSpec.columnOrder << QStringLiteral("Total") << QStringLiteral("OrderNo");

        ErrorCollector errors;
        ProfileValidator v;
        v.validate(profile, cat, headers, &errors);
        QVERIFY(errors.list().isEmpty());
    }
};

// QTEST_MAIN：生成 QApplication 版 main()（ProfileValidator 测试不涉及 GUI，但 QTEST_MAIN
//   提供完整事件循环也无妨）。下一行 #include 引入 moc 为本文件 Q_OBJECT 类生成的元代码。
QTEST_MAIN(TstProfileValidator)
#include "tst_profile_validator.moc"
