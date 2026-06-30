#include "dbridge/DataBridge.h"

#include "dbridge/Errors.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include "DataBridgePrivate.h"
#include "profile/ProfileLoader.h"
#include "profile/ProfileValidator.h"
#include "schema/SchemaCatalog.h"
#include "service/ErrorCollector.h"

// ============================================================================
// DataBridge.cpp — dbridge 库「对外门面」（Facade）的实现
// ============================================================================
//
// 【这个文件是什么】
//   DataBridge.h 中声明的公共门面类 DataBridge 的具体实现，外加它的 pimpl 实现体
//   detail::DataBridgePrivate 的若干私有方法（openDb/closeDb/loadProfileDoc/
//   refreshCatalog）。它是用户接触整个 ETL 库的唯一入口的「落地处」。
//
// 【pimpl 转交模式（贯穿全文件）】
//   公共类 DataBridge 自身几乎不持有状态——它只握着一个 unique_ptr<DataBridgePrivate>
//   d_。所有真正的成员数据（QSqlDatabase db_、profiles_ 映射表、SchemaCatalog
//   catalog_、ImportService/ExportService/AutoProfileBuilder 等子服务）都藏在
//   DataBridgePrivate 里（定义见 src/DataBridgePrivate.h）。因此本文件里几乎每个
//   公共方法都是「做点参数检查 → 把活转交给 d_->某子服务」的薄壳。这样做的好处见
//   DataBridge.h 顶部「pimpl 惯用法」一节（编译防火墙、下游免重编译）。
//
// 【本文件在 ETL 管线中的位置】
//   它是「门面 + 编排」层：本身不读 Excel、不拼 SQL、不做校验，而是按正确顺序把这些
//   职责派发给下层（ProfileLoader 解析配置、SchemaIntrospector 自省、ProfileValidator
//   校验、ImportService/ExportService 执行搬运）。
//
// 【协作者一览】
//   · DataBridgePrivate（同库）——藏所有成员与子服务的实现体。
//   · ProfileLoader / ProfileValidator —— 解析与校验 Profile（JSON↔ProfileSpec）。
//   · SchemaIntrospector / SchemaCatalog —— 自省数据库 schema 并缓存为目录。
//   · ImportService / ExportService —— 真正执行导入/导出搬运。
//   · ErrorCollector —— 收集校验阶段的多条 RowError。
//   · 同步层（sync）——通过 setSyncActive / runImportOnDb / snapshotCatalog 等
//     「供同步层使用」的方法与本门面交互（带 I-04 / J-09 等修复标记）。
//
// 【贯穿全文件的错误约定】
//   绝大多数方法用 `QString* err`（可为 nullptr）回报失败原因，返回 bool/空对象表示
//   失败；importExcel/exportExcel 这类返回结果对象的，则把错误塞进结果的 errors 列表。
//   错误码字符串集中在 include/dbridge/Errors.h（err::E_OPEN_DB 等）。
// ============================================================================

namespace dbridge {

// ──────────────────────────────────────────────────────────────────────────
// DataBridgePrivate 实现 —— pimpl 实现体的私有方法（被下面的公共 API 调用）
// ──────────────────────────────────────────────────────────────────────────

namespace detail {

// openDb —— 打开 SQLite 连接并完成「版本校验 + PRAGMA 配置 + 解析真实路径」。
//   做什么：用唯一连接名 connName_ 注册一条 QSQLITE 连接 → 打开 → 校验 SQLite 版本
//           （UPSERT 必需 >= 3.24）→ 设置 busy_timeout / WAL / foreign_keys → 通过
//           PRAGMA database_list 解析出 main 库的真实绝对路径 resolvedDbPath_。
//   参数：spec 连接规格；err 失败原因出参（可空）。
//   返回：成功 true 并置 dbOpen_=true；任一步失败 false（失败时已 close 连接）。
//   副作用：在 Qt 全局连接表里登记一条命名连接、改连接级 PRAGMA、写 resolvedDbPath_。
//   注意：不在此刷新 catalog——schema 自省由公共 open() 在本函数成功后单独调用。
bool DataBridgePrivate::openDb(const ConnectionSpec& spec, QString* err) {
    // 用本实例构造时生成的唯一连接名向 Qt 注册一条 QSQLITE 连接（唯一名避免多实例串号）。
    db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName_);
    db_.setDatabaseName(spec.sqlitePath);
    if (!db_.open()) {
        if (err)
            *err =
                QString::fromLatin1(err::E_OPEN_DB) + QStringLiteral(": ") + db_.lastError().text();
        return false;
    }

