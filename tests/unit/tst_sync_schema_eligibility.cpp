// ============================================================================
// tst_sync_schema_eligibility.cpp — SchemaEligibility（同步表「资格校验」）单元测试
// ============================================================================
//
// 【被测对象】SchemaEligibility 的两个静态方法：
//   · verify(db, tables, &rejected, &err)：逐表检查「该表能不能纳入增量同步」。
//   · expandSyncTables(db, tables, &err)：把「空清单」展开为库里所有用户表；非空则原样透传。
//
// 【为什么要做资格校验（在系统中的价值）】
//   本项目的增量同步以「单列主键」作为行身份、以 changeset 捕获变更。某些 schema 与这套
//   机制不兼容，必须在初始化期就拦下（否则运行期会出更隐蔽的错），包括：
//     · 无显式主键的表（无法定位「同一行」）；
//     · 复合主键（MVP 阶段尚不支持，见 E_SYNC_COMPOSITE_PK_NOT_SUPPORTED）；
//     · 视图 / 虚表(FTS5 等)（不是可被 session 捕获的普通表）；
//     · 不存在的表名。
//   而「生成列(GENERATED)」不阻断整表——只是该列本身被排除出同步，表仍合格。
//
// 【验证的关键契约：错误如何上报】
//   注意 verify 的「拒绝原因」是逐表写入 *rejected 列表（每条形如 "表名: 原因/错误码"），
//   而【不是】写 *err。所以多数用例断言「rejected 里有以某表名开头的条目」，复合主键用例
//   还额外断言 rejected 文案里含 E_SYNC_COMPOSITE_PK_NOT_SUPPORTED 错误码。这种「整体
//   失败但逐表给原因」的设计支持「部分拒绝」（见 verify_mixedSet_partialRejection）。
//
// 【夹具】每个用例用 init/cleanup 建/拆一个独立的内存 SQLite 连接（:memory:），名字带
//   UUID 避免并发用例撞名；exec() 是建表小工具（失败只 qWarning，不中断）。
// 【框架】Qt Test，QTEST_APPLESS_MAIN（无 GUI，纯逻辑测试）。
// ============================================================================
#include "dbridge/Errors.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "sync/schema/SchemaEligibility.h"

using namespace dbridge::sync;

class TstSyncSchemaEligibility : public QObject {
    Q_OBJECT
    QString conn_;     // 本用例专属的连接名（带 UUID，防并发撞名）
    QSqlDatabase db_;  // 内存 SQLite 连接

    // 建表/执行 DDL 的小工具：失败只打告警、不中断（建表语句在用例里都应当成功）。
    void exec(const QString& sql) {
        QSqlQuery q(db_);
        if (!q.exec(sql))
            qWarning() << "exec failed:" << q.lastError().text() << "| SQL:" << sql;
    }

   private slots:
    // init —— 每个用例【前】自动调用：开一个全新的命名内存库，保证用例间互不干扰。
    void init() {
        conn_ =
            QStringLiteral("tst_se_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
        db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn_);
        db_.setDatabaseName(QStringLiteral(":memory:"));  // 内存库：用例结束即蒸发，零残留
        QVERIFY(db_.open());
    }
    // cleanup —— 每个用例【后】自动调用：先释放句柄再 removeDatabase（次序很重要，
    //   否则 Qt 会警告「连接仍在使用」）。
    void cleanup() {
        db_ = QSqlDatabase();  // 先丢弃本地句柄，断开对连接的引用
        if (QSqlDatabase::contains(conn_)) {
            QSqlDatabase::database(conn_).close();
            QSqlDatabase::removeDatabase(conn_);
        }
    }

    // --- eligible cases ---
    // 合格①：带显式单列主键的普通 rowid 表 → 合格。
    // GIVEN t_ok 有 `id INTEGER PRIMARY KEY NOT NULL`；WHEN verify；THEN 返回 true 且
    //   rejected 为空（QVERIFY2 第二参在失败时打印被拒清单+err，方便定位）。
    void verify_rowidExplicitSinglePk_eligible() {
        exec("CREATE TABLE t_ok (id INTEGER PRIMARY KEY NOT NULL, name TEXT)");
        QStringList rejected;
        QString err;
        bool ok = SchemaEligibility::verify(db_, {"t_ok"}, &rejected, &err);
        QVERIFY2(ok, qPrintable("Rejected: " + rejected.join(',') + " err: " + err));
        QVERIFY(rejected.isEmpty());
    }

    // 合格②：WITHOUT ROWID 表只要有单列主键也合格（资格看「是否有单列 PK」，不看是否 rowid 表）。
    // GIVEN t_wr 声明 PRIMARY KEY(id) 且 WITHOUT ROWID；WHEN verify；THEN 返回 true。
    void verify_withoutRowid_eligible() {
        exec("CREATE TABLE t_wr (id INTEGER NOT NULL, name TEXT, PRIMARY KEY(id)) WITHOUT ROWID");
        QStringList rejected;
        QString err;
        bool ok = SchemaEligibility::verify(db_, {"t_wr"}, &rejected, &err);
        QVERIFY2(ok, qPrintable("Rejected: " + rejected.join(',') + " err: " + err));
    }

