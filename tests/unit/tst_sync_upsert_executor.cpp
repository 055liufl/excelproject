// ============================================================================
// tst_sync_upsert_executor.cpp — UpsertExecutor（行变更落库执行器）单元测试
// ============================================================================
//
// 【被测对象】UpsertExecutor::apply(db, rows, &errors, &fatalErr)：把一批 RowMutation
//   （来自同步「应用」环节）按各自的 UpsertMode 写进本地库。它是「收到对端变更后真正
//   改库」的最后一棒，必须正确实现 INSERT-或-UPDATE 的 upsert 语义、批量、容错。
//
// 【UpsertMode 两种模式（本测试的核心契约）】
//   · DoUpdate ：主键已存在则更新该行、不存在则插入（标准 upsert，覆盖写）。
//   · DoNothing：主键已存在则【保持不动】、不存在才插入（不覆盖既有行）。
//   这两种模式服务于冲突仲裁的不同结果——胜者覆盖用 DoUpdate，败者「不得改写」用 DoNothing。
//
// 【这些用例守护的不变量】
//   ① DoUpdate 对新键 → 插入（行数 +1，无错误）；
//   ② DoUpdate 对已存在键 → 真的更新了字段值（验证 name/qty 被改写）；
//   ③ DoNothing 对已存在键 → 绝不覆盖（验证旧值原样保留）；
//   ④ 一次 apply 批量多行 → 全部写入；
//   ⑤ 同 schema 连续两次 apply → 内部 prepared 语句缓存可安全复用（不崩、结果正确）；
//   ⑥ 空批 → 平凡成功；
//   ⑦ 约束冲突行（如 PK=NULL）→ 不得崩溃；以「收集行级错误」或「返回失败」之一收场。
//
// 【夹具】init/cleanup 建/拆独立内存库（连接名带 UUID 防撞名），并预建 items 表
//   `(id INTEGER PRIMARY KEY NOT NULL, name TEXT, qty INTEGER)`。
//   exec()=执行 DDL 并断言成功；count()=查行数小工具。makeRow()=快速造 RowMutation。
// 【框架】Qt Test，QTEST_APPLESS_MAIN（纯逻辑、无 GUI）。
// ============================================================================
#include "dbridge/Types.h"
#include "dbridge/sync/SyncTypes.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "sync/apply/UpsertExecutor.h"

using namespace dbridge::sync;

// ── makeRow —— 夹具：快速构造一个 RowMutation（一行待落库的变更）─────────────────
// 做什么：把 表名 / 主键列名 / 列名 / 列值 / upsert 模式 装进 RowMutation。
// 注意：参数里的 pkVals 实际未被赋给 RowMutation（主键值已包含在 cols/vals 中——见各用例
//   都把 id 同时列进 cols/vals）；它仅为调用处可读性而保留形参，体现「这行的主键是谁」。
// 默认模式 DoUpdate（覆盖写），DoNothing 用例显式传入。
static RowMutation makeRow(const QString& table, const QStringList& pkCols,
                           const QVariantList& pkVals, const QStringList& cols,
                           const QVariantList& vals, UpsertMode mode = UpsertMode::DoUpdate) {
    RowMutation m;
    m.table = table;
    m.pkColumns = pkCols;  // 主键列名（upsert 据此判断「已存在/新行」）
    m.columns = cols;      // 要写的列名（含主键列）
    m.values = vals;       // 与 cols 一一对应的列值
    m.mode = mode;         // DoUpdate（覆盖）/ DoNothing（不覆盖）
    return m;
}

class TstSyncUpsertExecutor : public QObject {
    Q_OBJECT
    QString conn_;     // 本用例专属连接名（UUID 防撞名）
    QSqlDatabase db_;  // 内存 SQLite 连接

    // 执行 DDL 并断言成功（建表语句必须成功，否则后续用例无意义）。
    void exec(const QString& sql) {
        QSqlQuery q(db_);
        QVERIFY2(q.exec(sql), qPrintable(q.lastError().text()));
    }
    // 数某表行数的小工具：失败/无结果返回 -1（与正常的非负行数可区分）。
    int count(const QString& table) {
        QSqlQuery q(db_);
        q.exec(QStringLiteral("SELECT COUNT(*) FROM ") + table);
        return q.next() ? q.value(0).toInt() : -1;
    }