    // 校验 SQLite 版本 >= 3.24.0。
    // 为什么必须：本库的写库走 `INSERT ... ON CONFLICT DO UPDATE`（UPSERT），该语法
    // 自 SQLite 3.24.0 才支持；低版本会拼出无法执行的 SQL，故在此提前拦截并明确报错。
    QSqlQuery q(db_);
    if (!q.exec(QStringLiteral("SELECT sqlite_version()")) || !q.next()) {
        if (err)
            *err = QStringLiteral("Failed to query SQLite version");
        db_.close();
        return false;
    }
    QString ver = q.value(0).toString();
    QStringList parts = ver.split('.');  // "3.24.0" → ["3","24","0"]
    int major = parts.value(0).toInt();
    int minor = parts.value(1).toInt();
    int patch = parts.value(2).toInt();
    // 版本比较：major<3，或 3.<24，或 3.24.<0 都判定为过低（patch<0 分支恒不成立，
    // 仅为对称完整地表达「3.24.0 是下限」的语义）。
    if (major < 3 || (major == 3 && minor < 24) || (major == 3 && minor == 24 && patch < 0)) {
        if (err)
            *err = QString::fromLatin1(err::E_OPEN_DB) + QStringLiteral(": SQLite version ") + ver +
                   QStringLiteral(" < 3.24.0, ON CONFLICT DO UPDATE not supported");
        db_.close();
        return false;
    }

    // 配置连接级 PRAGMA（注意：PRAGMA 是连接私有的，每条连接都要各自设置）。
    q.exec(QStringLiteral("PRAGMA busy_timeout = ") +
           QString::number(spec.busyTimeoutMs));  // 锁争用时重试此毫秒数再报 busy
    if (spec.enableWal) {
        q.exec(QStringLiteral("PRAGMA journal_mode = WAL"));  // WAL：提升并发读写（同步层常需）
    }
    q.exec(QStringLiteral("PRAGMA foreign_keys = ON"));  // 打开外键约束执行（SQLite 默认关闭）

    // H-02 修复：通过 PRAGMA database_list 解析出 main 库的「真实物理路径」，使 URI 路径、
    // 相对路径、SQLite 别名都归一到同一个 OS 文件身份。
    // 为什么重要：dbPath() 把这个 resolvedDbPath_ 作为「同一文件」的身份键交给同步层
    // （SyncContextRegistry / BatchTransfer），若各处用不同写法指向同一文件就会错配上下文。
    resolvedDbPath_ = spec.sqlitePath;  // 兜底：解析失败时退回用户传入的原始路径
    if (q.exec(QStringLiteral("PRAGMA database_list"))) {
        // database_list 每行：(seq, name, file)；name=="main" 那行的 file 即主库文件路径。
        while (q.next()) {
            if (q.value(1).toString() == QLatin1String("main")) {
                const QString p = q.value(2).toString();
                if (!p.isEmpty())
                    resolvedDbPath_ = QFileInfo(p).absoluteFilePath();  // 归一为绝对路径
                break;
            }
        }
    }

