#pragma once

// ============================================================================
// Scenario2SnapshotService.h — 场景2「中心A 快照服务」+ 快照编解码
// ============================================================================
//
// 【为什么存在（对应需求 #2）】
//   场景2 是"子节点B ⇄ 中心A"的差异比对。原实现里 B 直接读取 center_A.db 文件来拿 A 的
//   数据——这在真实场景不成立（B 不可能持有 A 的库文件）。本文件把"A 的数据"改为**经 UDP
//   向 A 请求、由 A 读自己的库后回传**：
//     · B 侧：向自己的 outbox 写一个"快照请求"工件（snapreq__<reqId>）；场景1 同款
//       UdpFileTransport 把它经 UDP 送到 A 的 inbox。
//     · A 侧：CenterSnapshotResponder（本文件，常驻后台线程）监视 A 的 inbox，收到请求后
//       用 dbridge 公共接口（DataBridge::userTables/describeTable，对应需求 #3）发现 A 的
//       表与字段、SELECT 读各表行，序列化成"快照响应"工件（snapresp__<reqId>）写回 A 的
//       outbox；UdpFileTransport 再经 UDP 送回 B 的 inbox；B 反序列化后喂给比对会话。
//
//   如此，比对所需的中心A 数据**全程走 UDP 文件传输层**，B 不再触碰 A 的库文件。
//
// 【编解码格式（QDataStream，Qt_5_12 版本）】
//   [magic:u32][version:u32][tableCount:u32]
//   repeat: [table:QString][schemaFingerprint:QString][contentChecksum:QString]
//           [rowCount:i64][rowCount2:u32] repeat:[row:QVariantMap]
//   与 dbridge::sync::RemoteTableSnapshot 一一对应（meta + rows）。
// ============================================================================

#include "dbridge/sync/IComparisonSession.h"  // RemoteTableSnapshot

#include <QAtomicInt>
#include <QByteArray>
#include <QList>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QThread>

namespace s2snap {

// encodeSnapshots —— 把一组 RemoteTableSnapshot 序列化为字节流（供写成 UDP 工件）。
QByteArray encodeSnapshots(const QList<dbridge::sync::RemoteTableSnapshot>& snaps);

// decodeSnapshots —— 反序列化字节流为 RemoteTableSnapshot 列表（B 收到 A 的响应后调用）。
//   格式非法/版本不符时返回空列表。
QList<dbridge::sync::RemoteTableSnapshot> decodeSnapshots(const QByteArray& bytes);

}  // namespace s2snap

// ── CenterSnapshotResponder —— 中心A 的"快照响应"后台线程 ─────────────────────
//
// 监视 A 的 inbox 目录里的快照请求工件（snapreq__<reqId>.payload + .ready），收到后：
//   ① 用 DataBridge 公共接口发现 A 的用户表与每表字段/主键；
//   ② 逐表 SELECT * 读行（按主键排序，保证两端行序可对齐）+ 计算内容校验和；
//   ③ 序列化为快照响应工件（snapresp__<reqId>.payload + .ready）写入 A 的 outbox。
// 线程模型仿 UdpFileTransport：run() 在后台线程轮询；requestStop() 置原子标志、wait() 等退出。
// A 在场景2 中不跑同步引擎，仅作"数据源 + 快照响应服务"。
class CenterSnapshotResponder : public QThread {
    Q_OBJECT
   public:
    // centerDbPath：A 的 SQLite 库路径；inboxDir/outboxDir：A 的收发目录（与 UDP 传输层约定一致）。
    CenterSnapshotResponder(const QString& centerDbPath, const QString& inboxDir,
                            const QString& outboxDir, QObject* parent = nullptr);

    // 线程安全停止：置原子标志后立即返回；之后须 wait()。
    void requestStop();

    // takeLog —— 线程安全地取走并清空「A 侧处理日志」缓冲（子节点B 收到响应后调用，按序输出）。
    //   为什么不用 Qt 信号：B 侧等待响应时会阻塞主线程事件循环，信号会滞后/乱序；改由 B 在收到
    //   响应后主动"抽干"缓冲，保证时序恒为 ①B发送→②A处理→③B比对，且全部在主线程输出。
    QStringList takeLog();

   protected:
    void run() override;

   private:
    static constexpr int kPollMs = 40;  // inbox 轮询间隔（毫秒）

    // 线程安全地追加一条 A 侧处理日志（run() 线程调用）。
    void pushLog(const QString& line);

    QString centerDbPath_;
    QString inboxDir_;
    QString outboxDir_;
    QAtomicInt stop_;
    QMutex logMutex_;         // 保护 pendingLog_（run 线程写、主线程 takeLog 读）
    QStringList pendingLog_;  // A 侧处理日志缓冲，待 B 抽干
};
