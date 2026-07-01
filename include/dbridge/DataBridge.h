#pragma once
#include "dbridge/Export.h"
#include "dbridge/SchemaInfo.h"
#include "dbridge/Types.h"

#include <QSqlDatabase>
#include <QStringList>

#include <memory>

// ============================================================================
// DataBridge.h — 整个 dbridge 库的「对外门面」（Facade）公共 API
// ============================================================================
//
// 【这个类是什么】
//   DataBridge 是用户接触整个库的唯一入口。它把分散在各内部子系统（Excel 读写、
//   schema 自省、Profile 解析校验、导入/导出服务、SQL 构建、乃至同步引擎）的能力
//   收拢成一组高层方法：open/close、loadProfile、importExcel/exportExcel、
//   generateAutoProfileJson……调用方无需了解内部类，只面对这一个门面。
//
// 【典型生命周期（建立直觉）】
//   DataBridge bridge;
//   bridge.open(spec);                       // ① 打开 SQLite 连接并做一次 schema 自省
//   bridge.loadProfile("orders.json");       // ② 载入一份 Excel↔DB 映射配置（Profile）
//   auto r = bridge.importExcel("a.xlsx", o); // ③ 按 Profile 把 Excel 灌进数据库
//   auto e = bridge.exportExcel("b.xlsx", o); // ④ 反向：把数据库查出来写成 Excel
//   bridge.close();                          // ⑤ 释放连接
//
// 【pimpl 惯用法（pointer-to-implementation）】
//   本类只在头里暴露「方法签名 + 一个不透明指针 d_」，真正的成员数据与实现细节
//   全部藏在 DataBridgePrivate（见 src/DataBridgePrivate.h）。好处：
//     · 头文件不必 #include 内部类型（ImportService/SchemaCatalog 等），编译更快、
//       依赖更少；改动内部实现不会触发下游用户重编译（ABI/编译防火墙）。
//     · 故下面只前置声明 detail::DataBridgePrivate / ProfileSpec / SchemaCatalog，
//       不包含它们的完整定义。
//
// 【与「同步层（sync）」的关系】
//   除了给最终用户用的常规方法外，本门面还额外暴露一组「供同步子系统调用」的方法
//   （runImportOnDb / runExportOnDb / snapshotProfileCatalog / snapshotCatalog /
//    dbPath / setSyncActive）。它们带有 I-04 / J-09 等修复标记，专门解决「同步在
//   后台线程上、用另一条数据库连接、并要捕获变更集」这些场景下的线程与一致性问题。
//   普通调用方一般用不到这些，下面逐一注明其用途与约束。
//
// 【DBRIDGE_EXPORT】
//   符号可见性导出宏（来自 Export.h）。动态库下控制导出/导入；本项目以静态库链接
//   （定义了 DBRIDGE_STATIC_DEFINE）时展开为空，不产生任何修饰。
// ============================================================================

namespace dbridge {

// 前置声明库内部（detail 命名空间）类型——见上文「pimpl 惯用法」：只声明不包含，
// 让本公共头与内部实现解耦。它们的完整定义分别在 DataBridgePrivate.h /
// profile/ProfileSpec.h / schema/SchemaCatalog.h。
namespace detail {
class DataBridgePrivate;  // 私有实现体（藏所有成员数据与子服务）
struct ProfileSpec;       // 一份解析后的 Excel↔DB 映射配置
class SchemaCatalog;      // 自省得到的数据库 schema 目录
}  // namespace detail

class DBRIDGE_EXPORT DataBridge {
   public:
    DataBridge();  // 构造：仅创建 pimpl 实现体（生成唯一连接名），尚未打开任何数据库
    ~DataBridge();  // 析构：在 .cpp 里 = default；此处需声明，否则 unique_ptr 无法析构不完整类型
    // 禁止拷贝：DataBridge 独占一条 QSqlDatabase 连接，复制语义没有意义且会引发双重关闭。
    DataBridge(const DataBridge&) = delete;
    DataBridge& operator=(const DataBridge&) = delete;