    dbOpen_ = true;
    return true;
}

// closeDb —— 关闭连接并从 Qt 全局连接表中安全移除（幂等：重复/未打开调用无害）。
//   关键陷阱（见下方原英文注释）：必须先把成员 db_ 复位成一个空 QSqlDatabase，再调用
//   removeDatabase。原因：QSqlDatabase 是「显式共享」句柄（QExplicitlySharedDataPointer），
//   只要 db_ 还引用着这条连接，removeDatabase 就会认为「连接仍在使用」并打印告警。
void DataBridgePrivate::closeDb() {
    if (dbOpen_) {
        db_.close();
        dbOpen_ = false;
    }
    // Drop our QSqlDatabase reference BEFORE removeDatabase, otherwise Qt
    // logs "connection is still in use" because db_ itself still holds a
    // QExplicitlySharedDataPointer to the connection.
    // 【译】在 removeDatabase 之前先丢弃我们对 QSqlDatabase 的引用，否则 Qt 会因为 db_
    // 自身仍持有指向该连接的共享指针而打印「connection is still in use」告警。
    db_ = QSqlDatabase();  // 复位为无效句柄，释放对连接的共享引用
    if (QSqlDatabase::contains(connName_)) {
        QSqlDatabase::removeDatabase(connName_);  // 真正从全局连接表注销
    }
}

// loadProfileDoc —— 把一份已解析的 JSON 文档转成 ProfileSpec 并（在库已打开时）做校验后入库。
//   做什么：① 交 ProfileLoader 把 JSON → ProfileSpec；② 若数据库已打开，先刷新 catalog，
//           再用「导出模式」校验（H-01：在载入期而非导入期就反馈结构性错误）；
//           ③ 校验通过则按 spec.name 把这份 Profile 存进 profiles_ 映射表。
//   参数：doc 已解析的 JSON 文档；err 失败原因出参（可空）。
//   返回：成功 true；解析或校验失败 false 并写 *err。
//   副作用：可能刷新 catalog_；成功时写入 profiles_[spec.name]（同名会覆盖）。
bool DataBridgePrivate::loadProfileDoc(const QJsonDocument& doc, QString* err) {
    ProfileLoader loader;
    ProfileSpec spec;
    if (!loader.load(doc, &spec, err))  // JSON → ProfileSpec（语法/必填项等由 Loader 把关）
        return false;

    // H-01 fix: run schema/cross-field validation at load time (not only at import/export time)
    // so callers get immediate feedback for structurally invalid profiles.
    // Requires an open DB for cross-field column/table checks; skips if DB not yet opened.
    // 【译】H-01 修复：在「载入期」就跑 schema / 跨字段校验（而不是拖到导入/导出时），让调用方
    // 立刻得到「结构性非法 Profile」的反馈。跨字段的表/列存在性检查需要一个已打开的数据库；
    // 若此刻库尚未打开则跳过校验（待真正导入/导出时再校验）。
    if (dbOpen_) {
        QString catErr;
        if (refreshCatalog(&catErr)) {  // 先刷新一次 schema，让校验对着最新结构进行
            // Use export-mode validation: checks table/column existence, conflict keys,
            // columnOrder, fkInject, reverse lookup — but not Excel-header-specific rules.
            // 【译】采用「导出模式」校验：检查表/列是否存在、冲突键、columnOrder、fkInject、
            // 反向查找——但不含「Excel 表头是否匹配」这类只在导入时才需要的规则。
            // （之所以选导出模式：载入期还没拿到具体 Excel，无法校验表头。）
            ErrorCollector valErrors;
            ProfileValidator validator;
            if (!validator.validateForExport(spec, catalog_, &valErrors)) {
                if (err) {
                    // ErrorCollector 里可能有多条错误：拼成一条 "; " 分隔的可读串回报。
                    QStringList msgs;
                    for (const auto& e : valErrors.list())
                        msgs.append(e.message.isEmpty() ? e.code : e.message);  // 无 message 退回码
                    *err = msgs.isEmpty() ? QStringLiteral("Profile validation failed")
                                          : msgs.join(QStringLiteral("; "));
                }
                return false;
            }
        }
        // 注意：若 refreshCatalog 失败，这里「静默跳过校验」而非报错——载入仍按宽松处理，
        // 把结构性问题留待真正导入/导出时再暴露。
    }

    profiles_[spec.name] = spec;  // 以 Profile 名为键存入；同名覆盖（支持热替换配置）
    return true;
}

// refreshCatalog —— 重新自省当前数据库 schema 并刷新内部 catalog_。
//   委托 SchemaIntrospector 把表/列/索引/外键读进 catalog_（先 clear 再填，见其实现）。
//   失败置 *err 并返回 false。多处入口（open/import/export/snapshot）在动手前都会先调它，
//   以确保对着「当前真实结构」工作（schema 可能在两次调用之间被外部 DDL 改动）。
bool DataBridgePrivate::refreshCatalog(QString* err) {
    SchemaIntrospector introspector;
    return introspector.load(db_, &catalog_, err);
}

}  // namespace detail

