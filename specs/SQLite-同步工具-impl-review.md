# 代码审查报告 — SQLite同步工具+Excel导入导出

## 总览

- 审查范围：`src/` 目录下全部 `.h` / `.cpp` 源文件，共 122 个文件；重点覆盖 profile 加载/校验、Excel 导入导出、lookup/fkInject/time-format、SQLite 同步捕获/应用/传输/table_state 等实现。
- 规范文档列表：
  1. `specs/Qt-SQLite-Excel-批量导入导出-设计文档.md`
  2. `specs/MVP-Qt-SQLite-Excel-批量导入导出-实现设计.md`
  3. `specs/SQLite-同步工具-设计文档.md`
  4. `specs/SQLite-同步工具-plan.md`
  5. `openspec/specs/export-column-order/spec.md`
  6. `openspec/specs/export-reverse-lookup/spec.md`
  7. `openspec/specs/fk-injection/spec.md`
  8. `openspec/specs/row-lookup/spec.md`
  9. `openspec/specs/time-format/spec.md`
- 总体评分：78 / 100
- 问题数量统计：Critical 0，High 4，Medium 3，Low 1。

主要结论：Excel 导入主链路的大部分关键要求已经实现，包括 `ON CONFLICT DO UPDATE`、绑定参数、批量 lookup 预取、fkInject 数组形态、route-local 失败集合、时间格式解析、`columnOrder` 校验以及多数同步三分支骨架。但同步上下文身份、Session 初始化硬门禁、反向 lookup 的 route-local 导出语义和 `table_state` 校验和规范仍有明显偏差，会影响同步正确性或导出结果可靠性。

## Critical 问题（必须修复，阻断功能）

本轮未确认到 Critical 级别问题。当前实现不是完全不可运行，但存在 High 级正确性问题，建议在发布前修复。

## High 问题（严重，影响正确性）

### H-01：SyncContext 未按实际 SQLite 主库身份解析，也未落库校验 `context_uuid`

- 文件位置：`src/sync/SyncContext.cpp:20`，`src/sync/SyncContext.cpp:65`，`src/sync/SyncContext.h:28`
- 问题描述：`SyncContextRegistry::canonicalKey()` 直接对调用方传入的 `sqlitePath` 做 `stat()` / Windows file index。规范要求先打开 SQLite 连接，通过 `PRAGMA database_list` 读取 `main` 库的已解析路径，再基于 OS 文件身份生成 key，并在库内写入/校验 `context_uuid` 作为兜底。当前 `contextUuid` 只在内存生成，未写入任何 `__sync_*` 元数据表；同时 URI 路径、SQLite 解析后的相对路径、新建库路径迁移等场景没有按规范处理。
- 违反规范：`specs/SQLite-同步工具-设计文档.md` §4.3 / G-07；`specs/SQLite-同步工具-plan.md` T1.0b。
- 影响：同一物理库通过 SQLite URI、不同路径表示或平台特定路径规则打开时，可能无法复用同一个 `SyncContext`，从而破坏前台互斥 gate 和单写线程假设；反过来，也缺少库内 UUID 兜底来防止错误合并两个物理库上下文。
- 修复建议：把 context key 解析移到“已打开 SQLite 主库”之后，使用 `PRAGMA database_list` 的 main 路径做 OS identity，并在同一库内持久化 UUID。

