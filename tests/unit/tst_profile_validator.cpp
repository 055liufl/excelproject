#include <QtTest>

#include "profile/ProfileSpec.h"
#include "profile/ProfileValidator.h"
#include "schema/SchemaCatalog.h"
#include "service/ErrorCollector.h"

using namespace dbridge::detail;

// Helper: build a minimal SchemaCatalog with named tables and column sets
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

    void testFkInjectFromAsRouteIsValid() {
        SchemaCatalog cat = makeBasicCatalog();
        QStringList headers = {QStringLiteral("OrderNo"), QStringLiteral("LineNo")};

        RouteSpec orders = makeRoute(QStringLiteral("orders"), {QStringLiteral("order_no")},
                                     {QStringLiteral("order_no")}, {QStringLiteral("OrderNo")});

        RouteSpec items = makeRoute(QStringLiteral("items"),
                                    {QStringLiteral("order_no"), QStringLiteral("line_no")},
                                    {QStringLiteral("order_no"), QStringLiteral("line_no")},
                                    {QStringLiteral("OrderNo"), QStringLiteral("LineNo")});
        items.parent = QStringLiteral("orders");
        FkInjectSpec fk;
        fk.fromTable = QStringLiteral("orders");
        fk.pairs.append({QStringLiteral("order_no"), QStringLiteral("order_no")});
        items.fkInject.append(fk);
        // items already maps order_no from Excel — remove it to avoid conflict
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
    void testLookupCascadeRejected() {
        SchemaCatalog cat = makeBasicCatalog();
        // "c_name" would be a lookup output from lk1, used as match.second in lk2 → cascade
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

    void testOrderByTemporalSortableNoWarning() {
        // dbFormat starts with yyyy → dict-sort safe, no warning
        auto cat = makeBasicCatalog();
        // Add order_date column to catalog
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
        dateCol.dateFormat.excel.format = QStringLiteral("yyyy/M/d");
        dateCol.dateFormat.db.declared = true;
        dateCol.dateFormat.db.format = QStringLiteral("yyyy-MM-dd");

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

    void testOrderByTemporalNonSortableWarning() {
        // dbFormat = "d/M/yyyy" does not start with yyyy → W_TIME_ORDERBY_NONSORTABLE
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

    void testOrderByNonTemporalNoWarning() {
        // orderBy on a plain text column → no temporal warning
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

    void testColumnOrderUnknownHeaderError() {
        // "NoSuchCol" is not a known ColumnSpec.source → E_EXPORT_UNKNOWN_HEADER
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

    void testColumnOrderCaseSensitivityError() {
        // "orderno" (lowercase) does not match source "OrderNo" → E_EXPORT_UNKNOWN_HEADER
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

    void testColumnOrderClassColumnAcceptedInMixed() {
        // Mixed mode: classColumn = "Type" in columnOrder → should be accepted (no error)
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

    void testColumnOrderDuplicateError() {
        // "OrderNo" appears twice → E_EXPORT_DUPLICATE_ORDER
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

    void testColumnOrderWithExplicitSqlError() {
        // explicitSql + columnOrder → E_EXPORT_ORDER_WITH_RAW_SQL
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

    void testColumnOrderValidSubsetIsOk() {
        // subset of known headers, no duplicates → no errors
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

QTEST_MAIN(TstProfileValidator)
#include "tst_profile_validator.moc"