    // --- rejected: no explicit PK ---
    // 拒绝①：无显式主键 → 不合格（同步无法定位「同一行」）。
    // GIVEN t_nopk 只有普通列、无 PRIMARY KEY；WHEN verify；
    // THEN 返回 false，且 rejected 里有以 "t_nopk" 开头的条目（用 lambda 即时遍历查找）。
    void verify_noExplicitPk_rejected() {
        exec("CREATE TABLE t_nopk (name TEXT, val INTEGER)");
        QStringList rejected;
        QString err;
        bool ok = SchemaEligibility::verify(db_, {"t_nopk"}, &rejected, &err);
        QVERIFY(!ok);
        QVERIFY([&]() {
            for (auto& r : rejected)
                if (r.startsWith("t_nopk"))
                    return true;
            return false;
        }());
        // err is empty for reject-path; error is conveyed via rejected entries, not *err
        // 关键契约：拒绝路径下 *err 为空——拒绝原因走 rejected 逐表条目，而非 *err（见文件头）。
    }

    // --- rejected: composite PK (MVP limitation) ---
    // 拒绝②：复合主键 → 当前 MVP 不支持（行身份用单列 PK 表示）。
    // GIVEN t_cpk 声明 PRIMARY KEY(a,b)；WHEN verify；
    // THEN 返回 false；rejected 有以 "t_cpk" 开头的条目，且其文案内嵌错误码
    //   E_SYNC_COMPOSITE_PK_NOT_SUPPORTED（断言这一点，锁定「为何被拒」的语义）。
    void verify_compositePk_rejected() {
        exec(
            "CREATE TABLE t_cpk (a INTEGER NOT NULL, b INTEGER NOT NULL, "
            "val TEXT, PRIMARY KEY(a,b))");
        QStringList rejected;
        QString err;
        bool ok = SchemaEligibility::verify(db_, {"t_cpk"}, &rejected, &err);
        QVERIFY(!ok);
        QVERIFY([&]() {
            for (auto& r : rejected)
                if (r.startsWith("t_cpk"))
                    return true;
            return false;
        }());
        // E_SYNC_COMPOSITE_PK_NOT_SUPPORTED is embedded in the rejected entry text, not *err
        // 该错误码内嵌在 rejected 条目文本里（不在 *err）；下面遍历查找以确认拒绝原因正确。
        bool hasCode = false;
        for (const auto& r : rejected)
            if (r.contains(QLatin1String(dbridge::err::E_SYNC_COMPOSITE_PK_NOT_SUPPORTED))) {
                hasCode = true;
                break;
            }
        QVERIFY(hasCode);
    }

    // --- rejected: view ---
    // 拒绝③：视图 → 不合格（视图不是可被 session 捕获/可写的实表）。
    // GIVEN 建基表 base_t，再建视图 v_ok；对 v_ok 校验；
    // THEN 返回 false，rejected 含以 "v_ok" 开头的条目。
    void verify_view_rejected() {
        exec("CREATE TABLE base_t (id INTEGER PRIMARY KEY)");
        exec("CREATE VIEW v_ok AS SELECT id FROM base_t");
        QStringList rejected;
        QString err;
        bool ok = SchemaEligibility::verify(db_, {"v_ok"}, &rejected, &err);
        QVERIFY(!ok);
        QVERIFY([&]() {
            for (auto& r : rejected)
                if (r.startsWith("v_ok"))
                    return true;
            return false;
        }());
    }

    // --- partial rejection ---
    // 部分拒绝：一批表里只要有任一不合格，verify 整体返回 false，但 rejected 只列【不合格】
    //   的那些——合格表不进 rejected。这验证了「逐表给原因」的设计能精确区分好坏表。
    // GIVEN t_good（合格）+ t_bad（无 PK，不合格）一起校验；
    // THEN 返回 false；rejected 里【没有】t_good、但【有】t_bad。
    void verify_mixedSet_partialRejection() {
        exec("CREATE TABLE t_good (id INTEGER PRIMARY KEY NOT NULL)");
        exec("CREATE TABLE t_bad  (x TEXT, y TEXT)");  // no PK（故意不合格的那张）
        QStringList rejected;
        QString err;
        bool ok = SchemaEligibility::verify(db_, {"t_good", "t_bad"}, &rejected, &err);
        QVERIFY(!ok);
        // 合格表 t_good 不应出现在 rejected 里（注意外层的 ! 取反：断言「找不到 t_good」）。
        QVERIFY(![&]() {
            for (auto& r : rejected)
                if (r.startsWith("t_good"))
                    return true;
            return false;
        }());
        QVERIFY([&]() {
            for (auto& r : rejected)
                if (r.startsWith("t_bad"))
                    return true;
            return false;
        }());
    }