// ──────────────────────────────────────────────────────────────────────────
// DataBridge 公共 API —— 以下方法多为「检查参数 → 转交 d_->子服务」的薄壳（pimpl）
// ──────────────────────────────────────────────────────────────────────────

// 构造：仅创建 pimpl 实现体（其构造里会生成本实例唯一的连接名 connName_）。
// 此刻尚未打开任何数据库；真正连库要等用户调用 open()。
DataBridge::DataBridge() : d_(std::make_unique<detail::DataBridgePrivate>()) {
}
// 析构：= default。之所以在 .cpp 里定义（而非头里 =default）——头文件只前置声明了
// DataBridgePrivate（不完整类型），unique_ptr 的析构需要见到完整定义，而完整定义只在
// 本 .cpp 可见（#include 了 DataBridgePrivate.h）。这是 pimpl 的固定写法。
DataBridge::~DataBridge() = default;

// open —— 打开连接 + 立即做一次全库 schema 自省（见 DataBridge.h 的契约说明）。
//   流程：若已打开先 close()（保证可重入重开）→ openDb 打开并配置连接 → refreshCatalog
//         把表/列/索引/外键读进 catalog_。任一步失败则回滚（关闭连接）并置 *err。
bool DataBridge::open(const ConnectionSpec& spec, QString* err) {
    if (d_->dbOpen_)
        close();  // 已打开则先关旧连接，再开新连接（避免连接名冲突 / 残留状态）
    if (!d_->openDb(spec, err))
        return false;

    // 初次 schema 自省：open 成功就应让 catalog_ 立即可用（后续导入/导出不必每次都首刷）。
    QString schErr;
    if (!d_->refreshCatalog(&schErr)) {
        if (err)
            *err = QStringLiteral("Schema introspection failed: ") + schErr;
        d_->closeDb();  // 自省失败视作 open 失败：回滚，关闭刚打开的连接，保持「未打开」语义
        return false;
    }
    return true;
}

// close —— 关闭连接（幂等，转交 closeDb）。
void DataBridge::close() {
    d_->closeDb();
}

// loadProfile —— 从磁盘 JSON 文件载入 Profile。
//   只负责「读文件 + 解析 JSON」，随后把解析好的文档交 loadProfileDoc 完成转换/校验/入库。
//   文件打不开、或 JSON 语法错误，分别置不同的 *err（后者用 E_PROFILE_PARSE 码）。
bool DataBridge::loadProfile(const QString& jsonPath, QString* err) {
    QFile f(jsonPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err)
            *err = QStringLiteral("Cannot open profile file: ") + jsonPath;
        return false;
    }
    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseErr);
    if (doc.isNull()) {  // JSON 语法非法
        if (err)
            *err = QString::fromLatin1(err::E_PROFILE_PARSE) + QStringLiteral(": ") +
                   parseErr.errorString();
        return false;
    }
    return d_->loadProfileDoc(doc, err);  // 余下转换/校验/入库统一走私有方法
}

// loadProfileFromString —— 同 loadProfile，但 Profile 内容直接来自内存中的 JSON 字符串
//   （常配合 generateAutoProfileJson 的输出，或用于测试 / 内嵌配置）。
bool DataBridge::loadProfileFromString(const QString& json, QString* err) {
    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &parseErr);  // 注意：UTF-8 字节
    if (doc.isNull()) {
        if (err)
            *err = QString::fromLatin1(err::E_PROFILE_PARSE) + QStringLiteral(": ") +
                   parseErr.errorString();
        return false;
    }
    return d_->loadProfileDoc(doc, err);
}

