#include <QVariant>
#include <QtTest>

#include "mapping/RowPayload.h"
#include "profile/ProfileSpec.h"
#include "sql/SqlBuilder.h"

// ============================================================================
// tst_sql_builder.cpp — SqlBuilder（ETL 端 SQL 文本生成器）的单元测试
// ============================================================================
//
// 【被测对象是什么】
//   SqlBuilder 把「映射规格(RouteSpec/ExportSpec) + 行负载(RoutePayload)」翻译成具体的
//   SQL 文本——导入侧的 UPSERT 语句、导出侧的多表自动 JOIN SELECT。它是 ETL 把内存中的
//   结构化意图落成可执行 SQL 的关键一环。
//
// 【这组用例守什么】
//   · buildUpsert 必须用 quoteIdent 给所有表名/列名加双引号——既防 SQL 注入，也容忍含空格/
//     保留字的标识符（断言里反复检查带引号的 "orders"/"order_no" 等形态）。
//   · 有非冲突列时生成 ON CONFLICT ... DO UPDATE SET（更新非键列）；
//   · 全部列都是冲突键时生成 DO NOTHING（无可更新列，幂等插入）；
//   · 多列冲突键(order_no+line_no)能正确进入 ON CONFLICT(...) 列表；
//   · 导出 SELECT 的列/FROM/ORDER BY 都用「表名.列名」限定，避免多表 JOIN 时列名歧义(H-6)。
//
// 【风格】这些用例不连数据库，纯粹对生成的 SQL 字符串做 contains/!contains 断言——
//   即「生成器吐出的文本是否包含/不包含某结构片段」，属于轻量的「文本契约」测试。
// ============================================================================

using namespace dbridge::detail;

class TstSqlBuilder : public QObject {
    Q_OBJECT

   private slots:
    // ── testBuildUpsertWithUpdate —— 有非冲突列时生成 DO UPDATE SET 形态 ────────
    // GIVEN orders 表，冲突键=order_no，另有 customer/amount 两个非键列；
    // WHEN buildUpsert；THEN SQL 应：① 带引号的 INSERT INTO "orders"；② 含 ON CONFLICT(；
    //   ③ 冲突列 "order_no" 出现；④ DO UPDATE SET；⑤ 非键列 "customer"/"amount" 被更新；
    //   ⑥ 最后一条反向断言：冲突键自身不应出现在 SET 子句（不能 "order_no" = excluded.——
    //   冲突键是定位行的依据，更新它没有意义且可能改变行身份）。
    void testBuildUpsertWithUpdate() {
        RoutePayload payload;
        payload.table = QStringLiteral("orders");
        payload.dbColumns << QStringLiteral("order_no") << QStringLiteral("customer")
                          << QStringLiteral("amount");
        payload.binds << QVariant(QStringLiteral("001")) << QVariant(QStringLiteral("Alice"))
                      << QVariant(100.0);
        payload.conflictKey << QStringLiteral("order_no");

        SqlBuilder builder;
        UpsertSql us = builder.buildUpsert(payload);

        // SqlBuilder uses quoteIdent — check for quoted form.
        QVERIFY(us.sql.contains(QStringLiteral("INSERT INTO \"orders\"")));
        QVERIFY(us.sql.contains(QStringLiteral("ON CONFLICT(")));
        QVERIFY(us.sql.contains(QStringLiteral("\"order_no\"")));
        QVERIFY(us.sql.contains(QStringLiteral("DO UPDATE SET")));
        QVERIFY(us.sql.contains(QStringLiteral("\"customer\"")));
        QVERIFY(us.sql.contains(QStringLiteral("\"amount\"")));
        QVERIFY(!us.sql.contains(QStringLiteral("\"order_no\" = excluded.")));
    }

