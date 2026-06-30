#pragma once
#include <QByteArray>
#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QVariantMap>

// ============================================================================
// TableStateStore.h — 每表「表态」（schemaFingerprint / contentChecksum /
//                     rowCount）的增量维护器
// ============================================================================
//
// 【这个文件是什么 / 在同步管线中的位置】
//   同步子系统在每张被同步的业务表上维护一份「表态」快照，落盘在元数据表
//   __sync_table_state 中，键为 (table_name, stream_epoch)。一行表态记录回答三个问题：
//     · schema_fingerprint —— 这张表当前的「结构指纹」（列名/类型/约束的哈希）；
//       两端指纹不一致 → E_SYNC_SCHEMA_MISMATCH，不能盲目互相应用变更；
//     · content_checksum   —— 这张表当前「全部内容」的一个 64 位累加校验和；
//       两端校验和一致 ≈ 数据一致，可用于「差异快速判等」（无需逐行比对即可
//       先判断“是否需要进一步比对”）；
//     · row_count          —— 行数，作为校验和之外的第二维一致性佐证。
//   这套快照让上层（比对/对账逻辑）能以 O(1) 代价先做一次“两端是否可能不同”的
//   廉价判断，只有判等失败时才退化到逐行 diff。
//
// 【stream_epoch 是什么】
//   同步「流纪元」。每当基线（baseline）被重建，纪元自增；同一张表在不同纪元下
//   各有独立的一行表态。这样基线重置不会污染旧纪元的历史，也保证“跨纪元的校验和
//   不会被错误地相互比较”。
//
// 【content_checksum 为什么是「模加和」而不是整库哈希】
//   关键设计：校验和被定义为「所有行的行哈希（取前 8 字节当作 quint64）按模 2^64
//   累加」。模加和具备「可增量更新」的代数性质——
//     · 插入一行：sum += hash(after)；
//     · 删除一行：sum -= hash(before)；
//     · 更新一行：sum += hash(after) − hash(before)。
//   于是每来一批行级变更（mutation），无需重扫全表即可 O(变更数) 地把校验和推进到
//   新值。代价是它不是密码学意义上的强校验（攻击者可构造碰撞），但用于“同步双方
//   一致性自检”足够，且换来了巨大的性能优势。整库重算只在 resetFromBaseline 时发生。
//
// 【为什么校验和以「十六进制/十进制字符串」存储】
//   SQLite 的 INTEGER 是有符号 64 位，无法无损存放 quint64 的高位（最高位会变负）。
//   故把 quint64 用 QString::number 转成十进制字符串存进 TEXT 列，读回时再
//   toULongLong 还原，全程保持无符号语义（见 .cpp 的 M-02 fix）。
//
// 【协作者】
//   · SyncDDL —— 负责 CREATE __sync_table_state（本类 init() 只校验其可访问，不建表）；
//   · WriteTxn —— applyMutations() 必须在一个已开启的写事务内被调用，保证“业务数据
//     变更”与“表态更新”原子地一起提交；
//   · SqlBuilder::quoteIdent —— 扫描业务表时安全地为表名加引号（H-3 fix）；
//   · 上层比对/对账逻辑 —— 通过 readState() 取两端表态做快速判等。
//
// 【线程模型】
//   无内部可变成员（纯方法类），所有状态都在传入的 QSqlDatabase 里。每个工作线程
//   持有自己的 QSqlDatabase 连接并各自调用，互不共享对象状态。
// ============================================================================

namespace dbridge::sync {

// TableMutation —— 用于「表态记账」的单条行级变更描述。
// 注意：它只携带做模加和所需的最小信息（主键哈希 + 前后行哈希 + 增删标志），
// 并不携带真正的列值——真正的数据变更由别处写入业务表，这里只负责更新校验和。
struct TableMutation {
    QString table;   // 该变更命中的业务表名
    QString pkHash;  // 主键哈希（标识是“哪一行”；记账本身按表聚合，此处主要供上层定位）
    QByteArray beforeHash;  // 变更前的行哈希；INSERT 时为空（旧值不存在）
    QByteArray afterHash;   // 变更后的行哈希；DELETE 时为空（新值不存在）
    bool isInsert = false;  // 是否为插入：true → 只 add(afterHash)、rowDelta +1
    bool isDelete = false;  // 是否为删除：true → 只 sub(beforeHash)、rowDelta -1
                            // 两者皆 false → 视为更新：既 add(after) 又 sub(before)，行数不变
};

// TableStateStore —— 用「增量校验和」维护 __sync_table_state 的存取器。
// content_checksum 在内存里是 quint64 模加和，落盘时以十进制字符串保存（见文件头说明）。
class TableStateStore {
   public:
    // init —— 启动期自检：确认 __sync_table_state 可被访问（表由 SyncDDL 创建，本类不建表）。
    // 实现用 "SELECT COUNT(*) ... WHERE 0" 这种零行查询，仅验证表/连接可用而不读数据。
    // 返回 true 成功；失败时（若 err 非空）填入 SQL 错误文本。
    bool init(QSqlDatabase& db, QString* err);

