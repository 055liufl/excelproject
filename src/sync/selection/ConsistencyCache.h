#pragma once
#include <QByteArray>
#include <QHash>
#include <QSqlDatabase>
#include <QString>

// ============================================================================
// ConsistencyCache.h — 选择性推送的「一致性剪枝缓存」（与对端已一致的父行指纹库）
// ============================================================================
//
// 【这个文件是什么 / 一句话职责】
//   维护一张「(表, 主键) → 中心权威指纹」的映射表，记录哪些行本节点确信「对端
//   中心已经持有与我完全相同的副本」。选择性推送在补外键闭包时，凭这张缓存把
//   「对端早已一致的父依赖行」剪掉，避免重复推送，从而缩小推送体积。
//
// 【为什么需要它（动机）】
//   选择性推送的核心难题是：用户只勾选了几行子表数据，但这些行通过外键指向父表，
//   必须把父行一并带上（闭包），对端才能无外键冲突地 apply（见 FkClosureBuilder）。
//   然而很多父行（如「客户」「商品」字典表）在历次同步/基线导入后，对端其实早就
//   有了一份一模一样的副本——再推一遍纯属冗余。ConsistencyCache 就是那本「对端
//   已有清单」：补闭包时先查它，命中即「一致性剪枝」，不再把该父行塞进推送包。
//
// 【在选择性推送链路中的位置】
//   SyncSelection（意图：选哪些行 + 开关 includeFkDeps/pruneConsistent）
//     → SelectionResolver（表+主键 → 整行 QVariantMap）
//     → FkClosureBuilder（补 FK 闭包 + 拓扑排序）
//         └─ 补每个父行前调用 ★ConsistencyCache::isConsistent★ 决定是否剪枝
//     → FrozenManifest → ChunkStreamer → SelectionPush
//   即本类是 FkClosureBuilder 在「补闭包」内环里反复查询的旁路缓存。
//
// 【缓存键与值（数据模型）】
//   · 键 = (table_name, primary_key)：表名 + 行主键（主键统一以字符串承载，与
//     SelectionResolver/FkClosureBuilder 全程一致，避免类型歧义）。
//   · 值 = center_fingerprint：该行在「对端中心」侧被确认时的内容指纹（SHA1）。
//     指纹算法必须与 FkClosureBuilder::rowFingerprint() 完全一致——对 QVariantMap
//     （按列名排序）逐 "列\0值\0" 拼接后取 SHA1——否则本地指纹永远比不中，剪枝失效。
//
// 【命中 / 失效 / 盖戳 三条主线（生命周期）】
//   · 盖戳（写入）：stampFromAuthoritative()——当本节点从「权威来源」（基线导入、
//     中心 ACK 等）确认某行已与中心一致时，把该行的中心指纹记进缓存。
//   · 命中（读取）：isConsistent()——补闭包时拿「本地当前行指纹」与缓存比对，
//     相等 ⇒ 对端已有同物 ⇒ 剪枝。注意比的是指纹而非「键是否存在」：键在但指纹
//     不等（本地后来又改了这行）⇒ 不一致 ⇒ 仍要推。
//   · 失效（删除）：invalidateTable()——一旦有入站变更 apply 到某张表，该表所有
//     缓存条目立即作废（整表清除）。因为入站 apply 改了本地数据，原先「与中心一致」
//     的判断不再可信，宁可全部重算也不能误剪。
//
// 【可持久化（durable）】
//   由 SyncConfig::consistencyCacheDurable() 控制：
//     · durable=true（默认）：缓存落盘到 __sync_consistency_cache 表，进程重启后
//       loadFromDb() 重新载入——跨会话保留「对端已有」知识，避免重启后第一次推送
//       又把一切父行重推一遍。
//     · durable=false：纯内存缓存，进程退出即丢失（适合测试或不在意冷启动开销）。
//   无论是否持久化，内存哈希 memCache_ 都是查询的唯一权威来源；持久化只是它的镜像。
//
// 【⚠ 建表 DDL 的「双轨」陷阱（务必理解，关乎 M-01 fix）】
//   本类内部带了一份兜底 DDL（见 .cpp 的 kCreateTable），但它**不含 updated_ms 列**；
//   而生产中真正生效的权威 DDL 在 src/sync/SyncDDL.h（含 updated_ms INTEGER NOT NULL）。
//   持久化写入（persistStamp）的 INSERT 语句故意带上 updated_ms 列——以匹配 SyncDDL.h
//   建出的真实表结构（详见 .cpp 中 M-01 fix 注释）。换言之：本类假定表通常已由
//   SyncDDL.h 建好；kCreateTable 仅是「表恰好不存在时」的最低限度兜底。
//
// 【线程模型】
//   无内部锁。约定只在单一同步 worker 线程内使用；持有的 QSqlDatabase 连接亦不跨线程。
//
// 注释风格参照 include/dbridge/Errors.h、Types.h、RowPayload.h：`// ──` 分节、中文、信息密集。
// 错误本类不直接产出错误码，仅以 bool + QString* err 透传 SQL 驱动文本。
// ============================================================================

