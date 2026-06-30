#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "profile/AutoProfileBuilder.h"
#include "schema/SchemaCatalog.h"
#include "schema/SchemaIntrospector.h"

// ============================================================================
// tst_auto_profile_builder.cpp — AutoProfileBuilder（自动 Profile 生成器）的单元测试
// ============================================================================
//
// 【被测对象是什么】
//   AutoProfileBuilder 根据一张表的真实结构（TableInfo），自动「推断」出一份导入
//   Profile（ProfileSpec）——即「Excel 该怎样写进这张表」的配置：写哪些列、用哪些
//   列作 UPSERT 的冲突键（conflict key）、自增主键要不要跳过，等等。它把「人工编写
//   Profile」这件繁琐易错的事自动化，是 dbridge「零配置导入」体验的关键一环。
//
// 【这条测试链条上的三个协作者（每个用例都按此顺序走一遍）】
//   1) 在内存库 (:memory:) 里 CREATE TABLE 出一张结构特定的表（构造被测场景）。
//   2) SchemaIntrospector::load() 把该表自省成 SchemaCatalog（真实结构的内部模型）。
//   3) AutoProfileBuilder::build(tableInfo, &spec, &err) 由结构推断出 ProfileSpec，
//      再对 spec 的关键字段做断言。
//   用内存库而非磁盘文件：每个用例自带 schema、互不污染、跑完即弃，最快且最干净。
//
// 【ProfileSpec 里被反复检视的字段】
//   · routes        —— 路由列表：一份 Profile 可写多张表，单表场景恒为 1 条 RouteSpec。
//   · route.conflict.columns —— UPSERT 的冲突键列（决定「按什么判定是同一行」）。
//   · route.columns —— 实际可写列（写库时会跳过自增主键、生成列等）。
//   · executable / issues —— 这份 Profile 能否直接执行；不能则 issues 列出原因（草稿态）。
//
// 【Qt Test 夹具方法（fixtures）】
//   init() / cleanup() 在「每个」测试用例前后各跑一次（区别于一次性的 initTestCase/
//   cleanupTestCase），保证每个用例拿到独立、干净的数据库连接。
//
// 注意：测试文件，只增注释，不改任何 SQL、断言或被测调用。
// ============================================================================

using namespace dbridge::detail;

class TstAutoProfileBuilder : public QObject {
    Q_OBJECT

    QString connName_;  // 本用例使用的数据库连接名（每个用例在 init() 里重新生成唯一名）

    // 夹具助手：执行一条建表/灌数 SQL；失败即让用例失败并打印底层错误文本。
    // QVERIFY2 比 QVERIFY 多一个失败说明参数（这里取 SQL 驱动的 lastError），便于定位。
    void execSql(QSqlDatabase& db, const QString& sql) {
        QSqlQuery q(db);
        QVERIFY2(q.exec(sql), q.lastError().text().toUtf8());
    }

    // 夹具助手：把已建好的库自省成 SchemaCatalog（封装 SchemaIntrospector::load 调用）。
    // 自省失败用 Q_ASSERT_X 直接中断——这是「测试前置条件」，若连结构都读不出来，
    //   后续断言无从谈起，应当尽早炸出。
    SchemaCatalog loadCatalog(QSqlDatabase& db) {
        SchemaIntrospector si;
        SchemaCatalog catalog;
        QString err;
        bool ok = si.load(db, &catalog, &err);
        Q_ASSERT_X(ok, "loadCatalog", err.toUtf8());
        return catalog;
    }

   private slots:
    // 每个用例「之前」：生成一个全局唯一的连接名（UUID 后缀），避免并行/重复跑时撞名。
    void init() {
        connName_ = QStringLiteral("tst_auto_") + QUuid::createUuid().toString();
    }

    // 每个用例「之后」：若该连接还在，先 close 再从全局注册表移除，杜绝连接泄漏/句柄残留。
    void cleanup() {
        if (QSqlDatabase::contains(connName_)) {
            QSqlDatabase::database(connName_).close();
            QSqlDatabase::removeDatabase(connName_);
        }
    }