    // open —— 打开 SQLite 连接并立即做一次全库 schema 自省。
    //   做什么：按 spec 打开数据库、校验 SQLite 版本(>=3.24，UPSERT 必需)、设置
    //           busy_timeout/WAL/foreign_keys 等 PRAGMA，再调用 SchemaIntrospector
    //           把表/列/索引/外键读进内部 catalog。若已打开，会先 close() 再重开。
    //   参数：spec 连接规格（路径、忙等待超时、是否启用 WAL，见 Types.h）。
    //   返回：成功 true；失败 false 并把原因写入 *err（若非空）。
    //   副作用：建立 Qt 命名连接、修改连接级 PRAGMA、刷新内部 catalog。
    bool open(const ConnectionSpec& spec, QString* err = nullptr);
    // close —— 关闭连接并从 Qt 连接表中移除（幂等：未打开时调用无害）。
    void close();

    // loadProfile —— 从磁盘 JSON 文件载入一份 Profile（按 Profile 内的 name 索引存放）。
    //   读文件→解析 JSON→交 ProfileLoader 转成 ProfileSpec；若库已打开，还会立刻用当前
    //   schema 做一次校验（H-01：在载入期而非导入期就反馈结构性错误）。失败置 *err。
    bool loadProfile(const QString& jsonPath, QString* err = nullptr);
    // loadProfileFromString —— 同上，但 Profile 内容直接来自内存中的 JSON 字符串
    //   （常配合 generateAutoProfileJson 的输出，或测试/内嵌配置使用）。
    bool loadProfileFromString(const QString& json, QString* err = nullptr);

    // generateAutoProfileJson —— 基于 SQLite 自省，为单张表自动生成一份「单表 Profile」的 JSON。
    //   做什么：刷新 schema → 取该表的列/主键/唯一约束，由 AutoProfileBuilder 推断出一份
    //           可直接使用（或供用户编辑）的 Profile，并序列化为 JSON 字符串返回。
    //   用法：返回的字符串可保存到文件，或修改后再喂给 loadProfileFromString。
    //   返回：成功返回 JSON 文本；失败返回空串并置 *err（库未打开 / 表不存在 / 无主键等）。
    QString generateAutoProfileJson(const QString& table, QString* err = nullptr);

    // ── schema 发现（表清单 / 表结构）───────────────────────────────────────────
    // 这两个方法把库内部的 schema 自省结果以「公共值类型」（见 SchemaInfo.h）暴露给调用方，
    // 使其无需自行编写 PRAGMA / sqlite_master 查询即可动态发现「库里有哪些表、每表的列与主键」。
    // 二者都会先刷新一次内部 schema 目录（对齐当前真实结构），故要求库已 open()。

    // userTables —— 列出当前库中的「用户表」，即排除 SQLite 内建表（sqlite_%）与本库同步
    //   子系统的元数据表（__sync_%）后的全部基表，按表名升序返回（确定序，便于展示/比对）。
    //   返回：表名列表；库未打开或自省失败时返回空列表并置 *err（若非空）。
    QStringList userTables(QString* err = nullptr);

    // describeTable —— 取某张表的列/主键结构（列按建表列序，含主键标记与复合主键次序）。
    //   参数：table 表名；out 出参，成功时被填为该表的 TableSchema；err 出参（可空）。
    //   返回：成功 true；库未打开 / 自省失败 / 表不存在 → false 并置 *err。
    bool describeTable(const QString& table, TableSchema* out, QString* err = nullptr);