// generateAutoProfileJson —— 基于自省，为单张表自动推断一份「单表 Profile」并序列化为 JSON。
//   流程：要求库已打开 → 刷新 schema → 确认表存在 → 交 AutoProfileBuilder 据列/主键/唯一约束
//         推断出 ProfileSpec → 再由它序列化成 JSON 文本返回。
//   返回：成功返回 JSON 串；任一前置条件不满足返回空串并置 *err。
//   用法：返回串可存盘，或修改后喂给 loadProfileFromString。
QString DataBridge::generateAutoProfileJson(const QString& table, QString* err) {
    if (!d_->dbOpen_) {
        if (err)
            *err = QStringLiteral("Database not open");
        return {};
    }
    // 刷新 schema：保证按表的「当前真实结构」推断（用户可能刚改过表）。
    QString schErr;
    if (!d_->refreshCatalog(&schErr)) {
        if (err)
            *err = schErr;
        return {};
    }

    if (!d_->catalog_.hasTable(table)) {  // 表不存在 → 明确报 E_PROFILE_TABLE_NOT_FOUND
        if (err)
            *err =
                QString::fromLatin1(err::E_PROFILE_TABLE_NOT_FOUND) + QStringLiteral(": ") + table;
        return {};
    }

    detail::ProfileSpec spec;
    // 把该表的 TableInfo 交给构建器推断（解引用 table() 返回的指针——上面 hasTable 已确保非空）。
    if (!d_->autoBuilder_.build(*d_->catalog_.table(table), &spec, err))
        return {};
    return d_->autoBuilder_.toJson(spec);  // ProfileSpec → JSON 文本
}

// setSyncActive —— 切换「同步活动」标志，供 importExcel() 的直写门控判断（J-09）。
//   线程内存序关键点（见 DataBridge.h 注释）：先写普通成员 syncTables_，「最后」才写
//   atomic 的 syncActive_。这样任何线程一旦读到 syncActive_==true，就保证已能看到对应的
//   syncTables_（atomic 写带的 release 语义为前面的普通写提供可见性保证）。顺序绝不可颠倒。
void DataBridge::setSyncActive(bool active, const QStringList& syncTables) {
    d_->syncTables_ = syncTables;
    d_->syncActive_ = active;  // written last so reader sees syncTables_ first
                               // 【译】最后写，确保读者看到 active==true 时已先看到表清单
}

// profileRouteTables —— 收集一份 Profile 中「所有路由涉及的目标表名」（覆盖全部模式）。
//   M-02 辅助函数：直写门控要判断「这份 Profile 会不会写到被同步监控的表」，就需要把
//   单/多表模式的顶层 routes 与混合模式各 class 下的 routes 全部展开成一个表名列表。
// M-02 helper: collect all route-level table names from a profile (all modes).
static QStringList profileRouteTables(const detail::ProfileSpec& spec) {
    QStringList tables;
    for (const auto& r : spec.routes)  // SingleTable / MultiTable：顶层路由
        tables.append(r.table);
    for (const auto& cls : spec.classes)  // Mixed：每个类别各自的路由
        for (const auto& r : cls.routes)
            tables.append(r.table);
    return tables;
}

