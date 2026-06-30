#pragma once
#include <QList>
#include <QSet>
#include <QSqlDatabase>
#include <QString>

#include "../../schema/SchemaCatalog.h"
#include "ConsistencyCache.h"
#include "SelectionResolver.h"

// ============================================================================
// FkClosureBuilder.h — 选择性推送第 2 阶段：补齐「外键依赖闭包」并排出拓扑序
// ============================================================================
//
// 【职责一句话】
//   输入是「用户精确选中的若干整行」（已由 SelectionResolver 物化为真实数据），
//   输出是「这些行 + 它们沿外键向上追溯到的全部父行」，且按拓扑序排好——保证父表
//   永远排在子表之前，使对端可以按顺序逐行 UPSERT 而不会触发外键约束失败。
//
// 【在选择性推送链路中的位置】
//   SyncSelection（意图）
//     → SelectionResolver（把 表+主键 → 整行 QVariantMap）
//     → ★FkClosureBuilder★（本文件：补 FK 闭包 + 拓扑排序 → 闭包清单 manifest）
//     → FrozenManifest（冻结清单与指纹，防漂移）
//     → ChunkStreamer（按字节预算切片成多个 SelectionPush artifact）
//
// 【为什么必须补外键闭包】
//   用户可能只勾选了一张子表的几行（例如几条订单明细），但这些行通过外键指向了
//   父表（订单、客户……）。如果只推子行、不推它们依赖的父行，对端在 apply 时会因
//   「父行不存在」而违反外键约束（E_SYNC_APPLY_FK）。所以推送前必须沿 FK 向父表
//   方向递归补全所有被引用的父行，构成一个「自洽」的可独立应用的数据子集（闭包）。
//
// 【两条关键不变量】
//   ① 闭包完整性：选择集中任一行引用到的父行，要么也在闭包里，要么已被一致性剪枝
//      （ConsistencyCache 判定对端已有相同副本）。否则报 E_SYNC_FK_CLOSURE_MISSING。
//   ② 拓扑可应用性：FK 图必须无环，才能排出「父先子后」的线性序；存在环则无法定序，
//      报 E_SYNC_FK_CYCLE_UNSUPPORTED。
//
// 【协作者】
//   · SchemaCatalog/TableInfo/FkInfo（src/schema/SchemaCatalog.h）：提供 FK 图——
//     每张表声明了哪些列（fromColumn）指向哪张父表（refTable）的哪列（toColumn）。
//   · ConsistencyCache：缓存「与对端中心已一致的父行指纹」，用于一致性剪枝，
//     避免把对端早已拥有的父行重复塞进闭包（减小推送体积）。
//   · SelectionResolver::ResolveResult：本阶段的输入（被选中的整行）。
//   · 错误码见 include/dbridge/Errors.h（E_SYNC_FK_CLOSURE_MISSING /
//     E_SYNC_FK_CYCLE_UNSUPPORTED / E_SYNC_SELECTION_TOO_LARGE）。
//
// 注释风格参照 Errors.h / SyncTypes.h：`// ──` 分节、中文、信息密集。
// ============================================================================

namespace dbridge::sync {

// Computes the transitive FK closure of a selection and produces a
// topologically sorted manifest.
//   （计算一个选择集的「传递闭包」（沿外键递归追溯到的全部父行），
//    并产出一份按拓扑序排好的清单 manifest。）
class FkClosureBuilder {
   public:
    // Entry —— 闭包清单中的一行（既可能是用户选中的行，也可能是被带入的父依赖行）。
    struct Entry {
        QString table;  // 行所属表名
        QString pk;     // 行主键值（统一以字符串承载，与 SelectionResolver 一致）
        QVariantMap row;  // 整行数据：列名 → 值（FK 列的值就是追溯父行的线索）
        bool isSelected = false;  // true = directly selected, false = FK dependency
                                  //   （true=用户直接选中；false=因外键依赖被自动带入的父行）
        int topoIndex = 0;  // 拓扑序下标：由 topoSort 填充，越小越先应用（父表更小）
    };

