#pragma once
#include "dbridge/Export.h"
#include "dbridge/sync/SyncConfig.h"
#include "dbridge/sync/SyncTypes.h"

#include <QList>
#include <QVariant>

#include <memory>

// ============================================================================
// IComparisonSession.h — 「差异比对 + 交互式合并」会话的对外接口（类 Beyond Compare）
// ============================================================================
//
// 【这个文件是什么】
//   dbridge 同步子系统对外暴露的「比对会话」抽象接口。它把“本地库”和“对端某次
//   快照”放在一起做差异比对，并提供一套交互式合并工作流（逐行/逐单元格地决定
//   采用本地值还是对端值），最后把“采用对端”的决策写回本地库。
//   交互体验类比 Beyond Compare：先看两端差异，再逐项选择如何合并。
//
// 【关键术语：local 与 remote 的语义（务必分清）】
//   · local  = 本地库 = 运行本会话的这台节点自己的 SQLite 数据库（“我”这一端）。
//              比对时的真值来源是本地磁盘上的表。
//   · remote = 对端 = 通过 RemoteTableSnapshot 传进来的“另一节点某次快照”
//              （“对方”那一端）。它不是实时连接，而是一份被快照固定下来的数据。
//   合并方向永远是“把对端的值合并进本地”——save() 改的只可能是本地库；对端只读。
//   因此 acceptLocal = 保留本地（丢弃对该行的采纳），acceptRemote = 采用对端值。
//
// 【两级比对（与下游 DiffEngine 对应，建立直觉）】
//   ① 表级：用 (schemaFingerprint 表结构指纹 + contentChecksum 内容校验和 +
//      rowCount 行数) 三元组快速判定两端某张表是否“完全一致”，只有三者全等才算
//      Identical；任一不同即 Different。→ 产出 TableDiff（见 tableDiffs()）。
//   ② 行级：以主键(PK)为 key，把本地行集合与对端行集合做集合比较，得出每行属于
//      Added / Deleted / Modified / Same，并对 Modified/Added/Deleted 逐列产出
//      CellDiff。→ 产出 RowDiff（见 rowDiffs()）。
//
// 【典型交互流程】
//   auto s = createComparisonSession(config, &err);   // 工厂创建（绑定到某个库）
//   s->initialize(remoteSnapshots, &err);             // 喂入对端快照、算差异、开门控
//   auto tables = s->tableDiffs();                    // 看哪些表有差异（表级概览）
//   auto rows   = s->rowDiffs("orders", 0, 100);      // 翻看某表的逐行差异（分页）
//   s->acceptRemote("orders", "1001");                // 决定：这一行采用对端
//   s->stageCell("orders", "1002", "qty", 5);         // 或：逐单元格手工指定值
//   s->save(&err);                                    // 一次性把暂存决策写回本地库
//   // 或 s->discard();                                // 放弃全部暂存、什么都不改
//
// 【DBRIDGE_EXPORT】
//   符号可见性导出宏（见 Export.h）。本项目以静态库链接时展开为空。
// ============================================================================

