#pragma once
#include <QHash>
#include <QSqlDatabase>
#include <QString>
#include <QUuid>

#include "profile/AutoProfileBuilder.h"
#include "profile/ProfileLoader.h"
#include "profile/ProfileSpec.h"
#include "schema/SchemaCatalog.h"
#include "schema/SchemaIntrospector.h"
#include "service/ExportService.h"
#include "service/ImportService.h"
#include <atomic>

// ============================================================================
// DataBridgePrivate.h — DataBridge 门面的「私有实现体」（pimpl 的 Impl 部分）
// ============================================================================
//
// 【这个类是什么】
//   它承载 DataBridge 的全部成员数据与子服务对象。公共头 DataBridge.h 出于「编译
//   防火墙」考量只持有一个不透明指针 d_，真正的字段都集中在这里。这是 pimpl 惯用法
//   的实现侧：本头只在库内部（.cpp）被 #include，对最终用户完全不可见。
//
// 【它聚合了哪些东西】
//   · 一条 QSqlDatabase 连接（db_）及其唯一命名（connName_）、打开标志（dbOpen_）；
//   · 一份 schema 目录（catalog_）与按名索引的 Profile 表（profiles_）；
//   · 三个「无状态、可复用」的子服务：导入、导出、自动建 Profile；
//   · 同步门控所需的两个字段（syncActive_ / syncTables_）。
//   DataBridge 的每个公共方法基本都是「转发到这里的字段/方法上」。
// ============================================================================

namespace dbridge::detail {

class DataBridgePrivate {
   public:
    // 构造：为这条连接生成全局唯一的命名（"dbridge_{UUID}"）。
    // 为何要唯一名：Qt 的 QSqlDatabase 连接以「名字」为键全局登记；多个 DataBridge
    // 实例若重名会互相冲突/覆盖，故用 UUID 保证彼此隔离。此刻并未真正打开数据库。
    DataBridgePrivate() : connName_(QStringLiteral("dbridge_") + QUuid::createUuid().toString()) {
    }

    // 析构：确保连接被关闭并从 Qt 连接表移除（RAII，避免泄漏命名连接）。
    ~DataBridgePrivate() {
        closeDb();
    }

    // openDb —— 实际打开数据库、校验版本、设 PRAGMA、解析 main 库真实路径（见 .cpp）。
    bool openDb(const ConnectionSpec& spec, QString* err);
    // closeDb —— 关闭并 removeDatabase；注意先释放 db_ 引用再移除（见 .cpp 注释）。幂等。
    void closeDb();

    // loadProfileDoc —— 把已解析的 JSON 文档转成 ProfileSpec、（库已打开则）做校验、存入
    // profiles_。
    bool loadProfileDoc(const QJsonDocument& doc, QString* err);
    // refreshCatalog —— 用 SchemaIntrospector 重新自省整库 schema 覆写 catalog_。
    bool refreshCatalog(QString* err);

    QSqlDatabase db_;      // 本门面独占的 SQLite 连接句柄
    QString connName_;     // 该连接在 Qt 连接表中的唯一名（见构造）
    bool dbOpen_ = false;  // 连接是否处于打开态（各公共方法据此做前置检查）
    QString resolvedDbPath_;  // H-02 fix: 经 PRAGMA database_list 解析出的 main 库规范路径
    // L-01 fix: 用 atomic，避免「SyncEngine 初始化置位」与「importExcel 读取」之间的数据竞争。
    std::atomic<bool> syncActive_{false};
    QStringList syncTables_;  // M-02: 被同步监控的表清单；为空表示无信息（保守起见门控全部）
    SchemaCatalog catalog_;  // 当前数据库的 schema 目录（表/列/索引/外键）
    QHash<QString, ProfileSpec> profiles_;  // 已载入的 Profile，按 Profile.name 索引

    // 子服务：无状态（不持有跨调用的可变状态），可被多次复用，故作为成员长期持有。
    ImportService importSvc_;         // 执行 Excel→DB 导入
    ExportService exportSvc_;         // 执行 DB→Excel 导出
    AutoProfileBuilder autoBuilder_;  // 由表结构自动推导 Profile
};

}  // namespace dbridge::detail
