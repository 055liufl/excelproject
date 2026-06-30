// tests/sync_common.h  — shared helpers for all sync unit tests
// （tests/sync_common.h —— 所有「同步子系统」单元测试共用的测试夹具/辅助函数）
#pragma once
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QUuid>
#include <QtTest>

#include "sync/SyncDDL.h"

// ============================================================================
// sync_common.h — 同步单测的公共「测试夹具」头：开/关一个建好 __sync_* 表的库
// ============================================================================
//
// 【这个文件是什么】
//   同步子系统的每个单测都需要一个「已经建好全部 __sync_* 元数据表」的 SQLite 库做底座
//   （changelog / outbound_ack / applied_vector / baseline 等表，DDL 权威定义在
//   sync/SyncDDL.h 的 allCreateStatements()）。把「开库 + 建表 + 关库 + 错误码断言」这些
//   每个测试都要重复的样板抽到这里，让各测试文件只 #include 一次即可复用，避免到处粘贴。
//
// 【为什么提供两种开库方式】
//   · openSyncDb        —— 开「:memory: 内存库」：最快、用完即焚，适合「单连接内」自洽的测试。
//   · openSyncFileDb    —— 开「磁盘临时文件库」并开 WAL：当一个测试要用「两个连接」同时看同一个
//     库（跨连接可见性，如 A 连接写、B 连接读到没有），内存库做不到（:memory: 每个连接是独立库），
//     必须落到真实文件 + WAL 才能让多连接看见彼此的提交。
//
// 【连接命名约定】每个连接名都拼上一段 UUID，保证「同进程内多个测试并行/接连运行」时连接名
//   绝不撞车（Qt 的 QSqlDatabase 连接名是进程级全局的，重名会冲突）。
//
// 【清理纪律】凡是 open* 出来的连接，调用方都必须在 cleanup() 里调 closeSyncDb() 释放——
//   Qt 要求「removeDatabase 前不得残留该连接的活动句柄」，否则会告警且移除不彻底（内存/句柄泄漏）。
// ============================================================================

// openSyncDb —— 开一个「:memory: 内存 SQLite 库」并建好全部 __sync_* 表。
//   做什么：用唯一连接名打开一个内存库（outDb 出参回填该连接），逐条执行 SyncDDL 的全部建表语句。
//   参数：outDb 出参，回填打开的连接对象。
//   返回：成功返回「连接名」（调用方 cleanup() 时要拿它去 closeSyncDb）；open 失败返回空串。
//   副作用：在进程级连接表里注册一个新连接；调用方负责最终 closeSyncDb 释放。
//   注意：建索引语句在空表上执行是无害的（benign），故这里不检查每条 DDL 的返回值。
inline QString openSyncDb(QSqlDatabase& outDb) {
    // 连接名 = 固定前缀 + 12 位 UUID 片段，确保进程内全局唯一、不与其它测试连接重名。
    const QString name =
        QStringLiteral("tst_sync_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
    outDb = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    outDb.setDatabaseName(QStringLiteral(":memory:"));  // :memory: → 进程内、连接私有、用完即焚
    if (!outDb.open())
        return {};  // 打不开（极少见）：返回空串，调用方据此判失败
    QSqlQuery q(outDb);
    // 逐条执行 SyncDDL 的全部建表/建索引语句，把这张空库变成「具备 __sync_* 元数据表」的库。
    for (const QString& ddl : dbridge::sync::ddl::allCreateStatements())
        q.exec(ddl);  // index creates on empty table are benign
                      // （空表上建索引是无害的，故不逐条校验返回值）
    return name;
}

// openSyncFileDb —— 开一个「磁盘临时文件库」并开 WAL，建好全部 __sync_* 表。
//   何时用：测试需要「多个连接看同一个库」（跨连接可见性）时。:memory: 每连接独立，做不到；
//           磁盘文件 + WAL 才能让一个连接提交的数据被另一个连接读到。
//   参数：outDb 出参回填连接；filePath 临时库文件路径（通常来自 QTemporaryDir）。
//   返回：成功返回连接名；open 失败返回空串。副作用：注册连接 + 在磁盘落一个库文件。
inline QString openSyncFileDb(QSqlDatabase& outDb, const QString& filePath) {
    const QString name = QStringLiteral("tst_sync_file_") +
                         QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
    outDb = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    outDb.setDatabaseName(filePath);
    if (!outDb.open())
        return {};
    QSqlQuery q(outDb);
    // 开 WAL（预写日志）模式：这是「多连接并发读写、且能互相看见提交」的前提；
    // 与生产里同步库的运行模式一致（见 ConnectionSpec::enableWal）。
    q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    for (const QString& ddl : dbridge::sync::ddl::allCreateStatements())
        q.exec(ddl);
    return name;
}

// closeSyncDb —— 与 open* 配对的释放函数：关闭并从全局连接表移除该连接。
//   先判 contains 再操作，对「未注册/已移除」的连接名调用是安全的（幂等）。
//   纪律：必须先 close()、再 removeDatabase()，避免「持活动句柄移除连接」的告警。
inline void closeSyncDb(const QString& connName) {
    if (QSqlDatabase::contains(connName)) {
        QSqlDatabase::database(connName).close();
        QSqlDatabase::removeDatabase(connName);
    }
}

// Assert that call returned false and *err contains the expected code string.
// 译：断言「某调用返回了 false（失败）」且「错误文本里包含期望的错误码字符串」。
//
// ASSERT_ERR_CODE —— 「负路径」断言宏：一行同时校验「确实失败」+「失败原因是预期的那个错误码」。
//   为什么做成宏而非函数：QVERIFY2 等 QtTest 断言要求在测试函数体内直接展开（它们会 return），
//     包成函数就无法在断言失败时让外层测试函数提前返回；用宏可把断言原样注入调用点。
//   参数：ok_val 被测调用的返回值（期望为 false）；err_str 该调用写出的错误文本（QString）；
//         code   期望出现在错误文本中的错误码字符串（如 "E_SYNC_..."）。
//   语义：① 先断言 !ok_val（必须是失败，否则报「期望失败却成功了」）；
//         ② 再断言 err_str 包含 code（否则打印「期望在错误里看到 X，实得 Y」的可读诊断）。
//   do{...}while(false)：宏的经典惯用法，让整个宏体成为「单条语句」，可安全用于 if/else 等上下文。
#define ASSERT_ERR_CODE(ok_val, err_str, code)                                                    \
    do {                                                                                          \
        QVERIFY2(!(ok_val), "Expected failure, got success");                                     \
        QVERIFY2((err_str).contains(QLatin1String(code)),                                         \
                 qPrintable(                                                                      \
                     QString("Expected '%1' in error: %2").arg(QLatin1String(code), (err_str)))); \
    } while (false)