namespace dbridge::sync {

// ── 表级差异状态（表与表之间的“整体”关系，不涉及具体哪一行）──────────────────
// 由表级比对的三元组（schemaFingerprint + contentChecksum + rowCount）判定。
enum class TableDiffStatus {
    Identical,  // 完全一致：三元组全等，两端该表无差异（可跳过行级比对）
    Different,  // 有差异：两端都有此表，但内容/结构/行数至少一项不同
    OnlyLocal,  // 仅本地有：对端快照里没有这张表（本地独有）
    OnlyRemote,  // 仅对端有：本地库里没有这张表（对端独有）
};

// ── 行级差异种类（以主键为 key 比较两端行集合后，单行的归类）─────────────────
enum class RowDiffKind {
    Same,   // 两端都有该 PK 且逐列值完全相同（无需合并）
    Added,  // 仅对端有该 PK（对端新增；以本地视角看是“待加入”的行）
    Deleted,  // 仅本地有该 PK（对端不存在；以本地视角看是“对端已删除”的行）
    Modified,  // 两端都有该 PK 但至少一列值不同（需逐列查看 CellDiff 决定取舍）
};

// ── 单元格级差异：某一列在两端的取值对照 ─────────────────────────────────────
// 一个 RowDiff 内含多个 CellDiff（每列一个），是合并取舍的最小粒度。
struct CellDiff {
    QString column;        // 列名（数据库列名）
    QVariant localValue;   // 本地该列的值（本行在 Added 行里为空/无效）
    QVariant remoteValue;  // 对端该列的值（本行在 Deleted 行里为空/无效）
    bool changed = false;  // 该列两端是否不同：localValue != remoteValue 时为 true
};

// ── 行级差异：某个主键对应的一行在两端的完整差异 ─────────────────────────────
struct RowDiff {
    RowDiffKind kind = RowDiffKind::Same;  // 该行的归类（见 RowDiffKind）
    QString primaryKey;  // 该行主键值（以字符串形式表示，作为两端配对的 key）
    QList<CellDiff> cells;  // 逐列差异明细（Added 行只有 remote 侧、Deleted 行只有 local 侧）
};

// ── 表级差异汇总：某张表两端差异的“计数概览”（供 UI 展示总览）────────────────
struct TableDiff {
    QString table;                                        // 表名
    TableDiffStatus status = TableDiffStatus::Identical;  // 表级关系（见 TableDiffStatus）
    int addedRows = 0;     // 对端相对本地新增的行数（估算/统计值）
    int deletedRows = 0;   // 对端相对本地删除的行数
    int modifiedRows = 0;  // 两端都有但内容不同的行数
};

// ── 对端快照：传给 initialize() 的“对方那一端”的只读数据 ─────────────────────
// 注意它是“快照”而非实时连接——一旦传入，对端数据在本会话期内固定不变。
struct RemoteTableSnapshot {
    QString table;  // 表名
    // 表级快速判等用的每表元数据（与本地三元组逐项比较即可判 Identical/Different）。
    struct Meta {
        QString schemaFingerprint;  // 表结构指纹（列定义/类型的哈希）
        QString contentChecksum;  // 内容校验和（全表行内容的聚合校验，十六进制）
        qint64 rowCount = 0;      // 行数
    } meta;
    // 行级比对用的对端行集合。可惰性加载：留空表示稍后通过 fetchRemoteRows() 拉取，
    // 避免一次性把超大表的全部行都载入内存。
    QList<QVariantMap> rows;
};

// ── IComparisonSession：比对会话接口（纯虚，由 ComparisonSession 实现）───────────
//
// 生命周期：createComparisonSession() 创建 → initialize() 初始化 → 反复查询/暂存 →
//          save() 或 discard() 收尾 → 析构。会话内部在 initialize 时会“钉住”一个
//          本地读快照并打开 inbox 门控，故应尽快收尾，不要长期持有。
//
// 线程模型：本接口非线程安全，约定在单一调用线程上使用（写回经由 SyncWorker 串行化）。
class DBRIDGE_EXPORT IComparisonSession {
   public:
    virtual ~IComparisonSession() = default;

    // C-10 fix（C-10 修复）：用对端快照数据初始化本会话。
    //   做什么：吸收 remoteSnapshots（对端各表的 Meta + 行集合），立即算出表级差异，
    //           并对被比对的表打开 InboundTableGate（门控），让 inbox 里针对这些表的
    //           入站变更先被搁置，避免“比对进行中底层数据又被改”导致结果错乱。
    //   为什么必须先调用：tableDiffs() / rowDiffs() 依赖初始化时建立的内部缓存与
    //           读事务快照；未初始化直接查询会得到空结果。
    //   参数：remoteSnapshots —— 对端各表快照（local 端是本地库，本参数即 remote 端）。
    //         err —— 出参，失败时写入错误描述（可为 nullptr 表示不关心原因）。
    //   返回：成功 true；失败 false（如无法开启本地读事务、读 data_version 失败）。
    //   副作用：开启本地只读事务钉住快照、打开门控；故只能调用一次。
    virtual bool initialize(const QList<RemoteTableSnapshot>& remoteSnapshots,
                            QString* err = nullptr) = 0;

