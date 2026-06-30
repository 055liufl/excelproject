#pragma once
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

#include "../WriteTxn.h"
#include "ChangelogStore.h"
#include <sqlite3.h>

// ============================================================================
// SessionRecorder.h — 用 SQLite session 扩展捕获“一笔本地写事务”的行级变更
// ============================================================================
//
// 【它在同步管线中的位置（变更捕获的起点）】
//   本地业务写库 → SessionRecorder 借助 session 钩子录下这批改动（changeset）
//   → 封存进 __sync_changelog（ChangelogStore）→ 后续被打包、广播给各对端。
//   也就是说：本类是“本地改动如何被记录下来”的第一环。
//
// 【与一次写事务的生命周期严格对齐（用法）】
//   WriteTxn txn(db); txn.begin();   // ① 开启写事务（BEGIN IMMEDIATE）
//   recorder.begin(h, tables);       // ② 建会话并附表（必须在业务写之前！）
//   // ... 执行业务 INSERT/UPDATE/DELETE ...  ← session 在后台默默记录每一行变更
//   recorder.sealInto(h, store, ...);// ③ 收集 changeset → 写 changelog → 拆会话
//   txn.commit();                    // ④ 提交事务（changelog 与业务改动同事务原子落地）
//
//   关键时序：begin() 必须在任何业务写之前调用——否则在 attach 之前发生的改动不会
//   被纳入 changeset；sealInto() 必须在“同一个 WriteTxn 内”调用，以保证 changelog
//   记录与被记录的业务改动一起提交或一起回滚（不会出现“改了库但没记 changelog”的撕裂）。
//
// 【依赖】需要 SQLITE_ENABLE_SESSION + SQLITE_ENABLE_PREUPDATE_HOOK
//   （能力探测见 SqliteHandle）。
// ============================================================================

namespace dbridge::sync {

// 针对一组同步表，录制一个“短命” sqlite3_session（生命周期 = 一笔写事务）的变更录制器。
class SessionRecorder {
   public:
    SessionRecorder() = default;
    // 析构即兜底回收：若上层因异常/提前 return 漏调 sealInto/abort，析构时也会拆掉会话。
    ~SessionRecorder() {
        abort();
    }

    // 不可拷贝：内部持有一个裸的 sqlite3_session* 资源，拷贝会导致重复释放。
    SessionRecorder(const SessionRecorder&) = delete;
    SessionRecorder& operator=(const SessionRecorder&) = delete;

    // 创建一个新会话，并把 syncTables 中的所有同步表附着进去，开始捕获变更。
    // 【时序要求】必须在 WriteTxn::begin() 之后、任何业务写之前调用（见文件头）。
    // 【参数】h —— 原生句柄；syncTables —— 要纳入捕获的表名；err —— 失败原因。
    // 【返回】成功 true；若已 active 或句柄为空或 attach 失败则 false。
    // 【副作用】持有一个 sqlite3_session*（由 sealInto/abort/析构 负责释放）。
    bool begin(sqlite3* h, const QStringList& syncTables, QString* err);

    // 收集会话产生的 changeset，写入 changelog，然后拆除会话（“封存”）。
    // 【必须】在与 begin() 相同的那个 WriteTxn 内调用，确保 changelog 写入与业务改动同事务原子化。
    // 【成功语义】*outLocalSeq 返回 changelog 为本条记录分配的 local_seq；
    //   若本事务实际未捕获到任何改动（空 changeset），仍视为成功并返回 *outLocalSeq=0
    //   （表示“没有可广播的变更”，而不是错误）。
    // 【参数】origin/epoch/schemaVer/schemaFp/parentSeq/originSeq —— 写入 changelog 的同步元信息
    //   （来源节点、流纪元、表结构版本与指纹、父序列、来源序列）；其余见下。
    // H-01 fix：pushId 透传给 ChangelogStore::append，使“选择性推送”的 changeset 记录下其 push_id，
    //   从而让广播屏障（broadcast barrier）只阻塞这一次特定推送，而非该 origin 的所有条目。
    // M-01 fix：可选的 *outChangeset 回带原始 changeset 字节，便于调用方据此做“增量”的
    //   TableMutation 更新，而不必走代价高昂的全量 resetFromBaseline() 重扫。
    bool sealInto(sqlite3* h, ChangelogStore& store, QSqlDatabase& db, WriteTxn& txn,
                  const QString& origin, qint64 epoch, qint64 schemaVer, const QString& schemaFp,
                  qint64 parentSeq, qint64 originSeq, qint64* outLocalSeq, QString* err,
                  const QString& pushId = QString(), QByteArray* outChangeset = nullptr);

    // 拆除并丢弃会话，不写入任何 changelog（用于回滚/放弃路径，以及析构兜底）。
    void abort();

    // 当前是否持有一个活动会话（begin 成功且尚未 seal/abort）。
    bool isActive() const {
        return session_ != nullptr;
    }

   private:
    sqlite3_session* session_ = nullptr;  // 当前会话句柄；nullptr 表示无活动会话

    // 从会话中提取 changeset 字节到 QByteArray。
    // 【返回约定（H-06 fix）】
    //   · 出错        → 返回 null QByteArray（用 isNull() 判定）；
    //   · 无变更      → 返回 非 null 的空 QByteArray（用 isEmpty() 判定快速路径）；
    //   · 有变更      → 返回含变更字节的 QByteArray。
    QByteArray collectChangeset(QString* err);
};

}  // namespace dbridge::sync