// importExcel —— 【同步阻塞】按已载入 Profile 把一张 .xlsx 导入数据库。
//   返回 ImportResult（读/写行数 + 逐行错误/警告；dryRun 下含构造好的载荷）。本方法本身
//   只做四道前置门禁，真正的搬运转交 d_->importSvc_.run（ImportService）。
//   前置门禁顺序：① 同步直写门控（J-09/M-02）② 库已打开 ③ Profile 已载入 ④ 刷新 catalog。
ImportResult DataBridge::importExcel(const QString& xlsxPath, const ImportOptions& options) {
    ImportResult result;
    // J-09: Block direct imports while sync is active; callers must use IBatchTransfer instead.
    // M-02 fix: if the profile's routes do not touch any sync-monitored table the import is safe
    // to proceed (e.g. auxiliary/reference tables not included in the sync table list).
    // 【译】J-09：同步活动期间禁止「直写导入」（绕过同步会让变更无法被 session 捕获、造成丢失），
    // 调用方应改用 IBatchTransfer 异步路径。M-02 放宽：若这份 Profile 的路由根本不触及任何被
    // 同步监控的表（如仅写不在同步清单里的辅助/参照表），则放行——这类导入对同步无害。
    if (d_->syncActive_) {
        // 先按名取出 Profile，以便检视它的路由表集合。
        auto profIt = d_->profiles_.find(options.profileName);
        bool blocked = true;  // 保守默认：无法判定安全时一律拦截
        if (profIt != d_->profiles_.end() && !d_->syncTables_.isEmpty()) {
            const QStringList routeTables = profileRouteTables(profIt.value());
            blocked = false;  // 既能取到 Profile 又有同步表清单 → 先假定安全，再逐表核对
            for (const QString& t : routeTables) {
                if (d_->syncTables_.contains(t)) {  // 命中任一被同步表 → 必须拦截
                    blocked = true;
                    break;
                }
            }
        }
        if (blocked) {
            RowError e;
            e.code = QString::fromLatin1(err::E_SYNC_WRITE_BLOCKED);
            e.message = QStringLiteral("Sync is active; use IBatchTransfer for imports");
            result.errors.append(e);
            return result;  // 注意：返回的 result.ok 仍是默认 false
        }
    }
    if (!d_->dbOpen_) {  // ② 库未打开
        RowError e;
        e.code = QString::fromLatin1(err::E_OPEN_DB);
        e.message = QStringLiteral("Database not open");
        result.errors.append(e);
        return result;
    }

    auto it = d_->profiles_.find(options.profileName);
    if (it == d_->profiles_.end()) {  // ③ 指定的 Profile 未载入
        RowError e;
        e.code = QString::fromLatin1(err::E_PROFILE_PARSE);
        e.message = QStringLiteral("Profile not loaded: ") + options.profileName;
        result.errors.append(e);
        return result;
    }

    // ④ 导入前刷新 catalog：确保 ImportService 对着「当前真实结构」做校验/外键查找/写库。
    QString schErr;
    if (!d_->refreshCatalog(&schErr)) {
        RowError e;
        e.code = QString::fromLatin1(err::E_OPEN_DB);
        e.message = QStringLiteral("Schema refresh failed: ") + schErr;
        result.errors.append(e);
        return result;
    }

    // 全部门禁通过：把活转交 ImportService，在门面自己的连接 d_->db_ 上执行。
    return d_->importSvc_.run(it.value(), d_->catalog_, xlsxPath, options, d_->db_);
}

// dbPath —— 返回当前打开库的「规范化绝对路径」（未打开返回空串）。
//   H-02：返回的是 openDb 经 PRAGMA database_list 解析出的 resolvedDbPath_，使
//   BatchTransfer 等调用方与 SyncContextRegistry 使用同一个「OS 文件身份」键，避免错配。
QString DataBridge::dbPath() const {
    // H-02 fix: return the resolved canonical path (from PRAGMA database_list) so that
    // BatchTransfer and other callers use the same OS-identity key as SyncContextRegistry.
    return d_->dbOpen_ ? d_->resolvedDbPath_ : QString();
}

// ── 以下为「供同步层使用」的方法（普通调用方一般不直接用）──────────────────

