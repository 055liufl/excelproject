// ============================================================================
// tst_schema_introspector.cpp — SchemaIntrospector(schema 自省器)单元测试
// ============================================================================
//
// 【这个测试文件在验证什么】
//   SchemaIntrospector 的职责：连上一个 SQLite 库,把它的「物理结构」(有哪些表、
//   每张表有哪些列、哪些列是主键/自增/非空、有哪些(唯一)索引、有哪些外键)读出来,
//   填进一份内存目录 SchemaCatalog。下游的导入/导出/同步全都依赖这份「真实结构」
//   来做列映射、冲突键定位、外键闭包等。本文件就是用一组「已知结构的小表」去喂自省器,
//   再断言自省结果与预期结构完全吻合——即「自省器读得准不准」的回归保护网。
//
// 【测试套路(每个用例都一样,先理解一遍后面就不赘述)】
//   GIVEN: 用 :memory: 内存库现建一张结构已知的表(CREATE TABLE …);
//   WHEN : 调 SchemaIntrospector::load(db, &catalog) 自省;
//   THEN : 从 catalog 里取出该表/列/索引/外键,断言其属性与建表语句一致。
//   用 :memory: 库的好处:零磁盘 IO、用例间天然隔离、cleanup 即销毁,跑得快又干净。
//
// 【QtTest 框架要点】
//   · 继承 QObject + Q_OBJECT,所有「private slots」里的无参方法都是一个测试用例;
//   · init()/cleanup() 是每个用例「前后」自动调用的夹具钩子(注意区别于只跑一次的
//     initTestCase/cleanupTestCase);本文件用它们为每个用例配唯一连接名并善后;
//   · QVERIFY/QVERIFY2/QCOMPARE 是断言宏,失败即终止当前用例并打印信息。
// ============================================================================

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "schema/SchemaCatalog.h"
#include "schema/SchemaIntrospector.h"

using namespace dbridge::detail;

class TstSchemaIntrospector : public QObject {
    Q_OBJECT

    // 本测试实例当前用例所用的 Qt 连接名;每个用例在 init() 里赋新值,保证彼此隔离。
    QString connName_;

    // execSql —— 测试夹具小工具:执行一条 SQL,失败即让用例 FAIL 并打印数据库错误文本。
    //   把「建表/插数据」这类前置准备语句的成功性断言收拢到一处,使各用例正文更聚焦。
    void execSql(QSqlDatabase& db, const QString& sql) {
        QSqlQuery q(db);
        QVERIFY2(q.exec(sql), q.lastError().text().toUtf8());
    }

   private slots:
    // init —— 每个测试用例「开始前」自动调用:生成一个全局唯一的连接名。
    //   为何要唯一(带 UUID):Qt 的 QSqlDatabase 以连接名为全局键;若多个用例复用同名连接,
    //   会互相覆盖/串号。每个用例独占一个名字,跑完在 cleanup 里注销,互不干扰。
    void init() {
        connName_ = QStringLiteral("tst_schema_") + QUuid::createUuid().toString();
    }

    // cleanup —— 每个测试用例「结束后」自动调用:关闭并从全局连接表注销本用例的连接。
    //   先判 contains 再操作,使「用例中途失败、连接可能未建」时也能安全善后(幂等)。
    void cleanup() {
        if (QSqlDatabase::contains(connName_)) {
            QSqlDatabase::database(connName_).close();
            QSqlDatabase::removeDatabase(connName_);
        }
    }