```cpp
// 示例：打开连接后解析 main 库真实路径，再生成 registry key。
static QString resolvedMainPath(QSqlDatabase& db, QString* err) {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("PRAGMA database_list"))) {
        if (err) *err = q.lastError().text();
        return {};
    }
    while (q.next()) {
        if (q.value(1).toString() == QLatin1String("main"))
            return QFileInfo(q.value(2).toString()).canonicalFilePath();
    }
    if (err) *err = QStringLiteral("main database not found");
    return {};
}

static bool ensureContextUuid(QSqlDatabase& db, const QString& expected, QString* err) {
    QSqlQuery ddl(db);
    if (!ddl.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS __sync_context_meta("
            "k TEXT PRIMARY KEY, v TEXT NOT NULL)"))) {
        if (err) *err = ddl.lastError().text();
        return false;
    }

    QSqlQuery sel(db);
    sel.prepare(QStringLiteral("SELECT v FROM __sync_context_meta WHERE k='context_uuid'"));
    if (!sel.exec()) {
        if (err) *err = sel.lastError().text();
        return false;
    }
    if (sel.next())
        return sel.value(0).toString() == expected;

    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(
        "INSERT INTO __sync_context_meta(k, v) VALUES('context_uuid', ?)"));
    ins.addBindValue(expected);
    return ins.exec();
}
```

### H-02：反向 lookup `exportOnMissing:"error"` 会跳过整条导出行，而不是只跳过声明 route 的贡献

- 文件位置：`src/service/ExportService.cpp:479`，`src/service/ExportService.cpp:503`，`src/service/ExportService.cpp:779`，`src/service/ExportService.cpp:963`
- 问题描述：`resolveAHeaders()` 遇到 NULL H 值或未命中 G 表时，将 `*rowSkip = true` 并返回；调用方随后 `continue`，整条 Excel 输出行被丢弃。规范要求 `exportOnMissing:"error"` 对声明 lookup 的 route 生效，“other routes contributing to the same Excel row are unaffected”。在多表 auto-join 或 Mixed 导出中，这会把同一 Excel 行中父 route / 兄弟 route 已经可导出的字段一并丢掉。
- 违反规范：`openspec/specs/export-reverse-lookup/spec.md` 的 `exportOnMissing` 行为和 row-resilient 要求。
- 影响：导出数据丢失，尤其是父表字段本来可导出、子 route 的反向 lookup 缺失时，当前实现会连父表字段也不写。
- 修复建议：把 `rowSkip` 改为 route-local 失败集合，例如记录失败 route 或失败 header，最终只屏蔽该 route 的 A headers / 该 route 拥有的输出字段；不要在 `E_REVERSE_LOOKUP_NOT_FOUND` 时整行 `continue`。只有无法安全构造整行的结构性错误才整行跳过。

```cpp
struct ReverseResolution {
    QHash<QString, QVariant> aValues;
    QSet<QString> failedAHeaders;
    QSet<QString> failedRouteTables;
    bool fatalRowSkip = false;
};

// NOT_FOUND: route-local error，不设置 fatalRowSkip。
if (lk.exportOnMissing == QLatin1String(ExportOnMissing::kError)) {
    errors->add(sheet, rowIndex, lk.name, tkey,
                QString::fromLatin1(err::E_REVERSE_LOOKUP_NOT_FOUND),
                message);
    out.failedRouteTables.insert(route.table);
    for (const auto& mp : lk.match)
        out.failedAHeaders.insert(mp.second);
    continue;
}

// 写行时只清空失败 route 的字段。
if (failedRouteTables.contains(ownerRouteByHeader.value(h))) {
    rowVals.append(QVariant());
} else {
    rowVals.append(valueForHeader(h));
}
```

### H-03：同步初始化硬门禁只检查 compile option，没有在实际 Qt SQLite 句柄上执行 session 调用