    // importExcel —— 【同步阻塞】按已载入的 Profile 把一张 .xlsx 导入数据库。
    //   流程：读 Excel→按路由展开成多表载荷→校验/类型转换/外键查找→UPSERT 写库。
    //   返回 ImportResult（读取/写入行数 + 逐行错误/警告列表；dryRun 下含构造好的载荷）。
    //   J-09：同步活动期间，针对「会触及被同步表」的 Profile 会被门控拒绝（改用
    //         IBatchTransfer 异步路径，以便走 session 变更捕获）；只触及非同步表的放行。
    ImportResult importExcel(const QString& xlsxPath, const ImportOptions& options);
    // exportExcel —— 【同步阻塞】按 Profile 从数据库查询并写出一张 .xlsx。
    //   导出前会先做一次 export 模式的 Profile 校验（H-03：让 columnOrder/rawSql/表列存在性
    //   等错误尽早暴露）。返回 ExportResult（写出行数 + 错误/警告）。
    ExportResult exportExcel(const QString& xlsxPath, const ExportOptions& options);

    // dbPath —— 返回当前打开的 SQLite 文件「规范化绝对路径」（未打开返回空串）。
    //   为何重要：同步层用它作为「同一个 OS 文件」的身份键去 SyncContextRegistry 查上下文。
    //   H-02：返回的是经 PRAGMA database_list 解析后的 main 库真实路径，使 URI 路径、
    //         相对路径、SQLite 别名都归一到同一文件身份，避免 BatchTransfer 等用错 key。
    QString dbPath() const;

    // ── 以下为「供同步层使用」的方法（普通调用方一般不直接用）─────────────────

    // runImportOnDb —— 用本门面已存的 Profile/catalog，但把写入定向到调用方传入的 db 连接。
    //   I-04：SyncWorker 需要在它自己持有的「带 session 变更捕获」的写连接 wconn 上跑
    //         ImportService，因此不能用门面内部的 db_，而是把连接作为参数注入进来。
    //   注意：仍要求门面已 open（用来取 Profile 并刷新 catalog），写库走的却是参数 db。
    ImportResult runImportOnDb(const QString& xlsxPath, const ImportOptions& options,
                               QSqlDatabase& db);
    // runExportOnDb —— 同上思路的导出版：用本门面的 Profile/catalog，在参数 db 上执行导出。
    ExportResult runExportOnDb(const QString& xlsxPath, const ExportOptions& options,
                               QSqlDatabase& db);

    // snapshotProfileCatalog —— 在「拥有者线程」上把 Profile 与 catalog 拷贝一份给调用方。
    //   为何需要：catalog/profiles 是门面线程私有状态，跨线程直接引用不安全。派活到工作
    //   线程前，先在本线程刷新 schema 并把指定 Profile + 整个 catalog 深拷贝出去（值语义），
    //   工作线程随后只操作这份独立副本，规避数据竞争。失败置 *err（未打开/Profile 不存在）。
    bool snapshotProfileCatalog(const QString& profileName, detail::ProfileSpec* profile,
                                detail::SchemaCatalog* catalog, QString* err = nullptr);

    // snapshotCatalog —— 只拷贝当前 schema catalog（不涉及任何 Profile）。
    //   用于「不依赖 ETL Profile」的同步路径（如基线导出、schema 指纹比对）。
    bool snapshotCatalog(detail::SchemaCatalog* catalog, QString* err = nullptr);

    // setSyncActive —— 切换「同步活动」标志，给 importExcel() 的直写门控提供依据。
    //   J-09：由 SyncEngine::initialize() 置 true、~SyncEngine() 置 false，防止同步进行时
    //         有人绕过同步直接写库造成变更丢失。
    //   syncTables：被同步监控的规范表清单。importExcel() 会据此放行「只写非同步表」的
    //               Profile（M-02），仅拦截真正会触及同步表的导入。
    //   线程：syncActive_ 是 atomic，且实现里「先写 syncTables_、后写 syncActive_」，
    //         保证读到 active==true 时一定已看到对应的表清单（见 .cpp 注释）。
    void setSyncActive(bool active, const QStringList& syncTables = {});

   private:
    std::unique_ptr<detail::DataBridgePrivate> d_;  // pimpl：唯一的实现体指针（藏所有状态）
};

}  // namespace dbridge