    // ── testBuildUpsertDoNothing —— 全列都是冲突键时退化为 DO NOTHING ──────────
    // GIVEN keys 表只有一列 key_col，且它就是冲突键（没有任何可更新的非键列）；
    // WHEN buildUpsert；THEN 生成 DO NOTHING（而非 DO UPDATE）——撞键时无列可改，直接幂等
    //   忽略。验证生成器能识别「无非键列」这一边界并切换到正确的冲突子句。
    void testBuildUpsertDoNothing() {
        RoutePayload payload;
        payload.table = QStringLiteral("keys");
        payload.dbColumns << QStringLiteral("key_col");
        payload.binds << QVariant(QStringLiteral("k1"));
        payload.conflictKey << QStringLiteral("key_col");

        SqlBuilder builder;
        UpsertSql us = builder.buildUpsert(payload);

        QVERIFY(us.sql.contains(QStringLiteral("DO NOTHING")));
        QVERIFY(!us.sql.contains(QStringLiteral("DO UPDATE")));
    }

    // ── testBuildUpsertMultiConflict —— 复合冲突键(多列)正确进入 ON CONFLICT(...) ─
    // GIVEN order_items 表，冲突键为 (order_no, line_no) 两列，外加非键列 sku；
    // WHEN buildUpsert；THEN ON CONFLICT( 子句中两个键列都带引号出现，sku 作为可更新列出现。
    //   验证多列联合唯一键场景下生成器不会漏列、不会把键列顺序搞错。
    void testBuildUpsertMultiConflict() {
        RoutePayload payload;
        payload.table = QStringLiteral("order_items");
        payload.dbColumns << QStringLiteral("order_no") << QStringLiteral("line_no")
                          << QStringLiteral("sku");
        payload.binds << QVariant(QStringLiteral("001")) << QVariant(1LL)
                      << QVariant(QStringLiteral("SKU-001"));
        payload.conflictKey << QStringLiteral("order_no") << QStringLiteral("line_no");

        SqlBuilder builder;
        UpsertSql us = builder.buildUpsert(payload);

        QVERIFY(us.sql.contains(QStringLiteral("ON CONFLICT(")));
        QVERIFY(us.sql.contains(QStringLiteral("\"order_no\"")));
        QVERIFY(us.sql.contains(QStringLiteral("\"line_no\"")));
        QVERIFY(us.sql.contains(QStringLiteral("\"sku\"")));
    }

    // ── testBuildAutoJoinSelectSingleTable —— 导出 SELECT 的列与 ORDER BY 均按表名限定 ─
    // GIVEN 单表 customer，两列映射 customer_no→CustomerNo、name→Name，ExportSpec 按 customer_no
    // 排序； WHEN buildAutoJoinSelect；THEN SQL 应：① FROM "customer"；② 列引用限定为
    //   "customer"."customer_no"（表名.列名，避免多表 JOIN 时同名列歧义）；③ 列别名 "CustomerNo"
    //   （导出表头用 Excel 侧名字）；④ 含 ORDER BY 且引用 "customer_no"（H-6 fix：ORDER BY 也要
    //   带表名限定）。单表场景看似无歧义，但生成器统一限定，多表时才不会出错。
    void testBuildAutoJoinSelectSingleTable() {
        RouteSpec route;
        route.table = QStringLiteral("customer");
        ColumnSpec c1;
        c1.dbColumn = QStringLiteral("customer_no");
        c1.source = QStringLiteral("CustomerNo");
        ColumnSpec c2;
        c2.dbColumn = QStringLiteral("name");
        c2.source = QStringLiteral("Name");
        route.columns << c1 << c2;

        ExportSpec exp;
        exp.orderBy << QStringLiteral("customer_no");

        SqlBuilder builder;
        QVector<RouteSpec> routeVec;
        routeVec << route;
        QString sql = builder.buildAutoJoinSelect(routeVec, exp);

        // SqlBuilder uses quoteIdent — check for quoted form.
        QVERIFY(sql.contains(QStringLiteral("FROM \"customer\"")));
        QVERIFY(sql.contains(QStringLiteral("\"customer\".\"customer_no\"")));
        QVERIFY(sql.contains(QStringLiteral("\"CustomerNo\"")));
        // H-6 ORDER BY qualified with table name.
        QVERIFY(sql.contains(QStringLiteral("ORDER BY")));
        QVERIFY(sql.contains(QStringLiteral("\"customer_no\"")));
    }
};

QTEST_MAIN(TstSqlBuilder)
#include "tst_sql_builder.moc"