- 文件位置：`src/sync/capture/SqliteHandle.cpp:15`，`src/sync/SyncWorker.cpp:184`
- 问题描述：`SqliteHandle::sessionAvailable()` 只调用 `sqlite3_compileoption_used("ENABLE_SESSION")` 和 `sqlite3_compileoption_used("ENABLE_PREUPDATE_HOOK")`。规范要求运行期必须从同一个 `QSqlDatabase` 取出的 `sqlite3*` 成功调用 `sqlite3session_create/attach/changeset`。当前初始化阶段通过 compile option 后就继续 DDL 和 eligibility，真正的 `SessionRecorder::begin()` 要到首次本地写/导入时才执行；如果 Qt QSQLITE 插件和链接到的 SQLite 符号不一致，初始化可能成功，首次写才失败。
- 违反规范：`specs/SQLite-同步工具-设计文档.md` §13.1 硬验收；`specs/SQLite-同步工具-plan.md` T1.11 初始化门禁。
- 影响：同步模式可能在不可捕获 changeset 的环境中被标记为可用，直到业务写入时才失败，破坏“初始化阶段失败、不得静默降级”的契约。
- 修复建议：在 `SyncWorker::run()` 展开 `canonicalSyncTables_` 并通过 schema eligibility 后，对实际 `wconn` 的 `sqlite3*` 做一次短命 session 自检。

```cpp
static bool exerciseSession(sqlite3* h, const QStringList& tables, QString* err) {
    sqlite3_session* s = nullptr;
    int rc = sqlite3session_create(h, "main", &s);
    if (rc != SQLITE_OK) {
        if (err) *err = QStringLiteral("sqlite3session_create failed");
        return false;
    }
    for (const QString& table : tables) {
        const QByteArray name = table.toUtf8();
        rc = sqlite3session_attach(s, name.constData());
        if (rc != SQLITE_OK) {
            sqlite3session_delete(s);
            if (err) *err = QStringLiteral("sqlite3session_attach failed for %1").arg(table);
            return false;
        }
    }
    int n = 0;
    void* p = nullptr;
    rc = sqlite3session_changeset(s, &n, &p);
    sqlite3_free(p);
    sqlite3session_delete(s);
    return rc == SQLITE_OK;
}
```

### H-04：`table_state` 行哈希不是规范编码，可能发生可构造碰撞并导致差异误判

- 文件位置：`src/sync/schema/TableStateStore.cpp:160`，`src/sync/apply/CapturedWriteTemplate.cpp:532`
- 问题描述：`TableStateStore::rowHash()` 将行序列化为 `key=value\n`，并直接使用 `QVariant::toByteArray()`。该格式没有类型标签和长度前缀，文本中包含换行、等号、列分隔符时可构造相同字节流；数值 `1`、文本 `"1"`、部分 BLOB/文本也可能被归一到相同字节表示。同步设计要求 `H(row)` 使用“按列序的规范编码强哈希”，`content_checksum` 是 DiffEngine 表级判等的权威依据。
- 违反规范：`specs/SQLite-同步工具-设计文档.md` §6.2；`specs/SQLite-同步工具-plan.md` T1.6 / T4.1。
- 影响：两个内容不同的表可能得到相同 `content_checksum`，DiffEngine 误判 Identical；反向也可能因为编码不稳定造成假差异。
- 修复建议：改为长度前缀 + 类型标签 + SQLite 值语义的规范编码，并保证 baseline 全扫路径和 changeset 增量路径共用同一个编码函数。

```cpp
static void writeBytes(QDataStream& ds, const QByteArray& bytes) {
    ds << quint32(bytes.size());
    ds.writeRawData(bytes.constData(), bytes.size());
}

QByteArray TableStateStore::rowHash(const QVariantMap& row) {
    QByteArray data;
    QDataStream ds(&data, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::BigEndian);

    ds << quint32(row.size());
    for (auto it = row.cbegin(); it != row.cend(); ++it) {
        writeBytes(ds, it.key().toUtf8());
        const QVariant& v = it.value();
        if (v.isNull()) {
            ds << quint8(0);
        } else if (v.typeId() == QMetaType::QByteArray) {
            ds << quint8(4);
            writeBytes(ds, v.toByteArray());
        } else if (v.canConvert<qlonglong>()) {
            ds << quint8(1) << qint64(v.toLongLong());
        } else if (v.canConvert<double>()) {
            ds << quint8(2) << double(v.toDouble());
        } else {
            ds << quint8(3);
            writeBytes(ds, v.toString().toUtf8());
        }
    }
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).left(16);
}
```

