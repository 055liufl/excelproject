#pragma once
#include "dbridge/sync/SyncTypes.h"

#include <QList>
#include <QSqlDatabase>
#include <QString>

// ============================================================================
// FrozenManifest.h — 选择性推送第 4 阶段：把「冻结清单」持久化到本地控制表
// ============================================================================
//
// 【一句话职责】
//   FrozenManifest 是「冻结清单（frozen manifest）」的持久化网关：它把一批 FrozenEntry
//   （每条 = 一行记录在“冻结那一刻”的快照指纹 + 拓扑序 + 角色）按 (push_id, chunk_seq)
//   读写到本地的内部控制表 __sync_frozen_manifest，供推送过程后续阶段查询与续传。
//   它本身不计算指纹、不做漂移判定，只负责「把冻结结果稳妥地存下来 / 取回来 / 清掉」。
//
// 【什么是“冻结（freeze）”，为什么要冻结】
//   选择性推送（syncSelected）要把“用户选中的若干行 + 它们的外键父行闭包”发给对端。
//   从“决定要推哪些行”到“真正把所有分片都发出并收到 ACK”之间存在一个时间窗口——
//   这期间本地库仍可能被继续写入，导致“正在推的那一行”内容发生变化。若不加防护，
//   对端收到的就可能是“半新半旧”的不一致数据。
//   解决办法：在推送开始前，对每一行拍一张“内容快照指纹（fingerprint）”并连同其
//   拓扑序号、角色（selected/dependency）一起“冻结”记录下来。这份不可变的清单就是
//   frozen manifest。之后无论本地库怎么变，推送都以这份冻结清单为准（snapshot 语义）；
//   并可在需要时把当前行的实时指纹与冻结指纹比对，发现“冻结后又被改动”=漂移（drift），
//   据此发出 W_SYNC_PUSH_ROW_DRIFTED 警告（漂移判定逻辑在调用方，本类只提供冻结指纹）。
//
// 【FrozenEntry 各字段速览（完整定义见 include/dbridge/sync/SyncTypes.h）】
//   · table       —— 行所属表名。
//   · primaryKey  —— 行主键（业务可读形式，字符串承载）。
//   · pkHash      —— 主键哈希（定长键，= SHA-1("table\x1fprimaryKey")，便于做索引/比对）。
//   · recordKind  —— "selected"（用户显式勾选）| "dependency"（外键闭包自动带入的父行）。
//   · topoIndex   —— 拓扑序下标：越小越先应用，保证“父表先于子表”，避免对端外键约束失败。
//   · fingerprint —— 行内容指纹（行的紧凑 JSON 的 SHA-1，二进制字节）。漂移检测的核心依据。
//   指纹与 pkHash 的实际计算发生在上游 ChunkStreamer::entryToFrozen()（见 ChunkStreamer.cpp），
//   本类拿到的是“已经算好的” FrozenEntry，只管落库/取出。
//
// 【在选择性推送链路中的位置】
//   SyncSelection（用户的选择意图：哪些表的哪些主键）
//     → SelectionResolver   （表+主键 → 物化为整行 QVariantMap）
//     → FkClosureBuilder    （补外键父行闭包 + 拓扑排序 → 闭包清单 manifest）
//     → ChunkStreamer       （按字节预算切片，并为每行算 pkHash/fingerprint，生成 FrozenEntry）
//     → ★FrozenManifest★    （本文件：把每个分片的 FrozenEntry 持久化到 __sync_frozen_manifest）
//     → 编码成 SelectionPush artifact 写入 outbox → 传输 → 对端按 topoIndex 顺序 apply
//   续传场景（C13）：若推送被中断后重启，可用 load() 从本地把某 push/chunk 的冻结清单
//   原样取回（仍按 topoIndex 有序），无需重新解析行数据，即可继续未完成的分片。
//
// 【对应的物理表 __sync_frozen_manifest（DDL 见 src/sync/SyncDDL.h）】
//     push_id      TEXT    NOT NULL   -- 本次推送批次 id
//     chunk_seq    INTEGER NOT NULL   -- 分片序号（同一 push 切成多片时区分）
//     table_name   TEXT    NOT NULL   -- = FrozenEntry::table
//     pk_hash      TEXT    NOT NULL   -- = FrozenEntry::pkHash
//     primary_key  TEXT    NOT NULL   -- = FrozenEntry::primaryKey
//     record_kind  TEXT    NOT NULL   -- = FrozenEntry::recordKind
//     topo_index   INTEGER NOT NULL   -- = FrozenEntry::topoIndex
//     fingerprint  BLOB    NOT NULL   -- = FrozenEntry::fingerprint（务必非 NULL，见 .cpp 说明）
//     PRIMARY KEY(push_id, chunk_seq, table_name, pk_hash)   -- 一行在一个分片内唯一
//     FOREIGN KEY(push_id) REFERENCES __sync_push_progress(push_id)
//   注意那条外键约束：在 foreign_keys=ON 时，必须先有父表 __sync_push_progress 中对应
//   push_id 的行，save() 才能成功插入子行——这正是调用方（SyncWorker，见 H-02 fix）
//   先插 push_progress、后调 save() 的原因。
//
// 【协作者】
//   · ChunkStreamer（同目录）：上游，产出已算好指纹/拓扑序的 FrozenEntry。
//   · SyncDDL（src/sync/SyncDDL.h）：建表语句的唯一出处；本类 init() 因此无需自己建表。
//   · SyncWorker（src/sync/SyncWorker.cpp，syncSelected 流程）：在写 outbox 前调用 save()，
//     推送结束/清理时调用 remove()。
//   · FrozenEntry / SelectionPushBody（include/dbridge/sync/SyncTypes.h）：承载的值类型。
//   · 错误码见 include/dbridge/Errors.h（漂移 → W_SYNC_PUSH_ROW_DRIFTED；落库失败由调用方
//     映射为 E_SYNC_TRANSPORT 等，本类只回传驱动错误文本）。
//
// 注释风格参照 include/dbridge/Errors.h / SyncTypes.h / RowPayload.h：`// ──` 分节、中文、
// 信息密集；保留并翻译既有英文注释与各 fix 标记。
// ============================================================================