    // --- generated column: table not rejected, column just excluded ---
    // 边界：含「生成列(GENERATED)」的表【不】被整表拒绝——生成列只是被排除出同步（它的值由
    //   其它列算出，无需也无法独立同步），表本身仍合格。这区分了「阻断性」与「非阻断性」缺陷。
    // GIVEN t_gen 有单列 PK 且含一个虚拟生成列 upper_name；WHEN verify；
    // THEN rejected 里【没有】t_gen（仅那一列被悄悄排除，不影响整表资格）。
    void verify_generatedColumn_tableNotRejected() {
        exec(
            "CREATE TABLE t_gen (id INTEGER PRIMARY KEY NOT NULL, "
            "name TEXT, upper_name TEXT GENERATED ALWAYS AS (UPPER(name)) VIRTUAL)");
        QStringList rejected;
        QString err;
        SchemaEligibility::verify(db_, {"t_gen"}, &rejected, &err);
        // table_gen must NOT be in rejected (generated column only excluded, not blocking)
        // 断言 t_gen 不在 rejected 中（生成列仅被排除，不构成整表阻断）。
        QVERIFY(![&]() {
            for (auto& r : rejected)
                if (r.startsWith("t_gen"))
                    return true;
            return false;
        }());
    }

    // --- expandSyncTables: empty list expands to all user tables ---
    // expandSyncTables①：传【空清单】= 「同步所有用户表」→ 应返回库中全部用户表，
    //   且自动剔除 SQLite 内部表（以 "sqlite_" 开头，如 sqlite_sequence）。
    // GIVEN 建 user1/user2，调 expandSyncTables(db, {})；
    // THEN 结果含 user1、user2，且无任何 "sqlite_" 开头的内部表。
    void expandSyncTables_empty_returnsUserTables() {
        exec("CREATE TABLE user1 (id INTEGER PRIMARY KEY)");
        exec("CREATE TABLE user2 (id INTEGER PRIMARY KEY)");
        QString err;
        QStringList tables = SchemaEligibility::expandSyncTables(db_, {}, &err);
        QVERIFY(tables.contains("user1"));
        QVERIFY(tables.contains("user2"));
        for (const QString& t : tables)
            QVERIFY(!t.startsWith("sqlite_"));  // 内部表必须被过滤掉
    }

    // expandSyncTables②：传【非空清单】= 「精确指定要同步哪些表」→ 原样透传，不扩展。
    // GIVEN 建 ua/ub，但只请求 {"ua"}；THEN 返回恰好 1 张表 "ua"（不把 ub 也带上）。
    void expandSyncTables_explicit_passthrough() {
        exec("CREATE TABLE ua (id INTEGER PRIMARY KEY)");
        exec("CREATE TABLE ub (id INTEGER PRIMARY KEY)");
        QString err;
        QStringList tables = SchemaEligibility::expandSyncTables(db_, {"ua"}, &err);
        QCOMPARE(tables.size(), 1);
        QCOMPARE(tables[0], QString("ua"));
    }

    // --- non-existent table in syncTables → rejected ---
    // 拒绝④：请求同步一张【不存在】的表 → 不合格。
    // GIVEN 库里没建任何表，却请求 {"no_such_table"}；WHEN verify；
    // THEN 返回 false，rejected 含以 "no_such_table" 开头的条目。
    void verify_missingTable_rejected() {
        QStringList rejected;
        QString err;
        bool ok = SchemaEligibility::verify(db_, {"no_such_table"}, &rejected, &err);
        QVERIFY(!ok);
        QVERIFY([&]() {
            for (auto& r : rejected)
                if (r.startsWith("no_such_table"))
                    return true;
            return false;
        }());
    }

    // --- FTS5 virtual table → rejected ---
    // 拒绝⑤：FTS5 等虚表 → 不合格（虚表由扩展模块支撑，无常规主键/不可被 changeset 捕获）。
    // GIVEN 用 fts5 建虚表 t_fts；WHEN verify；
    // THEN 返回 false，rejected 含以 "t_fts" 开头的条目。
    // 注意：若运行环境的 SQLite 未编入 FTS5，则建表会失败（exec 仅告警）——届时 verify 会
    //   因「表不存在」同样返回 false 并拒绝，断言仍成立（殊途同归）。
    void verify_fts5Virtual_rejected() {
        exec("CREATE VIRTUAL TABLE t_fts USING fts5(content)");
        QStringList rejected;
        QString err;
        bool ok = SchemaEligibility::verify(db_, {"t_fts"}, &rejected, &err);
        QVERIFY(!ok);
        QVERIFY([&]() {
            for (auto& r : rejected)
                if (r.startsWith("t_fts"))
                    return true;
            return false;
        }());
    }
};

// 无 GUI 的测试入口（纯逻辑，不需要 QApplication）；下一行引入 moc 生成的元代码。
QTEST_APPLESS_MAIN(TstSyncSchemaEligibility)
#include "tst_sync_schema_eligibility.moc"