    // testSimpleTable —— 验证最基础的列属性:列数、主键、自增、非空都被正确自省。
    //   GIVEN: 一张 customer 表,id 为「INTEGER PRIMARY KEY AUTOINCREMENT」、name 为「NOT NULL」;
    //   WHEN : 自省该库;
    //   THEN : catalog 里能找到 customer 表,共 3 列;id 列被识别为主键且自增;name 列被识别为非空。
    //   业务含义:这三类标志(主键/自增/非空)是导入时定位 UPSERT 冲突键、判断列可否留空的基础。
    void testSimpleTable() {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName_);
        db.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db.open());
        execSql(db, R"(CREATE TABLE customer (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            phone TEXT
        ))");

        // 自省:把 db 的结构读进 catalog;load 返回 false 即自省失败,打印 err 文本。
        SchemaIntrospector si;
        SchemaCatalog catalog;
        QString err;
        QVERIFY2(si.load(db, &catalog, &err), err.toUtf8());

        QVERIFY(catalog.hasTable(QStringLiteral("customer")));  // 表必须被发现
        const TableInfo* t = catalog.table(QStringLiteral("customer"));
        QVERIFY(t);                      // table() 命中返回非空指针
        QCOMPARE(t->columns.size(), 3);  // 恰好 3 列(id/name/phone)

        const ColumnInfo* idCol = t->column(QStringLiteral("id"));
        QVERIFY(idCol);
        QVERIFY(idCol->primaryKey);     // id 是主键
        QVERIFY(idCol->autoIncrement);  // 且带 AUTOINCREMENT(自增)

        const ColumnInfo* nameCol = t->column(QStringLiteral("name"));
        QVERIFY(nameCol);
        QVERIFY(nameCol->notNull);  // name 被识别为 NOT NULL
    }

    // testCompositePrimaryKey —— 验证「复合主键」:多列共同构成主键时,每一列都应被标为 primaryKey。
    //   GIVEN: order_items 表以 (order_no, line_no) 两列做 PRIMARY KEY;
    //   WHEN : 自省;
    //   THEN : order_no 与 line_no 两列的 primaryKey 都为 true。
    //   业务含义:同步/导入需识别全部主键列才能正确定位「是哪一行」(明细表常用复合主键)。
    void testCompositePrimaryKey() {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName_);
        db.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db.open());
        execSql(db, R"(CREATE TABLE order_items (
            order_no TEXT NOT NULL,
            line_no INTEGER NOT NULL,
            sku TEXT,
            PRIMARY KEY (order_no, line_no)
        ))");

        SchemaIntrospector si;
        SchemaCatalog catalog;
        QString err;
        QVERIFY2(si.load(db, &catalog, &err), err.toUtf8());

        const TableInfo* t = catalog.table(QStringLiteral("order_items"));
        QVERIFY(t);

        // Both columns should be primary key
        // 【译】复合主键的两列都应被标为主键(缺一不可,否则后续无法唯一定位行)。
        const ColumnInfo* oCol = t->column(QStringLiteral("order_no"));
        QVERIFY(oCol && oCol->primaryKey);
        const ColumnInfo* lCol = t->column(QStringLiteral("line_no"));
        QVERIFY(lCol && lCol->primaryKey);
    }

    // testUniqueIndex —— 验证「唯一索引」能被自省并归入 TableInfo::indexes。
    //   GIVEN: products.sku 声明为「NOT NULL UNIQUE」,SQLite 会为之隐式建一个唯一索引;
    //   WHEN : 自省;
    //   THEN : 该表的 indexes 中存在一个 unique==true 且覆盖列 "sku" 的索引。
    //   业务含义:唯一约束可作为 UPSERT 的冲突键候选,自省器必须把它们一并收集进目录。
    void testUniqueIndex() {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName_);
        db.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db.open());
        execSql(db, R"(CREATE TABLE products (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            sku TEXT NOT NULL UNIQUE
        ))");

        SchemaIntrospector si;
        SchemaCatalog catalog;
        QString err;
        QVERIFY2(si.load(db, &catalog, &err), err.toUtf8());

        const TableInfo* t = catalog.table(QStringLiteral("products"));
        QVERIFY(t);

        // 遍历该表所有索引,找「既是唯一索引、又覆盖 sku 列」的那一条;找到即满足断言。
        bool foundUniqueIdx = false;
        for (const auto& idx : t->indexes) {
            if (idx.unique && idx.columns.contains(QStringLiteral("sku"))) {
                foundUniqueIdx = true;
            }
        }
        QVERIFY(foundUniqueIdx);
    }

    // testForeignKey —— 验证「外键」能被自省并填进 TableInfo::foreignKeys。
    //   GIVEN: items.order_no 用「REFERENCES orders(order_no)」声明了指向 orders 的外键;
    //   WHEN : 自省;
    //   THEN : items 表外键非空,且第一条外键的 refTable=="orders"、fromColumn=="order_no"。
    //   业务含义:FK 图是「选择性推送补外键闭包/拓扑排序」的基础数据,自省必须准确捕获
    //            (fromColumn=本表哪列、refTable=指向哪张父表)。
    void testForeignKey() {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName_);
        db.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db.open());
        execSql(db, R"(CREATE TABLE orders (order_no TEXT PRIMARY KEY))");
        execSql(db, R"(CREATE TABLE items (
            id INTEGER PRIMARY KEY,
            order_no TEXT REFERENCES orders(order_no)
        ))");

        SchemaIntrospector si;
        SchemaCatalog catalog;
        QString err;
        QVERIFY2(si.load(db, &catalog, &err), err.toUtf8());

        const TableInfo* t = catalog.table(QStringLiteral("items"));
        QVERIFY(t);
        QVERIFY(!t->foreignKeys.isEmpty());  // 必须至少识别出一条外键
        QCOMPARE(t->foreignKeys[0].refTable, QStringLiteral("orders"));      // 指向的父表
        QCOMPARE(t->foreignKeys[0].fromColumn, QStringLiteral("order_no"));  // 本表的外键列
    }
};

// QTEST_MAIN —— 生成 main():实例化测试类、跑遍所有 private slots 用例、汇总通过/失败。
QTEST_MAIN(TstSchemaIntrospector)
// 引入 moc 为本 TU 生成的元对象代码(Q_OBJECT 信号槽/反射所需);文件名须与本 .cpp 同名。
#include "tst_schema_introspector.moc"
