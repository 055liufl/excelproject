// ============================================================================
// tst_sync_table_state.cpp — TableStateStore（每表「表态」增量校验和）的单元测试
// ============================================================================
//
// 【被测对象是什么】
//   TableStateStore（见 src/sync/schema/TableStateStore.h）为每张同步表维护一份「表态」
//   快照：{schema_fingerprint, content_checksum, row_count}，键为 (table, stream_epoch)，
//   落盘在 __sync_table_state。上层用它做「差异快速判等」：两端三元组相同 ≈ 数据一致，
//   无需逐行 diff。
//
// 【最关键的设计——content_checksum 是「模加和」】
//   校验和 = 所有行哈希（取前 8 字节当 quint64）按模 2^64 累加。这赋予它两条代数性质，
//   本测试套件正是围绕它们设计的：
//     · 可增量更新：插入 += H(after)、删除 −= H(before)、更新 += H(after)−H(before)；
//     · 顺序无关 + 可抵消：加法满足交换律，故「以任意顺序应用同一批变更」结果相同，
//       且「插入再删除同一行」校验和精确回到原值。
//
// 【这组测试在守护哪些不变量】
//   1) rowHash 的确定性（同输入同输出）与区分性（不同输入不同输出）；
//   2) 行数随 INSERT/DELETE 正确增减；
//   3) 插入+删除同行 → 校验和归零（模加可抵消）；
//   4) G-06：表「身份」只由 {checksum, fp, row_count} 决定，与 high_water 序号无关；
//   5) 顺序无关性：不同应用顺序得到相同校验和；
//   6) 查不存在的表态 → found=false。
//
// 【测试夹具与隔离】每用例 init() 建全新内存库并执行全部建表 DDL（allCreateStatements），
//   cleanup() 释放连接；连接名用 UUID 保证用例间隔离。框架为 Qt Test。
// ============================================================================

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "sync/SyncDDL.h"
#include "sync/schema/TableStateStore.h"

using namespace dbridge::sync;

// ── 测试夹具辅助：构造各类 TableMutation（记账用的行级变更描述）──────────────────
// TableMutation 只携带「做模加和所需的最小信息」（主键哈希 + 前后行哈希 + 增删标志），
// 不含真实列值。下面三个工厂分别造出 INSERT / DELETE / UPDATE 三种语义的变更，
// 让各测试用例能简洁地拼出场景。

// makeInsert —— 构造一条 INSERT 变更：只有 afterHash（新行哈希），isInsert=true。
// 语义：记账时执行 sum += afterHash、rowDelta +1（旧值不存在，故无 beforeHash）。
static TableMutation makeInsert(const QString& table, const QString& pkHash,
                                const QByteArray& afterHash) {
    TableMutation m;
    m.table = table;
    m.pkHash = pkHash;
    m.afterHash = afterHash;
    m.isInsert = true;
    m.isDelete = false;
    return m;
}
// makeDelete —— 构造一条 DELETE 变更：只有 beforeHash（旧行哈希），isDelete=true。
// 语义：记账时执行 sum −= beforeHash、rowDelta −1（新值不存在，故无 afterHash）。
static TableMutation makeDelete(const QString& table, const QString& pkHash,
                                const QByteArray& beforeHash) {
    TableMutation m;
    m.table = table;
    m.pkHash = pkHash;
    m.beforeHash = beforeHash;
    m.isInsert = false;
    m.isDelete = true;
    return m;
}
// makeUpdate —— 构造一条 UPDATE 变更：前后哈希都有，isInsert/isDelete 均为 false。
// 语义：记账时 sum += after、sum −= before（既加新又减旧），行数不变（rowDelta 0）。
// 注：本套件当前用例未直接用到它，但保留以完整覆盖 TableMutation 的三种语义。
static TableMutation makeUpdate(const QString& table, const QString& pkHash,
                                const QByteArray& before, const QByteArray& after) {
    TableMutation m;
    m.table = table;
    m.pkHash = pkHash;
    m.beforeHash = before;
    m.afterHash = after;
    m.isInsert = false;
    m.isDelete = false;
    return m;
}

// TstSyncTableState —— TableStateStore 的测试套件。
class TstSyncTableState : public QObject {
    Q_OBJECT
    QString conn_;     // 本用例专属连接名（UUID 派生，保证隔离）
    QSqlDatabase db_;  // 内存库句柄（成员持有，供 init/cleanup 跨钩子复用）