// runImportOnDb —— 用本门面的 Profile/catalog，但把写入定向到「调用方传入的连接 db」。
//   I-04：SyncWorker 需要在它自己持有的「带 session 变更捕获」的写连接上跑 ImportService，
//         因此不能用门面内部的 d_->db_，而是把目标连接作为参数注入。
//   前置门禁与 importExcel 相同（库已打开 / Profile 已载入 / 刷新 catalog），唯一区别是
//   末行把 d_->db_ 换成参数 db。注意：仍要求门面 open（用于取 Profile 与刷新 catalog）。
ImportResult DataBridge::runImportOnDb(const QString& xlsxPath, const ImportOptions& options,
                                       QSqlDatabase& db) {
    ImportResult result;
    if (!d_->dbOpen_) {
        RowError e;
        e.code = QString::fromLatin1(err::E_OPEN_DB);
        e.message = QStringLiteral("Database not open");
        result.errors.append(e);
        return result;
    }
    auto it = d_->profiles_.find(options.profileName);
    if (it == d_->profiles_.end()) {
        RowError e;
        e.code = QString::fromLatin1(err::E_PROFILE_PARSE);
        e.message = QStringLiteral("Profile not loaded: ") + options.profileName;
        result.errors.append(e);
        return result;
    }
    QString schErr;
    if (!d_->refreshCatalog(&schErr)) {
        RowError e;
        e.code = QString::fromLatin1(err::E_OPEN_DB);
        e.message = QStringLiteral("Schema refresh failed: ") + schErr;
        result.errors.append(e);
        return result;
    }
    // 关键区别：写库走「参数 db」而非门面自有连接 d_->db_（I-04 注入写连接）。
    return d_->importSvc_.run(it.value(), d_->catalog_, xlsxPath, options, db);
}

// runExportOnDb —— runImportOnDb 的导出版：用本门面 Profile/catalog，在参数 db 上执行导出。
//   注意：与公共 exportExcel 不同，这里不重复跑 validateForExport（同步路径上调用方已自行
//   保证 Profile 可用，且追求轻量），直接转交 ExportService。
ExportResult DataBridge::runExportOnDb(const QString& xlsxPath, const ExportOptions& options,
                                       QSqlDatabase& db) {
    ExportResult result;
    if (!d_->dbOpen_) {
        RowError e;
        e.code = QString::fromLatin1(err::E_OPEN_DB);
        e.message = QStringLiteral("Database not open");
        result.errors.append(e);
        return result;
    }
    auto it = d_->profiles_.find(options.profileName);
    if (it == d_->profiles_.end()) {
        RowError e;
        e.code = QString::fromLatin1(err::E_PROFILE_PARSE);
        e.message = QStringLiteral("Profile not loaded: ") + options.profileName;
        result.errors.append(e);
        return result;
    }
    return d_->exportSvc_.run(it.value(), d_->catalog_, xlsxPath, options, db);
}

// snapshotProfileCatalog —— 在「拥有者线程」上把指定 Profile + 整个 catalog 深拷贝给调用方。
//   为何需要（见 DataBridge.h）：catalog_/profiles_ 是门面线程私有状态，跨线程直接引用不安全。
//   派活到工作线程前，先在本线程刷新 schema，再用「值语义赋值」把它们整份拷出去；工作线程
//   随后只操作这两份独立副本，规避数据竞争。
//   参数：profileName 选哪份；profile/catalog 为出参（可任一为 nullptr 表示不需要）；err 出参。
//   返回：成功 true；库未打开 / 刷新失败 / Profile 不存在则 false 并置 *err。
bool DataBridge::snapshotProfileCatalog(const QString& profileName, detail::ProfileSpec* profile,
                                        detail::SchemaCatalog* catalog, QString* err) {
    if (!d_->dbOpen_) {
        if (err)
            *err = QStringLiteral("Database not open");
        return false;
    }
    QString schErr;
    if (!d_->refreshCatalog(&schErr)) {  // 先在拥有者线程刷新，保证拷出的是最新 schema
        if (err)
            *err = QStringLiteral("Schema refresh failed: ") + schErr;
        return false;
    }
    auto it = d_->profiles_.find(profileName);
    if (it == d_->profiles_.end()) {
        if (err)
            *err = QStringLiteral("Profile not loaded: ") + profileName;
        return false;
    }
    if (profile)
        *profile = it.value();  // 值拷贝整份 ProfileSpec（深拷贝，独立于门面内副本）
    if (catalog)
        *catalog = d_->catalog_;  // 值拷贝整个 catalog（QHash 写时复制，赋值即得独立副本）
    return true;
}

