// ============================================================================
// tst_databridge_schema.cpp — DataBridge 公共 schema 发现接口单元测试
// ============================================================================
//
// 【这个测试文件在验证什么】
//   DataBridge 新增的两个对外接口（见 include/dbridge/DataBridge.h / SchemaInfo.h）：
//     · userTables()    —— 列出库中「用户表」，排除 sqlite_% 与 __sync_% 元数据表，升序返回。
//     · describeTable() —— 取某表列/主键结构（列按建表列序，含主键标记与复合主键次序）。
//   这两个接口把库内部的 schema 自省结果以公共值类型暴露，供调用方「动态发现表/字段」而
//   无需自行写 PRAGMA/sqlite_master 查询（sync-suite 场景2 即依赖它做比对表信息发现）。
//
// 【测试套路】
//   GIVEN: 在一个磁盘临时库上用裸连接建若干结构已知的表（含一张 __sync_* 元数据表）；
//   WHEN : DataBridge::open() 打开该库（open 内部即做全库 schema 自省）；
//   THEN : 断言 userTables()/describeTable() 的返回与建表语句一致。
// ============================================================================

#include "dbridge/DataBridge.h"
#include "dbridge/SchemaInfo.h"
#include "dbridge/Types.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace dbridge;

class TstDataBridgeSchema : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;  // 每个用例一个独立临时目录，析构自动清理
    QString dbPath_;     // 本用例的库文件路径
    QString seedConn_;   // 建表用的裸连接名（用完注销）

    // execSeed —— 用一次性裸连接在 dbPath_ 上执行一组建表 SQL（DataBridge.open 前的准备）。
    void execSeed(const QStringList& sqls) {
        seedConn_ = QStringLiteral("tst_dbs_seed_") +
                    QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), seedConn_);
            db.setDatabaseName(dbPath_);
            QVERIFY2(db.open(), qPrintable(db.lastError().text()));
            QSqlQuery q(db);
            for (const QString& s : sqls)
                QVERIFY2(q.exec(s), qPrintable(q.lastError().text() + " SQL: " + s));
            db.close();
        }
        QSqlDatabase::removeDatabase(seedConn_);
        seedConn_.clear();
    }

   private slots:
    void init() {
        QVERIFY(tmp_.isValid());
        dbPath_ = tmp_.path() + QStringLiteral("/schema_test.db");
    }

    void cleanup() {
        QFile::remove(dbPath_);
    }

    // testUserTablesExcludesMeta —— 用户表升序返回，且排除 __sync_% 与 sqlite_% 表。
    void testUserTablesExcludesMeta() {
        execSeed({
            QStringLiteral("CREATE TABLE beta (id INTEGER PRIMARY KEY, v TEXT)"),
            QStringLiteral("CREATE TABLE alpha (id INTEGER PRIMARY KEY, v TEXT)"),
            // 模拟同步子系统元数据表：必须被 userTables() 排除。
            QStringLiteral("CREATE TABLE __sync_meta (k TEXT PRIMARY KEY, v TEXT)"),
            // 带 AUTOINCREMENT 会隐式产生 sqlite_sequence 内建表：也必须被排除。
            QStringLiteral("CREATE TABLE gamma (id INTEGER PRIMARY KEY AUTOINCREMENT, v TEXT)"),
        });

        DataBridge bridge;
        ConnectionSpec spec;
        spec.sqlitePath = dbPath_;
        QString err;
        QVERIFY2(bridge.open(spec, &err), qPrintable(err));

        const QStringList tables = bridge.userTables(&err);
        QVERIFY2(err.isEmpty(), qPrintable(err));
        // 期望：仅三张用户表，升序；不含 __sync_meta / sqlite_sequence。
        QCOMPARE(tables, (QStringList{QStringLiteral("alpha"), QStringLiteral("beta"),
                                      QStringLiteral("gamma")}));
        QVERIFY(!tables.contains(QStringLiteral("__sync_meta")));
        QVERIFY(!tables.contains(QStringLiteral("sqlite_sequence")));
        bridge.close();
    }

    // testDescribeTable —— 列序、主键标记、复合主键次序、NOT NULL 均正确。
    void testDescribeTable() {
        execSeed({
            QStringLiteral("CREATE TABLE emp ("
                           "  id INTEGER PRIMARY KEY, name TEXT NOT NULL, salary INTEGER)"),
        });

        DataBridge bridge;
        ConnectionSpec spec;
        spec.sqlitePath = dbPath_;
        QString err;
        QVERIFY2(bridge.open(spec, &err), qPrintable(err));

        TableSchema ts;
        QVERIFY2(bridge.describeTable(QStringLiteral("emp"), &ts, &err), qPrintable(err));
        QCOMPARE(ts.table, QStringLiteral("emp"));
        // 列按建表列序：id, name, salary。
        QCOMPARE(ts.columns.size(), 3);
        QCOMPARE(ts.columns.at(0).name, QStringLiteral("id"));
        QCOMPARE(ts.columns.at(1).name, QStringLiteral("name"));
        QCOMPARE(ts.columns.at(2).name, QStringLiteral("salary"));
        // 主键：id（单列，pkOrder=1）。
        QVERIFY(ts.columns.at(0).primaryKey);
        QCOMPARE(ts.columns.at(0).pkOrder, 1);
        QVERIFY(!ts.columns.at(1).primaryKey);
        // NOT NULL：name。
        QVERIFY(ts.columns.at(1).notNull);
        // 便捷主键列表。
        QCOMPARE(ts.primaryKeyColumns(), (QStringList{QStringLiteral("id")}));
        bridge.close();
    }

    // testCompositePkOrder —— 复合主键按 pkOrder 次序返回列名。
    void testCompositePkOrder() {
        execSeed({
            QStringLiteral("CREATE TABLE grade ("
                           "  student TEXT, course TEXT, score INTEGER,"
                           "  PRIMARY KEY (course, student))"),  // 主键次序：course 先、student 后
        });

        DataBridge bridge;
        ConnectionSpec spec;
        spec.sqlitePath = dbPath_;
        QString err;
        QVERIFY2(bridge.open(spec, &err), qPrintable(err));

        TableSchema ts;
        QVERIFY2(bridge.describeTable(QStringLiteral("grade"), &ts, &err), qPrintable(err));
        // primaryKeyColumns 应按 PRIMARY KEY(course, student) 的声明次序返回。
        QCOMPARE(ts.primaryKeyColumns(),
                 (QStringList{QStringLiteral("course"), QStringLiteral("student")}));
        bridge.close();
    }

    // testDescribeMissingTable —— 不存在的表返回 false 并置错误码。
    void testDescribeMissingTable() {
        execSeed({QStringLiteral("CREATE TABLE only (id INTEGER PRIMARY KEY)")});

        DataBridge bridge;
        ConnectionSpec spec;
        spec.sqlitePath = dbPath_;
        QString err;
        QVERIFY2(bridge.open(spec, &err), qPrintable(err));

        TableSchema ts;
        QVERIFY(!bridge.describeTable(QStringLiteral("nope"), &ts, &err));
        QVERIFY(!err.isEmpty());
        bridge.close();
    }

    // testNotOpen —— 未打开库时两接口都失败并置错误。
    void testNotOpen() {
        DataBridge bridge;  // 未 open
        QString err;
        const QStringList t = bridge.userTables(&err);
        QVERIFY(t.isEmpty());
        QVERIFY(!err.isEmpty());

        err.clear();
        TableSchema ts;
        QVERIFY(!bridge.describeTable(QStringLiteral("x"), &ts, &err));
        QVERIFY(!err.isEmpty());
    }
};

QTEST_MAIN(TstDataBridgeSchema)
#include "tst_databridge_schema.moc"