    // 用例①：自增主键场景 —— 自增列既不能当冲突键、也不能当可写列。
    // GIVEN items 表：id INTEGER PRIMARY KEY AUTOINCREMENT + name TEXT NOT NULL UNIQUE。
    // WHEN  自动推断 Profile。
    // THEN  ① 单表 → 恰好 1 条 route；
    //       ② 冲突键应落在 UNIQUE 业务列 name 上（而非自增 id）——因为 id 由库自动分配，
    //          导入端无从提供，拿它做 UPSERT 匹配无意义；name 的 UNIQUE 才是稳定业务标识；
    //       ③ 可写列里绝不含 id —— 自增主键须交给数据库生成，导入不应写它。
    void testAutoincrementPrimaryKey() {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName_);
        db.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db.open());
        execSql(db, R"(CREATE TABLE items (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL UNIQUE
        ))");

        SchemaCatalog catalog = loadCatalog(db);
        AutoProfileBuilder builder;
        ProfileSpec spec;
        QString err;
        QVERIFY2(builder.build(*catalog.table("items"), &spec, &err), err.toUtf8());

        // autoincrement column should not be in conflict or writable cols
        // 译：自增列既不应出现在冲突键里、也不应出现在可写列里。
        QCOMPARE(spec.routes.size(), 1);  // 单表 → 恰好一条路由
        const RouteSpec& route = spec.routes[0];
        QVERIFY(route.conflict.columns.contains(
            QStringLiteral("name")));  // 冲突键 = UNIQUE 业务列 name
        for (const auto& col : route.columns) {
            QVERIFY(col.dbColumn !=
                    QStringLiteral("id"));  // skip autoincrement（可写列不含自增 id）
        }
    }

    // 用例②：复合主键场景 —— 冲突键应包含主键的「全部」组成列。
    // GIVEN pairs 表：PRIMARY KEY (a, b) 两列复合主键 + 普通列 val。
    // WHEN  自动推断 Profile。
    // THEN  冲突键须同时含 a 与 b —— 复合主键唯一标识一行需要「所有主键列共同参与」匹配，
    //       少任何一列都会把不同的行误判成同一行。这钉住了「冲突键 = 完整复合主键」的契约。
    void testCompositePrimaryKey() {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName_);
        db.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db.open());
        execSql(db, R"(CREATE TABLE pairs (
            a TEXT NOT NULL,
            b TEXT NOT NULL,
            val TEXT,
            PRIMARY KEY (a, b)
        ))");

        SchemaCatalog catalog = loadCatalog(db);
        AutoProfileBuilder builder;
        ProfileSpec spec;
        QString err;
        QVERIFY2(builder.build(*catalog.table("pairs"), &spec, &err), err.toUtf8());

        const RouteSpec& route = spec.routes[0];
        QVERIFY(route.conflict.columns.contains(QStringLiteral("a")));  // 复合主键第 1 列
        QVERIFY(
            route.conflict.columns.contains(QStringLiteral("b")));  // 复合主键第 2 列（缺一不可）
    }

    // 用例③：无任何唯一键场景 —— 推断成「草稿 Profile」而非直接失败（M-03 修复后的契约）。
    // GIVEN nopk 表：col1/col2 皆普通列，既无主键也无 UNIQUE。
    // WHEN  自动推断 Profile。
    // THEN  ① build() 仍返回 true —— 但产物是「草稿」，不是「可执行」Profile；
    //       ② spec.executable == false —— 没有冲突键就无法安全 UPSERT，故标记为不可执行；
    //       ③ issues 非空且首条含 E_PROFILE_NO_CONFLICT_KEY —— 用机器可读码点明「缺冲突键」，
    //          供 UI 提示用户手动指定键。这验证了「优雅降级为草稿 + 给出可诊断原因」的设计，
    //          区别于早期「无键即硬失败」的旧行为。
    void testNoUniqueKey() {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName_);
        db.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db.open());
        execSql(db, R"(CREATE TABLE nopk (
            col1 TEXT,
            col2 INTEGER
        ))");

        SchemaCatalog catalog = loadCatalog(db);
        AutoProfileBuilder builder;
        ProfileSpec spec;
        QString err;
        // M-03 fix: build() now returns true for draft profiles (executable=false + issues).
        // 译：【M-03 修复】对「草稿 Profile」build() 现在返回 true（但 executable=false 且带
        // issues）。
        QVERIFY(builder.build(*catalog.table("nopk"), &spec, &err));  // 草稿仍算「构建成功」
        QVERIFY(!spec.executable);                                    // 但不可直接执行
        QVERIFY(!spec.issues.isEmpty());                              // 必须给出阻碍原因
        QVERIFY(spec.issues.first().contains("E_PROFILE_NO_CONFLICT_KEY"));  // 原因 = 缺冲突键
    }

    // 用例④：JSON 序列化 —— Profile 能被导出成可持久化/可展示的 JSON 文本。
    // GIVEN customer 表：customer_no TEXT PRIMARY KEY + name。
    // WHEN  推断 Profile 后调用 builder.toJson(spec)。
    // THEN  产出的 JSON 非空，且包含若干「结构性关键字段」——
    //       "profileName"（顶层有名字字段）、"auto_customer"（自动命名约定 = auto_<表名>）、
    //       "singleTable"（单表模式标记）。这是「冒烟级」断言：不逐字段比对全文，只确认
    //       序列化产物结构成形、命名约定生效，足以挡住「序列化整体坏掉」的回归。
    void testJsonOutput() {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName_);
        db.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db.open());
        execSql(db, R"(CREATE TABLE customer (
            customer_no TEXT PRIMARY KEY,
            name TEXT
        ))");

        SchemaCatalog catalog = loadCatalog(db);
        AutoProfileBuilder builder;
        ProfileSpec spec;
        QString err;
        QVERIFY(builder.build(*catalog.table("customer"), &spec, &err));

        QString json = builder.toJson(spec);
        QVERIFY(!json.isEmpty());                                 // 序列化产物非空
        QVERIFY(json.contains(QStringLiteral("profileName")));    // 含「Profile 名」字段
        QVERIFY(json.contains(QStringLiteral("auto_customer")));  // 命名约定 auto_<表名> 生效
        QVERIFY(json.contains(QStringLiteral("singleTable")));    // 标注为单表模式
    }
};

QTEST_MAIN(TstAutoProfileBuilder)
#include "tst_auto_profile_builder.moc"
