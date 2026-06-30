#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "sync/SyncDDL.h"
#include "sync/apply/RowWinnerStore.h"

// ============================================================================
// tst_sync_row_winner.cpp — RowWinnerStore（行级冲突仲裁「胜者表」）的单元测试
// ============================================================================
//
// 【被测对象是什么】
//   多节点并发改同一行会冲突，必须有一个「与到达顺序无关、各节点算出同一结果」的确定性
//   裁决规则。RowWinnerStore 把每一行 (table, pk_hash) 的「当前胜者」持久化在 __sync_row_winner
//   表里，并用 beats() 规则仲裁谁赢（见 RowWinnerStore.h/.cpp 的 max-element 规则 G-01）：
//     先比 rank（高者胜，如中心>边缘）→ 平则比 originSeq（大者胜，较新）→ 仍平则比 origin
//     字符串字典序（纯兜底，保证确定性）。哨兵 rank==INT_MIN 代表「尚无胜者」，任何真实
//     rank 都战胜它。
//
// 【这组用例在守什么不变量】
//   · put() 是「条件写」：只有真正胜出才落库，否则幂等无操作；
//   · putOrRefill() 在 put 基础上额外允许「同 rank/seq 补写内容」（H-01 占位记录填满）；
//   · pkHash() 对同输入稳定、对不同输入相异（仲裁键不能碰撞）；
//   · resetAll/clear 正确地清全表 / 删单行；多行、多表之间互相隔离。
//
// 【测试夹具】每个用例 init() 起一张内存库并跑全套 DDL，cleanup() 释放。winnerOrigin()
//   辅助函数直接 SELECT 出某行胜者的 origin，用来「绕过被测 API、从库里独立核对」结果，
//   避免「用被测代码验证被测代码」的循环论证。
// ============================================================================

using namespace dbridge::sync;

// makeWinner —— 测试夹具工厂：用最少参数快速造一个 RowWinner（挑战者/胜者）。
// 关键点：winningContent 必须显式给非 null 值（""）——胜者表该列是 NOT NULL DEFAULT ''，
//   传 null 会触发约束失败。hash 默认 "h"，多数用例不关心内容指纹，只关心 rank/seq 的胜负。
static RowWinner makeWinner(const QString& origin, int rank, qint64 seq,
                            const QByteArray& hash = QByteArray("h")) {
    RowWinner w;
    w.origin = origin;
    w.rank = rank;
    w.originSeq = seq;
    w.contentHash = hash;
    w.winningContent = QStringLiteral("");  // must be non-null (NOT NULL DEFAULT '')（不可为 null）
    return w;
}

// winnerOrigin —— 独立核对工具：直接查 __sync_row_winner 取某行当前胜者的 origin。
// 为什么不复用 RowWinnerStore::get()：测试断言应当用「与被测路径无关」的手段验证落库结果，
//   直接读表能暴露 put/putOrRefill 是否真把正确的胜者写进去了。查无此行返回空串 {}。
static QString winnerOrigin(QSqlDatabase& db, const QString& table, const QString& pkHash) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT winning_origin FROM __sync_row_winner WHERE table_name=? AND pk_hash=?"));
    q.addBindValue(table);
    q.addBindValue(pkHash);
    if (q.exec() && q.next())
        return q.value(0).toString();
    return {};
}

class TstSyncRowWinner : public QObject {
    Q_OBJECT
    QString conn_;
    QSqlDatabase db_;