// snapshotCatalog —— 只深拷贝当前 schema catalog（不涉及任何 Profile）。
//   用于「不依赖 ETL Profile」的同步路径（如基线导出、schema 指纹比对）。逻辑同上去掉 Profile
//   步骤。
bool DataBridge::snapshotCatalog(detail::SchemaCatalog* catalog, QString* err) {
    if (!d_->dbOpen_) {
        if (err)
            *err = QStringLiteral("Database not open");
        return false;
    }
    QString schErr;
    if (!d_->refreshCatalog(&schErr)) {
        if (err)
            *err = QStringLiteral("Schema refresh failed: ") + schErr;
        return false;
    }
    if (catalog)
        *catalog = d_->catalog_;  // 仅拷贝目录副本给调用方
    return true;
}

// exportExcel —— 【同步阻塞】按已载入 Profile 把数据库导出成一张 .xlsx。
//   是 importExcel 的「反方向」公共门面。前置门禁顺序：① 库已打开 ② Profile 已载入
//   ③ 刷新 catalog ④ 导出模式校验（H-03，提前暴露 columnOrder/rawSql/表列存在性错误），
//   全部通过后把真正的搬运转交 d_->exportSvc_.run（ExportService），在门面自有连接 d_->db_ 上执行。
//   返回 ExportResult（导出行数 + 逐行错误/警告）；任一门禁失败则把错误塞进 result.errors 返回。
//   注意：导出方向无「同步直写门控」——读库不改库，不会绕过 session 捕获，故不像 importExcel 那样判
//   syncActive_。
ExportResult DataBridge::exportExcel(const QString& xlsxPath, const ExportOptions& options) {
    ExportResult result;
    if (!d_->dbOpen_) {  // ① 库未打开
        RowError e;
        e.code = QString::fromLatin1(err::E_OPEN_DB);
        e.message = QStringLiteral("Database not open");
        result.errors.append(e);
        return result;
    }

    auto it = d_->profiles_.find(options.profileName);
    if (it == d_->profiles_.end()) {  // ② 指定的 Profile 未载入
        RowError e;
        e.code = QString::fromLatin1(err::E_PROFILE_PARSE);
        e.message = QStringLiteral("Profile not loaded: ") + options.profileName;
        result.errors.append(e);
        return result;
    }

    // Refresh catalog before export
    // 【译】③ 导出前刷新 catalog：保证 ExportService 对着「当前真实结构」做列映射/校验/取数。
    QString schErr;
    if (!d_->refreshCatalog(&schErr)) {
        RowError e;
        e.code = QString::fromLatin1(err::E_OPEN_DB);
        e.message = QStringLiteral("Schema refresh failed: ") + schErr;
        result.errors.append(e);
        return result;
    }

    // H-03 fix: run export-mode profile validation before export so columnOrder, rawSql,
    // and table/column existence errors surface early.
    // 【译】H-03 修复：④ 导出前先跑「导出模式」Profile 校验，让 columnOrder、rawSql、以及
    // 表/列是否存在等错误「提前暴露」（而非搬运到一半才失败）。校验不通过则直接返回一个
    // 只含校验错误的 ExportResult（注意这里特意新建 valResult，使返回的错误集干净、只含校验项）。
    {
        detail::ErrorCollector valErrors;
        detail::ProfileValidator validator;
        if (!validator.validateForExport(it.value(), d_->catalog_, &valErrors)) {
            ExportResult valResult;
            valResult.errors = valErrors.list();
            return valResult;
        }
    }

    // 全部门禁通过：把活转交 ExportService，在门面自有连接 d_->db_ 上执行 DB→Excel 搬运。
    return d_->exportSvc_.run(it.value(), d_->catalog_, xlsxPath, options, d_->db_);
}

}  // namespace dbridge
