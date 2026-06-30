#pragma once
#include "dbridge/Types.h"
#include "dbridge/sync/SyncTypes.h"

#include "CapturedWriteTemplate.h"
#include "UpsertExecutor.h"

// ============================================================================
// SelectionPushApplier.h — 「选择性推送」的接收端应用器（薄适配层）
// ============================================================================
//
// 【什么是 SelectionPush（选择性推送）】
//   与「增量 changeset 同步」（全量捕获本地所有变更再传播）不同，选择性推送是
//   「定向投递一组指定的行」：发送端冻结一个记录集（连同其外键闭包），切成若干 chunk
//   （分块）逐块发出；接收端就是本类——把每个 chunk 应用到本地库。
//   典型场景：把某张主表的若干条记录连带其依赖行，精准推给特定边缘节点。
//
// 【这个类的定位：薄适配器，不含业务逻辑】
//   真正的写入、捕获、冲突处理全部在 CapturedWriteTemplate（「三分支写模板」）里完成。
//   本类只做一件事：把入参打包成一个 WriteParams（kind = InboundSelectionPush，即模板的
//   「分支 B」），调用 tpl_.execute()，再把结果翻译回 (bool + *err)。
//   之所以单独成类，是为了给上层一个「语义清晰、签名稳定」的选择性推送入口，而不必
//   直接面对模板那个通吃三种写法的宽接口。
//
// 【为什么这里几乎没有冲突仲裁代码】
//   选择性推送的行最终也会经过模板内部的 capture/apply 路径，行级胜负仍由
//   RowWinnerStore / changeset 逻辑统一裁定（见 CapturedWriteTemplate::branchBC）。
//   故本层不重复实现仲裁，per-row 约束错误也不在此层冒泡为 RowError（见 .cpp 说明）。
//
// 【协作者】
//   · CapturedWriteTemplate —— 实际执行写入的「三分支写模板」（分支 B = 入站选择性推送）。
//   · UpsertExecutor        —— 直接 UPSERT 执行器；本类持有引用以备「绕过模板」的调用者，
//                              但常规路径并不直接用它（模板自行管理其变更落库路径）。
// ============================================================================

namespace dbridge::sync {

// SelectionPushApplier —— 经由 CapturedWriteTemplate（分支 B）应用入站选择性推送的「一个 chunk」。
class SelectionPushApplier {
   public:
    // 构造：注入两个协作者引用（均不取得所有权，生命周期由调用方保证长于本对象）。
    SelectionPushApplier(CapturedWriteTemplate& tpl, UpsertExecutor& upsert);

    // 应用一个 chunk（分块）。内部组装 WriteParams 后调用 tpl_.execute()，kind 固定为
    // WriteKind::InboundSelectionPush（模板分支 B）。
    //
    // 参数（多为推送元数据，原样透传给写模板，用于 anti-echo / 冲突仲裁 / 进度对账）：
    //   wconn      —— 写连接；实际由模板持有，这里仅为 API 对称/未来按调用覆盖而保留（见 .cpp）。
    //   rows       —— 本 chunk 携带的行变更集合（每条含表名、列、值、主键列、UPSERT 模式等）。
    //   origin     —— 来源节点标识（originId）。
    //   epoch      —— 来源的
    //   stream_epoch（流纪元）：纪元变化意味着对端做过基线重置，用于丢弃陈旧流。 seq        ——
    //   该来源流中的序号（用于顺序/空洞检测与仲裁）。 schemaVer/schemaFp ——
    //   发送端的表结构版本号与指纹：与本地不一致则拒绝应用（防结构错位）。 originRank ——
    //   来源等级，参与行级冲突仲裁（高 rank 胜）。 pushId     —— 本次选择性推送的唯一标识；chunkSeq
    //   —— 该 push 内的分块序号。 syncTables —— 允许同步的表白名单（空 = 全部接受，仅测试用）。
    //   errors     —— 行级错误回收列表（可空）；本层成功时不向其追加（见 .cpp 末尾说明）。
    //   err        —— 失败时写入聚合错误文本（优先用模板的 errorMsg，退化用 errorCode）。
    // 返回：模板执行成功 true；失败 false 并填 *err。
    // 副作用：在模板内部的写事务中落库（含变更捕获、胜者更新等）。
    bool applyChunk(QSqlDatabase& wconn, const QList<RowMutation>& rows, const QString& origin,
                    qint64 epoch, qint64 seq, qint64 schemaVer, const QString& schemaFp,
                    int originRank, const QString& pushId, int chunkSeq,
                    const QStringList& syncTables, QList<dbridge::RowError>* errors, QString* err);

   private:
    CapturedWriteTemplate& tpl_;  // 实际执行写入的三分支写模板（引用，不拥有）
    UpsertExecutor& upsert_;  // 直写 UPSERT 执行器（引用，不拥有；常规路径未直接使用）
};

}  // namespace dbridge::sync