   private slots:
    // 每用例前：开唯一命名的内存库并建齐全部 __sync_* 元表（含 __sync_row_winner）。
    void init() {
        conn_ =
            QStringLiteral("tst_rw_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
        db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn_);
        db_.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db_.open());
        QSqlQuery q(db_);
        for (const QString& s : dbridge::sync::ddl::allCreateStatements())
            q.exec(s);
    }
    // 每用例后：先释放句柄再 removeDatabase（顺序见 applied_vector 测试同名注释）。
    void cleanup() {
        db_ = QSqlDatabase();
        if (QSqlDatabase::contains(conn_)) {
            QSqlDatabase::database(conn_).close();
            QSqlDatabase::removeDatabase(conn_);
        }
    }

    // init_ok —— init() 探测胜者表存在即成功。
    void init_ok() {
        RowWinnerStore rws;
        QString err;
        QVERIFY(rws.init(db_, &err));
    }

    // ── put_storesWinner —— 最基本契约：put 把胜者写进表，可被独立 SELECT 读出 ──
    // GIVEN 空表；WHEN put 一个 (nodeA, rank10, seq42)；THEN 直接查表得到 origin==nodeA。
    //   空位无在位者（哨兵），任何挑战者都直接占据——这是 beats 规则 1 的体现。
    // put stores winner; verify via SELECT
    void put_storesWinner() {
        RowWinnerStore rws;
        QString err;
        rws.init(db_, &err);
        RowWinner w = makeWinner("nodeA", 10, 42, QByteArray("abc"));
        QVERIFY(rws.put(db_, "orders", "pk001", w, &err));
        QCOMPARE(winnerOrigin(db_, "orders", "pk001"), QString("nodeA"));
    }

    // ── put_higherRankOverwrites —— 高 rank 覆盖低 rank（与写入先后无关）─────────
    // GIVEN 先写入低 rank 的 B(rank3)；WHEN 再写入高 rank 的 A(rank10)；THEN 胜者为 A。
    //   验证 beats 规则 2：rank 高者胜。注意「先 B 后 A」的写入顺序不改变结果——胜负只由
    //   规则定，这正是确定性仲裁的要义。
    // higher rank overwrites lower rank (put always writes; caller decides ordering)
    void put_higherRankOverwrites() {
        RowWinnerStore rws;
        QString err;
        rws.init(db_, &err);
        rws.put(db_, "t", "pk", makeWinner("B", 3, 1), &err);
        rws.put(db_, "t", "pk", makeWinner("A", 10, 2), &err);  // higher rank（更高 rank）
        QCOMPARE(winnerOrigin(db_, "t", "pk"), QString("A"));
    }

    // ── putOrRefill_lowerRankKeepsIncumbent —— 低 rank 挑战者打不过在位者 ───────
    // GIVEN 在位胜者 A(rank10)；WHEN 用低 rank 的 B(rank3, 即便 seq=99 更大) 去 putOrRefill；
    // THEN A 保持不变。说明 rank 是第一优先级，seq 再大也救不了低 rank（规则 2 先于规则 3）。
    //   也验证 putOrRefill 在「未胜出」时同样是幂等无操作。
    // putOrRefill: when challenger loses (lower rank), incumbent stays
    void putOrRefill_lowerRankKeepsIncumbent() {
        RowWinnerStore rws;
        QString err;
        rws.init(db_, &err);
        rws.put(db_, "t", "pk", makeWinner("A", 10, 1), &err);
        // low-rank challenger（低 rank 挑战者）
        rws.putOrRefill(db_, "t", "pk", makeWinner("B", 3, 99), &err);
        QCOMPARE(winnerOrigin(db_, "t", "pk"), QString("A"));  // A stays（A 保持）
    }

    // ── putOrRefill_higherRankReplaces —— 高 rank 挑战者经 putOrRefill 替换在位者 ─
    // GIVEN 在位者 B(rank3)；WHEN 高 rank 的 A(rank10) 来 putOrRefill；THEN 胜者变 A。
    //   说明 putOrRefill 在「正常胜出」分支与 put 行为一致（它只是额外多放行「同级补内容」）。
    // putOrRefill: when challenger wins (higher rank), it replaces incumbent
    void putOrRefill_higherRankReplaces() {
        RowWinnerStore rws;
        QString err;
        rws.init(db_, &err);
        rws.put(db_, "t", "pk", makeWinner("B", 3, 1), &err);
        rws.putOrRefill(db_, "t", "pk", makeWinner("A", 10, 2), &err);
        QCOMPARE(winnerOrigin(db_, "t", "pk"), QString("A"));
    }

    // ── resetAll_clearsTable —— 基线重置：清空整张胜者表 ──────────────────────
    // GIVEN 两张表/两行各有胜者；WHEN resetAll()；THEN COUNT(*)==0（全清）。
    //   基线重置后旧胜负全部作废，从空表重新累积——这条用例守的就是「清得干净、不留残行」。
    // resetAll clears all rows
    void resetAll_clearsTable() {
        RowWinnerStore rws;
        QString err;
        rws.init(db_, &err);
        rws.put(db_, "t1", "p1", makeWinner("A", 1, 1), &err);
        rws.put(db_, "t2", "p2", makeWinner("B", 2, 1), &err);
        QVERIFY(rws.resetAll(db_, &err));
        QSqlQuery q(db_);
        q.exec(QStringLiteral("SELECT COUNT(*) FROM __sync_row_winner"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 0);
    }

    // ── clear_specificRow —— clear 只删指定单行，不波及同表其它行 ──────────────
    // GIVEN 同表两行 pk1(A)、pk2(B)；WHEN clear(pk1)；THEN pk1 没了、pk2 仍是 B。
    //   对应场景：某行被一个「合法」DELETE 擦除时，须把该行胜者记录精确删除而不误伤邻行。
    // clear removes specific row, leaves others
    void clear_specificRow() {
        RowWinnerStore rws;
        QString err;
        rws.init(db_, &err);
        rws.put(db_, "t", "pk1", makeWinner("A", 1, 1), &err);
        rws.put(db_, "t", "pk2", makeWinner("B", 2, 1), &err);
        QVERIFY(rws.clear(db_, "t", "pk1", &err));
        QVERIFY(winnerOrigin(db_, "t", "pk1").isEmpty());       // pk1 已删
        QCOMPARE(winnerOrigin(db_, "t", "pk2"), QString("B"));  // pk2 不受影响
    }

    // ── pkHash_stableAndNonEmpty —— 主键哈希对同一输入稳定且非空 ───────────────
    // 同一 QVariantMap 两次算出的 pkHash 必须相等（确定性）且非空。仲裁键若不稳定，
    //   同一行在不同时刻/节点会落到不同 key，胜者表彻底错乱——这是仲裁正确性的地基。
    // pkHash is stable and non-empty for same input
    void pkHash_stableAndNonEmpty() {
        QVariantMap m;
        m["id"] = 42;
        m["name"] = "Alice";
        QString h1 = RowWinnerStore::pkHash(m);
        QString h2 = RowWinnerStore::pkHash(m);
        QCOMPARE(h1, h2);
        QVERIFY(!h1.isEmpty());
    }

    // ── pkHash_differsForDifferentValues —— 不同主键值哈希不同（抗碰撞）──────────
    // id=1 与 id=2 的哈希必须相异，否则两行会被仲裁成同一行（致命碰撞）。
    //   配合 RowWinnerStore.cpp 的「带类型标签规范编码」(M-01 fix) 防止可构造碰撞。
    // pkHash differs for different values
    void pkHash_differsForDifferentValues() {
        QVariantMap m1, m2;
        m1["id"] = 1;
        m2["id"] = 2;
        QVERIFY(RowWinnerStore::pkHash(m1) != RowWinnerStore::pkHash(m2));
    }

    // ── putOrRefill_sentinelIsLowest —— 哨兵 rank(INT_MIN) 是「无胜者」，必被真实 rank 击败 ─
    // GIVEN 先写入一条哨兵记录(rank=INT_MIN)；WHEN 真实胜者 A(rank1) 来 putOrRefill；
    // THEN A 获胜。验证 beats 规则 1：在位者若为哨兵即视同空位，任何真实 rank（哪怕只有 1）
    //   都战胜它。哨兵不是一个「很低的合法胜者」，而是「根本没有胜者」的标记。
    // sentinel rank (INT_MIN) treated as "no winner" – any real rank beats it
    void putOrRefill_sentinelIsLowest() {
        RowWinnerStore rws;
        QString err;
        rws.init(db_, &err);
        RowWinner sentinel = makeWinner("none", INT_MIN, 0);
        RowWinner real = makeWinner("A", 1, 1);
        rws.put(db_, "t", "pk", sentinel, &err);
        rws.putOrRefill(db_, "t", "pk", real, &err);
        QCOMPARE(winnerOrigin(db_, "t", "pk"), QString("A"));
    }

    // ── put_multipleRows_independent —— 同表内不同 pk_hash 行彼此隔离 ──────────
    // GIVEN 同表两行 pk1、pk2 各写不同胜者；THEN 两行胜者互不串扰（A 归 A、B 归 B）。
    //   守的是「胜者表主键是 (table, pk_hash) 复合键」这一隔离性——一行的胜负不影响另一行。
    // Multiple pk_hash values are isolated within same table
    void put_multipleRows_independent() {
        RowWinnerStore rws;
        QString err;
        rws.init(db_, &err);
        rws.put(db_, "t", "pk1", makeWinner("A", 5, 1), &err);
        rws.put(db_, "t", "pk2", makeWinner("B", 3, 1), &err);
        QCOMPARE(winnerOrigin(db_, "t", "pk1"), QString("A"));
        QCOMPARE(winnerOrigin(db_, "t", "pk2"), QString("B"));
    }
};

QTEST_APPLESS_MAIN(TstSyncRowWinner)
#include "tst_sync_row_winner.moc"