namespace dbridge::sync {

// Persistent frozen manifest: the topology-ordered list of (table, pk, fingerprint)
// entries for a push-id. Serves as the source for ChunkStreamer resumption (C13).
//   （持久化的冻结清单：某个 push-id 对应的、按拓扑序排列的 (表, 主键, 指纹) 条目列表。
//    它充当 ChunkStreamer 续传（C13）时的数据来源——中断后可据此原样取回冻结状态再续推。）
//
// 【设计取向】本类是“无状态的纯方法集合”：不持有任何成员变量，每个方法都把数据库连接
//   作为参数传入、用完即走。这样它天然可重入、可在 worker 线程内被随意构造（如调用方常以
//   栈对象 `FrozenManifest fm;` 临时使用），也无需考虑对象生命周期跨线程的问题。
// 【线程约定】所有方法都在 SyncWorker 的写线程上、持有写连接 wconn 的前提下被调用；
//   QSqlDatabase 连接本身不可跨线程共享，故调用方负责保证“连接与当前线程绑定”。
class FrozenManifest {
   public:
    // ── init —— 初始化占位（实际建表交给 SyncDDL，这里无事可做）─────────────────
    // 做什么：什么都不做，直接返回成功。物理表 __sync_frozen_manifest 由
    //   SyncDDL::allCreateStatements() 在同步引擎统一建表阶段创建，本类不重复建表，
    //   以免出现“建表 DDL 散落多处、版本漂移”的维护隐患。
    // 为什么仍保留这个方法：给本类一个与其它 *Store 组件一致的初始化入口（接口对称性），
    //   将来若需要为冻结清单做额外的运行时准备（如预热缓存、校验索引），有现成挂载点。
    // 参数：db 写连接（当前未使用）；err 出参错误（当前不会写）。
    // 返回：恒为 true。
    // 副作用：无。线程：随调用方（写线程）。复杂度：O(1)。
    bool init(QSqlDatabase& db, QString* err);

