#pragma once
#include <QByteArray>
#include <QString>

// ============================================================================
// OutboxWriter.h — 把同步「artifact」原子地发布到 outbox 目录的发送端
// ============================================================================
//
// 【在同步管线中的位置】
//   同步子系统不直接走 socket，而是把每一笔要发送的东西（changeset 变更包、
//   选择性推送分片、基线请求/响应、ACK 确认）序列化成一个「artifact 文件」，
//   写入本节点的 outbox 目录。随后由一个独立的「文件搬运层」（示例实现是 UDP，
//   也可以是 rsync/共享盘/任意第三方搬运器）把 outbox 里的文件搬到对端的 inbox
//   目录。对端的 InboxWatcher 再扫描 inbox 把文件取走处理。
//   OutboxWriter 就是这条链路最前端的「写文件」一环——只负责安全落盘，不关心
//   文件最终怎么被搬走，也不关心对端是谁（路由信息已编码进 artifact 文件名，
//   见 SyncDDL.h 的命名契约）。
//
// 【为什么要「原子发布」：发布协议（publish protocol）】
//   搬运层会异步地、并发地扫描 outbox 目录。如果 OutboxWriter 直接以最终文件名
//   边写边落盘，搬运层可能在文件「只写了一半」时就把它读走/搬走，导致对端收到
//   损坏的半截 artifact。为杜绝这种竞态，采用经典的「先写临时名 + 原子改名 +
//   就绪标记」三步发布协议：
//       1) 写入 finalName.tmp（搬运层只认最终名与 .ready，不会碰 .tmp）；
//       2) flush + fsync 把数据真正刷到磁盘（保证掉电后不丢）；
//       3) rename(.tmp → finalName)：POSIX rename 是原子的，要么完整出现要么不
//          出现，绝无「半截可见」的中间态；
//       4) 再写一个空的 finalName.ready 标记文件（哨兵），告诉搬运层/接收方
//          「这个 artifact 已经完整就位、可以搬运/消费了」；
//       5) fsync 容纳目录本身，让 rename 与 .ready 的创建在掉电后也能存活。
//   接收方/搬运层的约定：只有当 .ready 出现时，对应的同名主文件才算「数据齐全」。
//
// 【线程模型】
//   本类无内部可变共享状态（dir_ 构造后只读），write/writeAck 可被不同线程对
//   不同 artifactName 并发调用；但调用方需保证不会用同一个 finalName 并发写
//   （文件名由 SyncDDL 的命名 helper 加 UUID 后缀保证全局唯一，正常不会撞名）。
//
// 【协作者】
//   · SyncDDL.h（dbridge::sync::ddl）—— 生成 artifactName / ackName 的命名 helper；
//   · AckChannel —— 通过本类的 writeAck() 批量发布 ACK；
//   · InboxWatcher（对端）—— 扫描 .ready、消费主文件；
//   · 第三方文件搬运器 —— 把 outbox 文件搬到对端 inbox。
// ============================================================================

namespace dbridge::sync {

// OutboxWriter —— 原子地把 artifact 发布到某个 outbox 目录。
// 发布协议：写 .tmp → fsync → rename 成 .payload 或 .ack → 写 .ready 标记。
class OutboxWriter {
   public:
    // 绑定到一个 outbox 目录。目录不存在时会在首次写入时按需创建（mkpath）。
    explicit OutboxWriter(const QString& outboxDir);

    // 写一个 payload artifact（变更包/推送分片/基线等）。
    // artifactName 应来自 SyncDDL 的命名 helper（已编码 origin/epoch/kind/seq/
    // 目标 peer/UUID，决定了对端如何路由解析），调用方不要自己拼名字。
    // 返回 true 表示已原子发布成功；失败时（若 err 非空）写入诊断信息。
    bool write(const QString& artifactName, const QByteArray& data, QString* err);

    // 写一个 ACK artifact（确认包，文件名以 .ack 结尾，由 ddl::ackArtifactName 生成）。
    // 与 write() 走完全相同的原子发布协议，仅语义上区分主载荷与确认。
    bool writeAck(const QString& ackName, const QByteArray& data, QString* err);

   private:
    // 共享实现：写 tmp → flush+fsync → 原子 rename → 写 .ready → fsync 目录。
    // write() 与 writeAck() 都委托到这里；finalName 决定最终文件名（含扩展名）。
    bool writeAtomic(const QString& finalName, const QByteArray& data, QString* err);

    QString dir_;  // 目标 outbox 目录的路径（构造后只读）
};

}  // namespace dbridge::sync