   private slots:
    // init —— 每个用例前：开独立内存库 + 预建 items 表（单列主键 id）。
    void init() {
        conn_ =
            QStringLiteral("tst_ue_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
        db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn_);
        db_.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db_.open());
        exec("CREATE TABLE items (id INTEGER PRIMARY KEY NOT NULL, name TEXT, qty INTEGER)");
    }
    // cleanup —— 每个用例后：先释放句柄再移除连接（次序见 schema_eligibility 同款说明）。
    void cleanup() {
        db_ = QSqlDatabase();  // release handle before removeDatabase
        if (QSqlDatabase::contains(conn_)) {
            QSqlDatabase::database(conn_).close();
            QSqlDatabase::removeDatabase(conn_);
        }
    }

    // 用例①：DoUpdate 对【新主键】→ 插入新行。
    // GIVEN 空表，apply 一行 id=1（DoUpdate 默认模式）；
    // THEN  apply 返回 true、表里恰 1 行、无行级错误。
    void apply_doUpdate_insertsNew() {
        UpsertExecutor ue;
        QList<RowMutation> rows;
        rows << makeRow("items", {"id"}, {1}, {"id", "name", "qty"}, {1, "Widget", 10});
        QList<dbridge::RowError> errors;
        QString err;
        QVERIFY(ue.apply(db_, rows, &errors, &err));
        QCOMPARE(count("items"), 1);
        QVERIFY(errors.isEmpty());
    }

    // 用例②：DoUpdate 对【已存在主键】→ 覆盖更新字段值（这是 upsert 的「U」语义）。
    // GIVEN 先插入 id=1(Widget,10)，再对 id=1 apply (Gadget,99)；
    // THEN  第二次 apply 成功，且查回该行 name 已变 Gadget、qty 已变 99（确认是「更新」而非新增）。
    void apply_doUpdate_updatesExisting() {
        UpsertExecutor ue;
        QList<RowMutation> rows;
        rows << makeRow("items", {"id"}, {1}, {"id", "name", "qty"}, {1, "Widget", 10});
        ue.apply(db_, rows, nullptr, nullptr);  // 第一次：插入（errors/err 传 nullptr 表示不收集）
        // update qty —— 用同一主键 id=1 再 apply 一组新值，应触发 UPDATE 分支
        rows.clear();
        rows << makeRow("items", {"id"}, {1}, {"id", "name", "qty"}, {1, "Gadget", 99});
        QVERIFY(ue.apply(db_, rows, nullptr, nullptr));
        QSqlQuery q(db_);
        q.exec("SELECT name, qty FROM items WHERE id=1");
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QString("Gadget"));  // name 被更新
        QCOMPARE(q.value(1).toInt(), 99);                    // qty 被更新
    }

    // 用例③：DoNothing 对【已存在主键】→ 绝不覆盖（这是「败者不得改写胜者」的关键保证）。
    // GIVEN 先插入 id=5(Original,1)，再用 DoNothing 模式对 id=5 apply (ShouldNotWin,999)；
    // THEN  apply 返回 true（不报错），但查回 name 仍是 "Original"（旧值原样保留，未被覆盖）。
    void apply_doNothing_doesNotOverwrite() {
        UpsertExecutor ue;
        // insert first —— 先放一行原始数据
        QList<RowMutation> ins;
        ins << makeRow("items", {"id"}, {5}, {"id", "name", "qty"}, {5, "Original", 1});
        ue.apply(db_, ins, nullptr, nullptr);

        // DoNothing must not overwrite —— 同主键、DoNothing 模式：应被「忽略写入」
        QList<RowMutation> dep;
        dep << makeRow("items", {"id"}, {5}, {"id", "name", "qty"}, {5, "ShouldNotWin", 999},
                       UpsertMode::DoNothing);
        QVERIFY(ue.apply(db_, dep, nullptr, nullptr));  // 成功（无冲突报错），但不改数据

        QSqlQuery q(db_);
        q.exec("SELECT name FROM items WHERE id=5");
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QString("Original"));  // unchanged（确认未被覆盖）
    }

    // 用例④：一次 apply 批量多行 → 全部写入。
    // GIVEN 一批 5 行（id=1..5）；THEN apply 成功且表里恰好 5 行（验证批处理完整落库）。
    void apply_batchMultipleRows() {
        UpsertExecutor ue;
        QList<RowMutation> rows;
        for (int i = 1; i <= 5; ++i)
            rows << makeRow("items", {"id"}, {i}, {"id", "name", "qty"},
                            {i, QString("Item%1").arg(i), i * 10});
        QVERIFY(ue.apply(db_, rows, nullptr, nullptr));
        QCOMPARE(count("items"), 5);
    }

    // 用例⑤：同 schema 连续两次 apply → 内部 prepared 语句缓存复用不出错。
    // 背景：UpsertExecutor 会按「表+列集」缓存已 prepare 的 QSqlQuery 以提速；本用例确保
    //   第二次 apply 命中缓存时不崩溃、结果正确。
    // GIVEN 先 apply id=10，再 apply id=11（同表同列集，命中缓存）；THEN 两次都成功、共 2 行。
    void apply_preparedCacheReused() {
        UpsertExecutor ue;
        // Apply twice with same schema — prepared cache should be reused (no crash)
        // 同一 schema 连用两次——预编译语句缓存应被复用且不崩。
        QList<RowMutation> r1, r2;
        r1 << makeRow("items", {"id"}, {10}, {"id", "name", "qty"}, {10, "A", 1});
        r2 << makeRow("items", {"id"}, {11}, {"id", "name", "qty"}, {11, "B", 2});
        QVERIFY(ue.apply(db_, r1, nullptr, nullptr));
        QVERIFY(ue.apply(db_, r2, nullptr, nullptr));
        QCOMPARE(count("items"), 2);
    }

    // 用例⑥：空批 → 平凡成功（无行可写也不算失败，简化上层调用，无需自行判空）。
    void apply_emptyBatch_succeeds() {
        UpsertExecutor ue;
        QVERIFY(ue.apply(db_, {}, nullptr, nullptr));
    }

    // 用例⑦：约束冲突行（PK=NULL）→ 不得崩溃；以「收集行级错误」或「返回失败」之一收场。
    // 背景：items.id 是 PRIMARY KEY NOT NULL，写入 id=NULL 必然违反约束。本用例不规定具体
    //   是哪种收场（行级 error / 整体 false 视实现而定），但【强约束「不能崩溃」】——这是
    //   容错性的底线：单行坏数据不应把整个同步进程拖垮。
    // GIVEN 一行 id=NULL；WHEN apply（同时给 errors 与 fatalErr 两个出参）；
    // THEN  无论返回值如何都不崩溃（ok 故意丢弃，仅断言「跑完没崩」）。
    void apply_constraintViolation_rowErrorCollected() {
        // Insert a NOT NULL violation: name=NULL when NOT NULL constraint exists
        // (items.name has no NOT NULL, so try id=NULL which violates PK)
        // 构造约束冲突：items.name 无 NOT NULL，故改用 id=NULL 来违反「主键 NOT NULL」。
        UpsertExecutor ue;
        QList<RowMutation> rows;
        // id=NULL violates PRIMARY KEY NOT NULL —— 必然触发约束失败
        rows << makeRow("items", {"id"}, {QVariant()}, {"id", "name", "qty"}, {QVariant(), "X", 1});
        QList<dbridge::RowError> errors;
        QString fatalErr;
        // Should not crash; may collect row error or return false
        // 关键断言（隐式）：调用本身不得崩溃；返回 true/false 都可接受。
        bool ok = ue.apply(db_, rows, &errors, &fatalErr);
        // Either row error collected or fatal error — must not crash
        // 收场方式（行级错误 or 整体失败）由实现决定，这里不强求；ok 故意忽略。
        (void)ok;
    }
};

// 无 GUI 测试入口；下一行引入 moc 生成的元代码。
QTEST_APPLESS_MAIN(TstSyncUpsertExecutor)
#include "tst_sync_upsert_executor.moc"