    // ── save —— 把一个分片的冻结清单条目写入控制表 ───────────────────────────────
    // Persist entries for a push (called before releasing the read snapshot, C16)
    //   （为一次推送持久化条目；在“释放读快照之前”调用——C16：必须趁还握着冻结时刻的
    //    一致快照时就把清单落库，否则等快照释放后本地库再被改动，冻结指纹就不再可信。）
    //
    // 做什么：对 entries 中的每一条 FrozenEntry，向 __sync_frozen_manifest 执行一条
    //   INSERT OR REPLACE，键为 (pushId, chunkSeq, table, pkHash)。
    // 为什么用 INSERT OR REPLACE：使本方法对“同一分片重复保存”天然幂等——重发/重启续传时
    //   再次写入同一组键不会因主键冲突而失败，而是覆盖为最新冻结值（同一冻结快照下值相同，
    //   故覆盖是安全无害的）。
    // 前置条件（重要）：在 foreign_keys=ON 下，父表 __sync_push_progress 必须已存在该 pushId
    //   的行，否则首条 INSERT 即触发外键约束失败。调用方（SyncWorker）按 H-02 fix 的顺序，
    //   先插 push_progress 父行、再调用本方法。
    // 参数：
    //   db        —— 写连接（与当前线程绑定）。
    //   pushId    —— 本次推送批次 id（写入每行的 push_id 列）。
    //   chunkSeq  —— 本分片序号（写入每行的 chunk_seq 列）。
    //   entries   —— 本分片的冻结条目（其 fingerprint 须为非空字节，见下方 .cpp NOT NULL 说明）。
    //   err       —— 出参：任一行写入失败时填入底层驱动错误文本（可为 nullptr 表示不关心）。
    // 返回：true 全部条目写入成功；false 中途某条失败（已写入的前序条目不会回滚——事务边界
    //   由调用方掌控，本方法不自开事务）。
    // 副作用：向 __sync_frozen_manifest 插入/替换 entries.size() 行。
    // 错误模式：外键约束失败 / 主键问题 / 连接异常等 → 返回 false 并透传 *err。
    // 线程：写线程。复杂度：O(entries 条数)，每条一次预编译语句执行。
    bool save(QSqlDatabase& db, const QString& pushId, int chunkSeq,
              const QList<FrozenEntry>& entries, QString* err);

    // ── load —— 取回某个 push/分片的冻结清单（按拓扑序）────────────────────────────
    // Load entries for an existing push (for resumption)
    //   （加载某个已存在推送的条目；用于续传——中断后据此把冻结状态原样恢复，无需重算指纹。）
    //
    // 做什么：按 (pushId, chunkSeq) 查询 __sync_frozen_manifest，ORDER BY topo_index 返回，
    //   重建出一份与冻结时刻一致、且“父先子后”有序的 FrozenEntry 列表。
    // 为什么按 topo_index 排序：拓扑序是“可安全应用”的顺序（父表行先于引用它的子表行），
    //   取回即有序，调用方无需再排序就能直接顺序 apply / 续推。
    // 参数：db 读/写连接；pushId 推送批次 id；chunkSeq 分片序号。
    // 返回：该分片的 FrozenEntry 列表（按 topoIndex 升序）；若查询失败或无匹配，返回空列表。
    // 注意：本方法不区分“查询失败”与“确实没有数据”——两者都返回空列表，不写错误出参
    //   （续传语义下，空即视为“该分片无需恢复”，由调用方按空集合处理）。
    // 副作用：只读查询，不改库。线程：写/读线程均可。复杂度：O(命中行数)。
    QList<FrozenEntry> load(QSqlDatabase& db, const QString& pushId, int chunkSeq);

    // ── remove —— 清理一次推送的全部冻结条目 ─────────────────────────────────────
    // Cleanup finished push
    //   （清理已完成的推送：把该 pushId 的所有分片冻结条目一次性删除。）
    //
    // 做什么：DELETE FROM __sync_frozen_manifest WHERE push_id=?（不限 chunk_seq，整批清掉）。
    // 何时调用：一次推送的全部分片都已被对端 ACK、不再需要续传时，回收这批冻结记录，
    //   避免控制表无限堆积。
    // 参数：db 写连接；pushId 要清理的推送批次 id；err 出参错误（可为 nullptr）。
    // 返回：true 删除成功（即便没有匹配行也算成功——幂等清理）；false 执行失败并填 *err。
    // 副作用：删除 __sync_frozen_manifest 中 push_id 匹配的所有行。
    // 线程：写线程。复杂度：O(被删行数)。
    bool remove(QSqlDatabase& db, const QString& pushId, QString* err);
};

}  // namespace dbridge::sync