## Medium 问题（中等，影响质量）

### M-01：同步导入后用全表扫描重建 `__sync_table_state`，违反常规写路径增量维护要求

- 文件位置：`src/sync/SyncWorker.cpp:1516`
- 问题描述：`submitImportSync()` 在导入成功后调用 `ts_->resetFromBaseline()`，注释也明确说明这是 full scan。规范要求 `table_state` 在 apply/import/save 等常规写路径中通过 changeset 或 `RowMutation` 增量维护；只有 re-baseline 允许全表扫描。
- 违反规范：`specs/SQLite-同步工具-设计文档.md` §6.2；`specs/SQLite-同步工具-plan.md` T1.6、T3.1、T5.1。
- 影响：大表导入后会 O(全同步表行数) 扫描，性能与数据规模绑定；如果扫描期间失败还会扩大事务持有时间，增加锁竞争。
- 修复建议：让 `SessionRecorder::sealInto()` 返回本次 changeset，或者把导入写入改走 `CapturedWriteTemplate`，在同一事务内由 changeset 提取 `TableMutation` 后调用 `applyMutations()`。

```cpp
QByteArray captured;
if (!rec_->sealInto(hPtr_, *clog_, *wconnPtr_, txn,
                    config_.nodeId(), streamEpoch_,
                    config_.schemaVersion(), fp,
                    0, originSeq, &localSeq, &sealErr, {}, &captured)) {
    txn.rollback();
    return fail(sealErr);
}

const QList<TableMutation> muts =
    CapturedWriteTemplate::extractMutations(captured, canonicalSyncTables_);
if (!ts_->applyMutations(*wconnPtr_, muts, streamEpoch_, fp, originSeq, &tsErr)) {
    txn.rollback();
    return fail(tsErr);
}
```

### M-02：`table_state` 模加实现使用有符号 `qint64` 中间值，存在溢出/实现定义行为

- 文件位置：`src/sync/schema/TableStateStore.cpp:35`，`src/sync/schema/TableStateStore.cpp:44`，`src/sync/schema/TableStateStore.cpp:193`
- 问题描述：代码注释说 checksum 是 `quint64 modular sum`，但 `Delta::checksumDelta` 是 `qint64`；`hashToU64()` 返回 `quint64` 后被强转为 `qint64` 累加，`updateRow()` 又执行 `static_cast<qint64>(oldSum) + checksumDelta`。当高位为 1 或批次较大时，有符号溢出在 C++ 中不是规范的无符号模加语义。
- 违反规范：`specs/SQLite-同步工具-设计文档.md` §6.2 “模加”聚合要求。
- 影响：不同编译器/优化级别下可能出现不一致 checksum，破坏跨节点判等稳定性。
- 修复建议：所有 checksum 聚合使用 `quint64`；删除用负数表达 DELETE 的写法，改为显式加/减无符号值。

```cpp
struct Delta {
    quint64 add = 0;
    quint64 sub = 0;
    qint64 rowDelta = 0;
};

if (m.isInsert) {
    d.add += hashToU64(m.afterHash);
    d.rowDelta += 1;
} else if (m.isDelete) {
    d.sub += hashToU64(m.beforeHash);
    d.rowDelta -= 1;
} else {
    d.add += hashToU64(m.afterHash);
    d.sub += hashToU64(m.beforeHash);
}

const quint64 newSum = oldSum + delta.add - delta.sub;
```

### M-03：自动 profile 对无 PK/UNIQUE 的表直接失败，未提供规范要求的 draft/issues 形态