    // applyMutations —— 把一批行级变更增量地累加进各表的表态。
    // 【做什么】先按表把 muts 聚合成「每表的 add/sub/行数增量」（用无符号 quint64 累加，
    //   保持模加语义），再对每张受影响的表执行一次 updateRow upsert。
    // 【为什么】避免重扫全表：N 条变更 → O(N) 更新校验和。
    // 【副作用】写 __sync_table_state；必须在一个已开启的 WriteTxn 内调用，以便与业务
    //   数据变更同事务提交（否则崩溃可能导致“数据已变、表态没变”的不一致）。
    // 【参数】streamEpoch 当前流纪元；schemaFp 当前结构指纹（一并写入，反映可能的结构变化）；
    //   originSeq 触发本批变更的来源序号，用作 high_water_seq（只增不减，见 updateRow）。
    // 【返回】全部表更新成功返回 true；任一失败立即返回 false 并通过 err 上报。
    bool applyMutations(QSqlDatabase& db, const QList<TableMutation>& muts, qint64 streamEpoch,
                        const QString& schemaFp, qint64 originSeq, QString* err);

    // readState —— 读取某表在某纪元下的当前表态，供「差异快速判等」。
    // J-12: Returns true on success (query executed without error), false on query error.
    // *found is set to true iff a state row exists; false means the table has never been
    // synced (no row in __sync_table_state). Output pointers fp/checksum/rowCount are only
    // populated when *found is true.
    // 译：J-12 — 必须区分「查无此行」与「查询出错」两种情况：
    //   · 查询本身执行无误即返回 true（哪怕没有命中任何行）；只有 SQL 真正报错才返回 false。
    //   · *found 仅当确实存在表态行时置 true；false 表示该表从未被同步过（元数据表里没有这一行）。
    //   · 输出参数 fp/checksum/rowCount 只有在 *found==true 时才被填充（调用方据此判断有效性）。
    // 这种「三态」设计避免把“表是新的”误当成“查询失败”，是上层做判等时的关键前提。
    bool readState(QSqlDatabase& db, const QString& table, qint64 streamEpoch, QString* fp,
                   QString* checksum, qint64* rowCount, bool* found, QString* err);

    // resetFromBaseline —— 基线全量重置：丢弃旧表态，逐表全扫重算校验和与行数。
    // 【做什么】对每张表：先 DELETE 掉本纪元下的旧表态行，再 SELECT * 整表扫描、
    //   对每行算行哈希并模加累计，最后 INSERT OR REPLACE 写回一行全新的表态。
    // 【为什么】增量记账长期累加可能与真实数据「漂移」（漏记/外部直写），或建立新基线时
    //   需要一个权威的“从头算”的真值；本方法即提供这个 O(全表行数) 的权威重算入口。
    // 【副作用】重写 __sync_table_state 中这些表在本纪元的行；high_water_seq 复位为 0。
    bool resetFromBaseline(QSqlDatabase& db, const QStringList& tables, qint64 streamEpoch,
                           const QString& schemaFp, QString* err);

    // rowHash —— 计算一行的「规范化行哈希」：SHA-256 后截取前 16 字节。
    // 【为什么要“规范化”】不同机器/不同迭代顺序必须得到同一哈希，否则两端校验和无从比较。
    //   实现采用「长度前缀 + 类型标签」编码并按列名排序（见 .cpp 的 H-04 fix），杜绝
    //   “可构造的碰撞”（例如把两列拼接误判为相等）。
    // 【返回】16 字节摘要；其前 8 字节再经 hashToU64 转成 quint64 参与模加和。
    // static：纯函数，无对象状态依赖，便于在扫描循环里直接调用。
    static QByteArray rowHash(const QVariantMap& row);

   private:
    // hashToU64 —— 取哈希的前 8 字节按「大端」拼成 quint64（不足 8 字节返回 0）。
    // 大端固定字节序保证跨平台一致：同一行哈希在任何机器上映射到同一个 quint64。
    static quint64 hashToU64(const QByteArray& h);

    // updateRow —— 对单张表的表态行做一次 upsert，按「无符号模加」推进校验和与行数。
    // 【参数】add/sub 是本批要「加上 / 减去」的 quint64 哈希贡献（分开传递正是为了
    //   规避有符号溢出 UB，见 M-02 fix）；rowCountDelta 行数增量；highWaterSeq 用
    //   MAX 语义只增不减地推进“已处理到的来源序号”。
    // 【实现要点】先读旧 sum/rows，算出 newSum = oldSum + add − sub（纯 quint64 环绕），
    //   再以 ON CONFLICT(table,epoch) DO UPDATE 原子写回（不存在则插入）。
    bool updateRow(QSqlDatabase& db, const QString& table, qint64 streamEpoch,
                   const QString& schemaFp, quint64 add, quint64 sub, qint64 rowCountDelta,
                   qint64 highWaterSeq, QString* err);

    // readChecksum —— 读回当前校验和并还原成 quint64（不存在或解析失败返回 0）。
    // 落盘是十进制字符串，这里用 toULongLong 无损还原无符号值。
    quint64 readChecksum(QSqlDatabase& db, const QString& table, qint64 streamEpoch);

    // readRowCount —— 读回当前行数（不存在返回 0）。
    qint64 readRowCount(QSqlDatabase& db, const QString& table, qint64 streamEpoch);
};

}  // namespace dbridge::sync