    // readState —— 套件内私有便捷封装：新建一个 TableStateStore、init、读一行表态。
    // 注意它【不是】测试用例（不在 private slots 里），仅为减少样板而存在；
    // 多数用例其实直接用局部 ts.readState(...)，此封装为可能的复用保留。
    void readState(const QString& table, qint64 epoch, QString* fp, QString* checksum,
                   qint64* rowCount) {
        TableStateStore ts;
        QString err;
        ts.init(db_, &err);
        bool found = false;
        ts.readState(db_, table, epoch, fp, checksum, rowCount, &found, &err);
    }

   private slots:
    // init —— 每用例前钩子：建全新内存库，并执行同步子系统的【全部建表 DDL】。
    // 为什么跑 allCreateStatements()：TableStateStore 读写的是 __sync_table_state 表，
    //   该表由 SyncDDL 创建；测试需先把这些元数据表建好，被测方法才有表可操作。
    void init() {
        conn_ =
            QStringLiteral("tst_ts_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
        db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn_);
        db_.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db_.open());
        QSqlQuery q(db_);
        // 建好同步子系统的全部元数据表（含 __sync_table_state）。
        for (const QString& s : dbridge::sync::ddl::allCreateStatements())
            q.exec(s);
    }
    // cleanup —— 每用例后钩子：先释放成员句柄，再关闭并移除连接（顺序同 WriteTxn 测试）。
    void cleanup() {
        db_ = QSqlDatabase();  // release handle before removeDatabase
                               // 【译】移除前先释放句柄引用
        if (QSqlDatabase::contains(conn_)) {
            QSqlDatabase::database(conn_).close();
            QSqlDatabase::removeDatabase(conn_);
        }
    }

    // init_ok —— 验证：init() 能在“表已由 DDL 建好”的库上成功自检通过。
    // 这是最基础的“环境就绪”用例：若它失败，后续所有读写表态的用例都无意义。
    void init_ok() {
        TableStateStore ts;
        QString err;
        QVERIFY(ts.init(db_, &err));
    }

    // rowHash_stable —— 验证 rowHash 的【确定性】：同一行内容两次哈希必须完全相等。
    // 为什么重要：模加和校验和依赖“同内容→同哈希”，否则插入与后续删除无法精确抵消、
    //   两端相同数据也会算出不同校验和，整个快速判等机制就失效。顺带验证哈希非空。
    void rowHash_stable() {
        QVariantMap row;
        row["id"] = 1;
        row["name"] = "Alice";
        QByteArray h1 = TableStateStore::rowHash(row);
        QByteArray h2 = TableStateStore::rowHash(row);
        QCOMPARE(h1, h2);        // 同输入 → 同输出（确定性）
        QVERIFY(!h1.isEmpty());  // 哈希结果非空
    }

    // rowHash_differentValues_different —— 验证 rowHash 的【区分性】：不同内容应得不同哈希。
    // 为什么重要：若不同行碰撞成同一哈希，模加和就无法区分它们，校验和会漏报差异。
    //   这里用最小差异（id=1 vs id=2）做合理性检查（非密码学强度证明，仅排除明显退化）。
    void rowHash_differentValues_different() {
        QVariantMap r1, r2;
        r1["id"] = 1;
        r2["id"] = 2;
        QVERIFY(TableStateStore::rowHash(r1) != TableStateStore::rowHash(r2));
    }

    // applyMutations_insert_incrementsRowCount —— 验证：一次 INSERT 让 row_count +1。
    // INSERT: row_count +1, checksum += H(new)
    // GIVEN 空表态；WHEN 对 orders 表应用一条 INSERT；
    // THEN readState 能读到该 (table, epoch) 的表态（found=true），且 row_count==1。
    void applyMutations_insert_incrementsRowCount() {
        TableStateStore ts;
        QString err;
        ts.init(db_, &err);
        QByteArray h = QByteArray("row_hash_1");
        QList<TableMutation> muts = {makeInsert("orders", "pk1", h)};
        QVERIFY(ts.applyMutations(db_, muts, 1, "fp1", 1, &err));

        QString fp, cs;
        qint64 rc = 0;
        bool found = false;
        QVERIFY(ts.readState(db_, "orders", 1, &fp, &cs, &rc, &found, &err));
        QVERIFY(found);           // 表态记录已被创建
        QCOMPARE(rc, qint64(1));  // 一次插入 → 行数 1
    }

    // applyMutations_delete_decrementsRowCount —— 验证：DELETE 让 row_count −1。
    // DELETE: row_count -1
    // GIVEN 先插入一行（rc 应为 1）；WHEN 再用【同一个哈希 h】删除它；
    // THEN row_count 回到 0。用同一 h 是为了让删除精确对应先前那次插入。
    void applyMutations_delete_decrementsRowCount() {
        TableStateStore ts;
        QString err;
        ts.init(db_, &err);
        QByteArray h = QByteArray("row_hash_1");
        // insert first  【译】先插入一行
        ts.applyMutations(db_, {makeInsert("t", "pk1", h)}, 1, "fp", 1, &err);
        // then delete  【译】再删除同一行（用同一哈希）
        QVERIFY(ts.applyMutations(db_, {makeDelete("t", "pk1", h)}, 1, "fp", 2, &err));

        QString fp, cs;
        qint64 rc = 0;
        bool found = false;
        ts.readState(db_, "t", 1, &fp, &cs, &rc, &found, &err);
        QCOMPARE(rc, qint64(0));  // 插入又删除 → 行数归 0
    }

    // applyMutations_insertDelete_checksumCancels —— 验证模加和的【可抵消】核心性质：
    //   对同一行先插入(+H)、后删除(−H)，校验和精确回到“零”（且行数回 0）。
    // INSERT then DELETE with same hash => checksum returns to zero (modular add/sub)
    // 这条用例是模加和设计的“试金石”：+H 与 −H 在模 2^64 下互为逆元，必然抵消。
    // 断言里 cs 允许 "0000000000000000"（定宽十六进制零）或 "0"（紧凑形式），
    //   因为不同存储格式下零的字符串表示可能不同，但语义都为零。
    void applyMutations_insertDelete_checksumCancels() {
        TableStateStore ts;
        QString err;
        ts.init(db_, &err);
        QByteArray h = QByteArray("same_hash");
        ts.applyMutations(db_, {makeInsert("t", "pk1", h)}, 1, "fp", 1, &err);
        ts.applyMutations(db_, {makeDelete("t", "pk1", h)}, 1, "fp", 2, &err);

        QString fp, cs;
        qint64 rc = 0;
        bool found = false;
        ts.readState(db_, "t", 1, &fp, &cs, &rc, &found, &err);
        // After insert+delete of same row, checksum should be 0 (hex "0000000000000000")
        // and row_count=0
        // 【译】对同一行插入+删除后，校验和应归零、行数应为 0。
        QCOMPARE(rc, qint64(0));
        QVERIFY(cs == "0000000000000000" || cs == "0");
    }

    // g06_highWaterDoesNotAffectIdentity —— 验证 G-06 不变量：表「身份」与 high_water 无关。
    // G-06: two nodes with same content but different high_water => both read as "identical"
    //        because content_checksum + schema_fp + row_count determine identity, NOT high_water
    // 【背景】high_water_seq（最高已处理序号）只是“进度”信息，不代表“内容”。两个节点若
    //   到达相同内容但途经的变更序号不同（high_water 不同），它们仍应被判为数据一致。
    // GIVEN 节点1：插入一行 X（origin_seq=1）；记录其表态 (cs1, rc1, fp1)。
    // WHEN  节点2：用更大的 origin_seq（99、100）插入再删除另一行 pk2——令 high_water 推进，
    //       但净内容仍是“那一行 X”（pk2 被加又被删，抵消）。
    // THEN  两次读出的 content_checksum / row_count / schema_fp 完全相同——证明身份判定
    //       只看内容三元组，不看 high_water。
    void g06_highWaterDoesNotAffectIdentity() {
        TableStateStore ts;
        QString err;
        ts.init(db_, &err);
        QByteArray h = QByteArray("row_hash_X");
        // 节点1：插入一行 X，origin_seq=1（high_water 推进到 1）。
        ts.applyMutations(db_, {makeInsert("t", "pk1", h)}, 1, "fp", /*originSeq=*/1, &err);

        QString fp1, cs1;
        qint64 rc1 = 0;
        bool f1 = false;
        ts.readState(db_, "t", 1, &fp1, &cs1, &rc1, &f1, &err);

        // simulate second node: same content but high_water advanced via a different origin_seq
        // 【译】模拟第二个节点：净内容相同，但 high_water 因更大的 origin_seq 而推进。
        //   插入 pk2(seq=99) 再删除 pk2(seq=100)——二者抵消，最终净内容仍只有那一行 X，
        //   但 high_water 已被推到 100。
        ts.applyMutations(db_, {makeInsert("t", "pk2", h)}, 1, "fp", /*originSeq=*/99, &err);
        ts.applyMutations(db_, {makeDelete("t", "pk2", h)}, 1, "fp", /*originSeq=*/100, &err);

        QString fp2, cs2;
        qint64 rc2 = 0;
        bool f2 = false;
        ts.readState(db_, "t", 1, &fp2, &cs2, &rc2, &f2, &err);

        // content_checksum and row_count are same (one row), fp same
        // 【译】校验和、行数（都只剩一行）、结构指纹三者均相同 → 身份相同。
        QCOMPARE(cs1, cs2);
        QCOMPARE(rc1, rc2);
        QCOMPARE(fp1, fp2);
        // (high_water_seq differs but not compared here — it's only informational per G-06)
        // 【译】high_water_seq 此时已不同，但本用例刻意不比较它——按 G-06，它仅是信息性字段，
        //   不参与“两端是否一致”的判定。
    }

    // applyMutations_orderInsensitive —— 验证模加和的【顺序无关性】：
    //   把同一批插入以不同顺序应用到两张表，最终校验和必须相同。
    // ORDER-INSENSITIVE: applying mutations in different order yields same checksum
    // Verified via rowHash: H(row1)+H(row2) == H(row2)+H(row1) (modular add is commutative)
    // 【为什么重要】真实同步中各节点收到变更的顺序天然不同；只有“顺序无关”才能保证
    //   不同到达顺序的节点最终算出一致的校验和，从而正确判等。根因是模加满足交换律。
    // GIVEN 两行哈希 h1、h2；WHEN 表 ta 按 (h1, h2) 顺序插入、表 tb 按 (h2, h1) 顺序插入；
    // THEN 两表的 row_count 与 content_checksum 完全相同（加法可交换）。
    void applyMutations_orderInsensitive() {
        TableStateStore ts;
        QString err;
        ts.init(db_, &err);

        QByteArray h1("hash_row_1"), h2("hash_row_2");
        // Apply h1 then h2 on table "ta"  【译】表 ta：先 h1 后 h2
        ts.applyMutations(db_, {makeInsert("ta", "pk1", h1)}, 1, "fp", 1, &err);
        ts.applyMutations(db_, {makeInsert("ta", "pk2", h2)}, 1, "fp", 2, &err);

        // Apply h2 then h1 on table "tb"  【译】表 tb：先 h2 后 h1（顺序相反）
        ts.applyMutations(db_, {makeInsert("tb", "pk2", h2)}, 1, "fp", 3, &err);
        ts.applyMutations(db_, {makeInsert("tb", "pk1", h1)}, 1, "fp", 4, &err);

        QString fp_a, cs_a, fp_b, cs_b;
        qint64 rc_a = 0, rc_b = 0;
        bool fa = false, fb = false;
        ts.readState(db_, "ta", 1, &fp_a, &cs_a, &rc_a, &fa, &err);
        ts.readState(db_, "tb", 1, &fp_b, &cs_b, &rc_b, &fb, &err);
        QVERIFY(fa && fb);
        QCOMPARE(rc_a, rc_b);  // same row count  【译】行数相同
        QCOMPARE(cs_a, cs_b);  // same checksum (modular add is commutative)
                               // 【译】校验和相同（模加可交换，故与顺序无关）
    }

    // readState_notFound_returnsFalse —— 验证：查询一张从未记账过的表，found 必须为 false。
    // GIVEN 一个干净库（"nonexistent" 表态从未被写入）；WHEN readState 查它；
    // THEN found=false——上层据此知道“该表尚无表态”，而非误把默认零值当作真实数据。
    void readState_notFound_returnsFalse() {
        TableStateStore ts;
        QString err;
        ts.init(db_, &err);
        QString fp, cs;
        qint64 rc = 0;
        bool found = false;
        ts.readState(db_, "nonexistent", 1, &fp, &cs, &rc, &found, &err);
        QVERIFY(!found);
    }
};

// 生成无 GUI 依赖的测试 main()，并引入 moc 元对象代码（文件名须与本 .cpp 同名）。
QTEST_APPLESS_MAIN(TstSyncTableState)
#include "tst_sync_table_state.moc"