    // ── build —— 顶层入口：由「选中行」出发，构建完整闭包并排序 ─────────────────
    // Build closure from directly-selected rows.
    //   （从「直接选中的行」出发，构建外键闭包。）
    // catalog provides FK graph. cache is used to prune consistent deps.
    //   （catalog 提供 FK 依赖图；cache 用于剪掉「对端已一致」的父依赖行。）
    // maxSize: fail with E_SYNC_SELECTION_TOO_LARGE if exceeded.
    //   （maxSize 是闭包总行数上限，超出即以 E_SYNC_SELECTION_TOO_LARGE 失败。）
    // H-02 fix: includeFkDeps and pruneConsistent honour the SyncSelection flags.
    //   （H-02 修复：includeFkDeps / pruneConsistent 两个开关来自 SyncSelection，
    //    使调用方可分别关闭「补闭包」与「一致性剪枝」。）
    //
    // 做什么：先把选中行去重放入工作表 → （可选）递归补 FK 父行 → 校验上限 → 拓扑排序。
    // 参数：rconn 只读连接（按主键回查父行）；selected 已物化的选中行；catalog FK 图；
    //       cache 一致性缓存；maxSize 行数上限；out 输出闭包清单（已排序）；err 出参错误；
    //       includeFkDeps=true 才补闭包；pruneConsistent=true 才做一致性剪枝。
    // 返回：true 成功（*out 为拓扑有序闭包）；false 失败（*err 含错误码，见上）。
    // 副作用：通过 fetchRow 对 rconn 执行若干只读 SELECT/PRAGMA；填充 *out。
    // 错误模式：父行缺失=E_SYNC_FK_CLOSURE_MISSING；超上限=E_SYNC_SELECTION_TOO_LARGE；
    //           FK 成环=E_SYNC_FK_CYCLE_UNSUPPORTED；SQL 失败=透传驱动文本。
    // 线程：无内部同步，约定在单线程（worker 线程）内使用，连接 rconn 不跨线程共享。
    bool build(QSqlDatabase& rconn, const QList<SelectionResolver::ResolveResult>& selected,
               const dbridge::detail::SchemaCatalog& catalog, ConsistencyCache& cache,
               qint64 maxSize, QList<Entry>* out, QString* err, bool includeFkDeps = true,
               bool pruneConsistent = true);

   private:
    // ── buildClosure —— 递归（实为迭代 BFS）补全外键父行 ──────────────────────
    // Recursively expand FK dependencies into work list.
    //   （把外键依赖（父行）递归地展开、追加进工作表 work。）
    // 做什么：以 work 当前内容为起点做广度优先遍历；对每行的每个 FK 列，取出引用值
    //   去父表回查父行，未见过且未被剪枝则追加进 work（从而其 FK 又会被继续展开）。
    // 参数：seen 去重集合（"表\x1f主键"），防止同一父行被重复抓取，亦天然处理 DAG 复用；
    //       pruneConsistent 控制是否对父行做一致性剪枝。
    // 返回：true 闭包补齐成功；false 出错（父行缺失 / SQL 失败，*err 已写）。
    // 复杂度：O(闭包行数 × 每行 FK 数)，每次回查父行近似走主键索引 O(log n)。
    bool buildClosure(QSqlDatabase& rconn, const dbridge::detail::SchemaCatalog& catalog,
                      ConsistencyCache& cache, bool pruneConsistent, QList<Entry>& work,
                      QSet<QString>& seen, QString* err);

    // ── topoSort —— Kahn 拓扑排序：把闭包排成「父先子后」 ─────────────────────
    // Kahn topological sort; returns E_SYNC_FK_CYCLE_UNSUPPORTED on cycle.
    //   （Kahn 算法拓扑排序；若发现环，返回 E_SYNC_FK_CYCLE_UNSUPPORTED。）
    // 做什么：以 FK 关系建图（父→子有向边），按入度为 0 逐层出队赋 topoIndex，最后据此排序。
    // 返回：true 排序成功（entries 被原地重排）；false=检测到环（无法定序）。
    bool topoSort(QList<Entry>& entries, const dbridge::detail::SchemaCatalog& catalog,
                  QString* err);

    // ── fetchRow —— 按主键从只读连接抓取单行 ─────────────────────────────────
    // Fetch a single row by PK from rconn.
    //   （从只读连接 rconn 按主键取一行。）
    // 返回：true=查询成功执行（命中则 *row 填满；未命中则 *row 被 clear()，由调用方判空）；
    //       false=无主键列 / SQL 执行失败（*err 已写）。注意「查无此行」不算 false。
    bool fetchRow(QSqlDatabase& rconn, const QString& table, const QString& pk, QVariantMap* row,
                  QString* err);

    // ── getPkColumn —— 取某表单列主键列名（带缓存） ──────────────────────────
    // Get PK column name for a table via PRAGMA table_info.
    //   （通过 PRAGMA table_info 取得该表的主键列名。）
    // 结果会写入 pkColCache_，同一表第二次查询命中缓存（含「无主键」也缓存为空串）。
    QString getPkColumn(QSqlDatabase& rconn, const QString& table);

    // 表名 → 单列主键列名 的进程内缓存（值为空串表示「该表无单列主键」，亦已缓存以免重查）。
    QHash<QString, QString> pkColCache_;
};

}  // namespace dbridge::sync