namespace dbridge::sync {

// In-memory (optionally durable) fingerprint cache for determining
// whether a dependency row is already consistent with the center.
//   （内存（可选持久化）的指纹缓存，用于判断某个「依赖行」是否已与中心一致。）
class ConsistencyCache {
   public:
    // ── init —— 初始化缓存（清空内存，可选载入持久化数据） ────────────────────
    // Initialize; if durable=true, creates/reads __sync_consistency_cache table.
    //   （初始化；durable=true 时会创建/读取 __sync_consistency_cache 表。）
    // 做什么：先把 memCache_ 清空并记下 durable 模式；若非持久化则直接成功返回；
    //   若持久化，则用兜底 DDL 确保表存在（kCreateTable），再从表中把全部条目载入内存。
    // 为什么：worker 每次（重）启动同步前调用一次，把上次会话的「对端已有」知识恢复到内存。
    // 参数：db 工作连接（建表/读取用）；durable 是否落盘；err 出参，失败时写入 SQL 错误文本。
    // 返回：true=初始化成功；false=建表或读取失败（*err 已写）。
    // 副作用：清空 memCache_；durable 时对 db 执行 CREATE TABLE IF NOT EXISTS + SELECT。
    // 线程：worker 线程内调用；db 不跨线程。
    bool init(QSqlDatabase& db, bool durable, QString* err);

    // ── isConsistent —— 命中判定：本地指纹是否等于缓存的中心指纹 ──────────────
    // Returns true if the local fingerprint matches the cached center fingerprint.
    //   （若「本地指纹」与缓存中的「中心指纹」相等则返回 true。）
    // 做什么：按 (table, pk) 在内存哈希里查值，存在且 == localFp 才算「一致」。
    // 为什么：FkClosureBuilder 补每个父行前调用本函数——返回 true 即「对端已有同物」，
    //   于是把该父行剪掉（不入闭包、不推送）；返回 false 则照常补进闭包。
    // 关键不直观点：比较的是「指纹值是否相等」，不是「键是否存在」。
    //   · 键不存在        → false（从没确认过一致 → 不能剪）。
    //   · 键在但指纹不等  → false（本地这行在盖戳之后又被改过，已偏离中心 → 必须推）。
    //   · 键在且指纹相等  → true （确实仍与中心一致 → 安全剪枝）。
    // 参数：table 表名；pk 行主键（字符串）；localFp 本地此刻重新计算出的行指纹。
    // 返回：true=一致可剪枝；false=不一致或无记录，需照常推送。
    // 副作用：无（const，纯读内存）。 复杂度：两级 QHash 查找 ≈ O(1)。
    // 线程：仅读 memCache_，但无锁，仍约定单线程使用。
    bool isConsistent(const QString& table, const QString& pk, const QByteArray& localFp) const;