    // 表级差异概览：返回每张参与比对的表的 TableDiff（Identical/Different/OnlyLocal/
    // OnlyRemote 及增删改计数）。结果在 initialize() 时已算好，此处只是返回缓存。
    virtual QList<TableDiff> tableDiffs() const = 0;

    // 行级差异：对指定表做“以主键为 key”的行集合比较，分页返回 RowDiff 列表。
    //   参数：table —— 表名；offset/limit —— 分页窗口（limit<0 表示不限、取到末尾）。
    //   注意：本地与对端会被切到同一个 [offset, offset+limit) 窗口后再比对。
    virtual QList<RowDiff> rowDiffs(const QString& table, int offset = 0, int limit = -1) const = 0;

    // 暂存“采用对端某一行”：把对端该 PK 的整行放进内存暂存区（StagingBuffer），
    // 待 save() 时写回本地。对端无此行则返回 false。
    virtual bool stageRow(const QString& table, const QString& pk) = 0;

    // 暂存“采用对端整张表的全部差异行”：对该表所有非 Same 的行逐一 stageRow。
    virtual bool stageTable(const QString& table) = 0;

    // 取消暂存：把某 (表,主键) 从暂存区移除（撤销之前的采纳决定）。
    virtual bool unstage(const QString& table, const QString& pk) = 0;

    // 采用本地（保留本地值）：语义上等价于“对该行不做任何采纳”，故实现为 unstage——
    // 移除该行的暂存项，save() 时便不会用对端值覆盖本地。
    virtual bool acceptLocal(const QString& table, const QString& pk) = 0;

    // 采用对端（用对端值覆盖本地）：等价于 stageRow——把对端整行暂存待写回。
    virtual bool acceptRemote(const QString& table, const QString& pk) = 0;

    // 暂存“单元格级”决策：只把某一行的某一列改成指定 value（其余列维持已暂存/本地值）。
    //   为什么单独存在：支持比 acceptLocal/acceptRemote 更细的手工合并——逐列挑值。
    //   累加语义见实现：多次 stageCell 会在“已暂存版本”上叠加，不会互相覆盖（C-4 修复）。
    virtual bool stageCell(const QString& table, const QString& pk, const QString& column,
                           const QVariant& value) = 0;

    // 惰性拉取对端行（用于对端快照行集合是惰性加载、或需要 keyset 分页时）。
    //   参数：keysetPageToken —— 分页游标（实现里编码为“起始下标”提示）；
    //         pageSize —— 每页行数（<=0 取到末尾）；snapshotId —— 快照标识（当前实现未用）。
    //   返回：以“对端视角”构造的 RowDiff 列表（每行 kind=Added、只填 remote 侧）。
    virtual QList<RowDiff> fetchRemoteRows(const QString& table, const QString& keysetPageToken,
                                           int pageSize, const QString& snapshotId) const = 0;

    // 提交暂存：把内存暂存区里的所有采纳决策一次性写回本地库。
    //   关键：写回前会做 stale（过期）检测——若本地库在初始化后被其它写入改动过
    //         （PRAGMA data_version 变了），则判定暂存已过期、拒绝写入并返回 false。
    //   写回路径优先经 SyncWorker 的“捕获式写”，使改动被 session 捕获并广播给 peer。
    //   收尾：无论成败都会释放读事务与门控（见实现）。
    virtual bool save(QString* err = nullptr) = 0;

    // 放弃：清空暂存、不写回任何东西，并释放读事务与门控。
    virtual void discard() = 0;
};

// ── 工厂：基于某个已配置的同步库创建一个比对会话 ─────────────────────────────
//   做什么：从 SyncContextRegistry 找到该库已初始化的同步上下文，打开一条只读连接
//           供 diff 查询使用，组装好全部依赖（TableStateStore/DiffEngine/门控/
//           UpsertExecutor）并返回一个自管理依赖生命周期的会话。
//   返回：成功返回独占所有权的 unique_ptr；失败返回 nullptr 并设置 *err
//         （如该库尚未初始化同步上下文、只读连接打不开）。
DBRIDGE_EXPORT std::unique_ptr<IComparisonSession> createComparisonSession(const SyncConfig& config,
                                                                           QString* err = nullptr);

}  // namespace dbridge::sync