- 文件位置：`src/profile/AutoProfileBuilder.cpp:54`，`src/DataBridge.cpp:152`
- 问题描述：`AutoProfileBuilder::build()` 在找不到 conflict key 时直接返回 `E_PROFILE_NO_CONFLICT_KEY`。MVP 文档允许以错误形式阻止不可执行 profile，但完整设计文档要求未知单表场景生成可检查的草稿，标记 `executable=false` 并携带 issues，便于用户补全唯一键/冲突策略后再执行。
- 违反规范：`specs/Qt-SQLite-Excel-批量导入导出-设计文档.md` 的自动配置草稿/问题收集要求；与 `specs/MVP-Qt-SQLite-Excel-批量导入导出-实现设计.md` 的最小实现边界存在取舍差异。
- 影响：用户无法从 API 获取列映射草稿，只得到失败；交互式修正 profile 的工作流不完整。
- 修复建议：保留现有 `generateAutoProfileJson()` 的 MVP 行为也可以，但应新增 draft API 或在返回 JSON 中包含 `executable/issues`。

```cpp
struct ProfileDraft {
    ProfileSpec profile;
    bool executable = true;
    QStringList issues;
};

if (conflictCols.isEmpty()) {
    draft.executable = false;
    draft.issues.append(QStringLiteral(
        "table '%1' has no PRIMARY KEY or UNIQUE constraint; "
        "please choose conflict.columns").arg(table.name));
    // 仍填充 columns，方便 UI 展示和用户补全。
}
```

## Low 问题（轻微，改善建议）

### L-01：`OutboxWriter` 使用 POSIX `fsync/open/close`，未做 Windows 分支保护

- 文件位置：`src/sync/transport/OutboxWriter.cpp:8`，`src/sync/transport/OutboxWriter.cpp:58`，`src/sync/transport/OutboxWriter.cpp:113`
- 问题描述：文件直接包含 `<fcntl.h>` / `<unistd.h>` 并调用 `::fsync()`、`::open()`、`::close()`。项目主体是 Qt/C++ 跨平台工具，同步设计也覆盖 Windows 文件身份处理；该实现会在 MSVC/Windows 构建上失败或需要兼容层。
- 违反规范：`specs/SQLite-同步工具-设计文档.md` 的跨平台同步实现目标；也与 `SyncContext.cpp` 已有 `Q_OS_WIN` 分支风格不一致。
- 影响：Windows 构建/发布不可用，属于可移植性问题。
- 修复建议：用 `#ifdef Q_OS_WIN` 分支调用 `FlushFileBuffers()`，POSIX 分支保留 `fsync()`；目录 fsync 在 Windows 上明确 no-op 或使用平台等价策略。

```cpp
#ifdef Q_OS_WIN
#include <windows.h>
#include <io.h>
static bool flushFileHandle(QFile& f) {
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(static_cast<int>(f.handle())));
    return h != INVALID_HANDLE_VALUE && FlushFileBuffers(h);
}
#else
#include <fcntl.h>
#include <unistd.h>
static bool flushFileHandle(QFile& f) {
    int fd = static_cast<int>(f.handle());
    return fd >= 0 && ::fsync(fd) == 0;
}
#endif
```

## 总结

- 主要问题领域：
  - 同步上下文与 Session 初始化门禁仍未完全达到“初始化即硬失败”的规范要求。
  - 反向 lookup 导出错误处理仍有 route-local 与 whole-row 语义混淆，会造成数据丢失。
  - `__sync_table_state` 的哈希/模加实现与规范编码不一致，影响 DiffEngine 的可信度。
  - 常规导入路径存在全表重扫，性能不符合增量维护设计。
  - 自动 profile 与跨平台传输实现仍有完整性/可移植性缺口。
- 优先修复顺序：
  1. P0：修复 H-01/H-03，确保同步上下文唯一性和 Session 硬门禁在初始化阶段闭合。
  2. P0：修复 H-04/M-02，统一规范行编码和无符号模加，保证 `table_state` 判等可信。
  3. P1：修复 H-02，调整反向 lookup miss 为 route-local 导出失败。
  4. P1：修复 M-01，把同步导入后的 `table_state` 维护改为 changeset/RowMutation 增量更新。
  5. P2：补齐 M-03 和 L-01，改善自动 profile 工作流与 Windows 可移植性。
