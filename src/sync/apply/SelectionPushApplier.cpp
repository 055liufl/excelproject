#include "SelectionPushApplier.h"

// ============================================================================
// SelectionPushApplier.cpp — 选择性推送应用器实现（参数打包 + 结果翻译）
// ============================================================================
//
// 本文件极薄：核心只是把 applyChunk() 的入参搬进 WriteParams，交给写模板执行，
// 再把模板返回的 WriteResult 折叠成 (bool, *err)。所有「真正的活」——变更捕获、
// 冲突仲裁、胜者更新、原子事务——都在 CapturedWriteTemplate 内部完成。
// 详尽的设计背景见同名头文件的文件头注释。
// ============================================================================

namespace dbridge::sync {

// ---------------------------------------------------------------------------
// constructor —— 注入协作者引用
// ---------------------------------------------------------------------------

// 构造函数：保存两个协作者的引用（成员初始化列表，零拷贝）。
// 二者均不取得所有权，调用方须保证其生命周期长于本应用器。
SelectionPushApplier::SelectionPushApplier(CapturedWriteTemplate& tpl, UpsertExecutor& upsert)
    : tpl_(tpl), upsert_(upsert) {
}

// ---------------------------------------------------------------------------
// public —— 应用一个 chunk
// ---------------------------------------------------------------------------

// applyChunk()：把一个入站推送分块交给写模板（分支 B）落库。
// 流程：① 标注两个本层用不到的参数；② 组装 WriteParams；③ execute()；④ 翻译结果。
// 错误模式：模板失败 → 返回 false 并填 *err；本层自身不产生错误。
// 线程/事务：写入发生在模板内部的写事务中；须在持有写连接的同一线程调用。
bool SelectionPushApplier::applyChunk(QSqlDatabase& wconn, const QList<RowMutation>& rows,
                                      const QString& origin, qint64 epoch, qint64 seq,
                                      qint64 schemaVer, const QString& schemaFp, int originRank,
                                      const QString& pushId, int chunkSeq,
                                      const QStringList& syncTables,
                                      QList<dbridge::RowError>* errors, QString* err) {
    // wconn 实际由 CapturedWriteTemplate 持有并使用；此处入参仅为「API 对称」以及未来
    // 可能的「按调用覆盖连接」预留，本层不直接触碰它，故显式标注未使用以消除编译告警。
    Q_UNUSED(wconn)  // wconn is owned by the CapturedWriteTemplate; passed for
                     // future API symmetry and potential per-call overrides.
    // upsert_ 是为「绕过模板、直接 UPSERT」的调用者持有的；常规选择性推送路径走模板，
    // 模板自行管理变更落库，故这里不使用 upsert_。
    Q_UNUSED(upsert_)  // UpsertExecutor is held for callers that bypass the
                       // template; the template handles its own mutation path.

    // ② 组装写模板入参：kind 固定为「入站选择性推送」（模板分支 B），其余字段原样透传。
    WriteParams params;
    params.kind = WriteKind::InboundSelectionPush;
    params.origin = origin;          // 来源节点标识
    params.epoch = epoch;            // 来源 stream_epoch（流纪元）
    params.seq = seq;                // 来源流序号
    params.schemaVer = schemaVer;    // 发送端表结构版本
    params.schemaFp = schemaFp;      // 发送端表结构指纹
    params.originRank = originRank;  // 来源等级（参与冲突仲裁）
    params.pushId = pushId;          // 本次推送 ID
    params.chunkSeq = chunkSeq;      // 分块序号
    params.mutations = rows;         // 本 chunk 的行变更集合
    params.syncTables = syncTables;  // 同步表白名单

    // ③ 交给写模板执行：模板内部完成「校验→应用→捕获→更新胜者」整条链路（原子事务）。
    WriteResult result = tpl_.execute(params);

    // ④ 翻译结果：失败时优先取人类可读的 errorMsg，为空则退化用机器码 errorCode。
    if (!result.ok) {
        if (err) {
            *err = result.errorMsg.isEmpty() ? result.errorCode : result.errorMsg;
        }
        return false;
    }

    // The template executes all mutations atomically; per-row constraint errors
    // surfaced by branchBC are stored via RowWinnerStore / changeset logic and
    // do not produce RowError entries at this layer.  If the caller supplied an
    // errors list we leave it untouched on success.
    // —— 模板对所有 mutation 做「原子」执行；分支 B/C 暴露的 per-row 约束错误由
    //    RowWinnerStore / changeset 逻辑在更底层消化，不会在本层冒泡成 RowError。
    //    因此即便调用方传了 errors 列表，本层成功路径也原样不动它（故显式标注未使用）。
    Q_UNUSED(errors)

    return true;
}

}  // namespace dbridge::sync