    // ── stampFromAuthoritative —— 盖戳：登记「此行已被中心确认为该指纹」 ───────
    // Record that center has confirmed this row's fingerprint.
    //   （登记：中心已确认这一行的指纹（即对端现在持有此版本）。）
    // 做什么：把 centerFp 写进内存哈希 memCache_[table][pk]；若持久化再落盘一份。
    // 为什么：当本节点从权威来源得知某行已与中心一致时调用——典型场景是基线导入完成后
    //   （见 SyncWorker：apply 完基线，对每张同步表逐行盖戳）或收到中心对推送的 ACK 后。
    //   有了戳，后续补闭包才能凭 isConsistent 把这些行剪掉。
    // 参数：db 工作连接（仅持久化时用于落盘）；table/pk 标识行；centerFp 中心侧确认的指纹。
    //   注意 centerFp 必须按与 isConsistent 比较时一致的算法计算（同 rowFingerprint）。
    // 返回：void（落盘失败被静默吞掉——见 .cpp 说明：内存戳已生效，持久化失败仅损失冷启动收益）。
    // 副作用：写 memCache_；durable 时 INSERT OR REPLACE 进持久化表。
    // 线程：单线程使用；db 不跨线程。 复杂度：O(1) + 一次可选 SQL 写。
    void stampFromAuthoritative(QSqlDatabase& db, const QString& table, const QString& pk,
                                const QByteArray& centerFp);

    // ── invalidateTable —— 整表失效：入站 apply 改动某表后丢弃其全部缓存 ───────
    // Invalidate all cached entries for a table (called on inbound apply).
    //   （让某张表的所有缓存条目失效；在「入站变更被 apply」时调用。）
    // 做什么：从内存哈希删掉整个 table 子表；若持久化再从落盘表 DELETE 该表所有行。
    // 为什么：入站 apply 修改了本地这张表的数据，先前「与中心一致」的所有判断都可能不再成立。
    //   逐行甄别代价高且易错，索性整表作废、宁可下次重算——保证绝不「误剪」一行其实需要推的数据。
    //   （由 BaselineManager 等在写入本地表后调用，见 BaselineManager.cpp。）
    // 参数：db 工作连接（仅持久化时用于落盘删除）；table 被改动的表名。
    // 返回：void（落盘删除失败同样静默——内存已清，最坏只是持久化镜像滞留，下次 init 会被覆盖）。
    // 副作用：从 memCache_ 移除该表；durable 时 DELETE FROM 表 WHERE table_name=table。
    // 线程：单线程使用。 复杂度：O(该表条目数)。
    void invalidateTable(QSqlDatabase& db, const QString& table);

   private:
    // 内存主存储：两级哈希。memCache_[table][pk] = 该行被确认时的「中心指纹」。
    //   外层按表名分桶、内层按主键定位，正好对应「整表失效」可一次删一桶、
    //   「单行命中/盖戳」可 O(1) 定位的访问模式。这是查询的唯一权威来源；
    //   持久化表只是它的落盘镜像（启动时由 loadFromDb 反向灌回内存）。
    QHash<QString, QHash<QString, QByteArray>> memCache_;

    // 是否持久化。true=落盘到 __sync_consistency_cache，跨会话保留；false=纯内存。
    // 默认 true，与 SyncConfig::consistencyCacheDurable() 的默认一致。
    bool durable_ = true;

    // ── loadFromDb —— 把持久化表的全部条目灌回 memCache_（仅 durable 时） ──────
    // 启动时由 init 调用。SELECT 三列（表名/主键/指纹）逐行填进内存哈希。
    // 返回：true=载入成功；false=SELECT 失败（*err 写驱动文本）。
    bool loadFromDb(QSqlDatabase& db, QString* err);

    // ── persistStamp —— 把单条盖戳落盘（INSERT OR REPLACE，仅 durable 时） ─────
    // 由 stampFromAuthoritative 在 durable 模式下调用。
    // ⚠ 见 .cpp 的 M-01 fix：INSERT 列表必须包含 updated_ms（SyncDDL.h 真实表里
    //   该列是 NOT NULL），否则触发约束失败。
    // 返回：true=写入成功；false=exec 失败（调用方静默忽略其返回值，内存戳已生效）。
    bool persistStamp(QSqlDatabase& db, const QString& table, const QString& pk,
                      const QByteArray& fp);

    // ── deleteTable —— 把某表的全部条目从持久化表删除（仅 durable 时） ─────────
    // 由 invalidateTable 在 durable 模式下调用，DELETE ... WHERE table_name=?。
    // 返回：true=删除成功；false=exec 失败（调用方静默忽略，内存已清是关键）。
    bool deleteTable(QSqlDatabase& db, const QString& table);
};

}  // namespace dbridge::sync
