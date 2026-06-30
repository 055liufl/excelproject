#include "AutoProfileBuilder.h"

#include "dbridge/Errors.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

// ============================================================================
// AutoProfileBuilder.cpp — 自省 → 单表 Profile 草稿 的生成逻辑实现
// ============================================================================
//
// 【本文件实现什么】AutoProfileBuilder 的两个成员：
//   · build()  —— 把一张表的自省信息（TableInfo）翻译成内存 Profile（ProfileSpec）。
//   · toJson() —— 把内存 Profile 序列化成可被 ProfileLoader 重新载入的 JSON 文本。
//   类的整体职责、在 ETL 管线中的位置与协作者，详见配套头文件 AutoProfileBuilder.h。
//
// 【生成逻辑总览（自省 → 字段映射 → 默认规则）】
//   输入是数据库自省得到的「表结构事实」：列名、声明类型、主键标志、是否自增、
//   是否生成列、非空约束、以及唯一索引集合。build() 据此做三步推断：
//     Step 1 推断「冲突键」conflict.columns —— 决定 UPSERT 靠哪几列判定「插入 or 更新」。
//             这是整份草稿能否「可执行」的关键：没有冲突键就无法做 UPSERT。
//     Step 2 推断「列映射」route.columns —— 哪些 DB 列要参与导入、各列对应哪个 Excel
//             表头、各列该挂哪些校验器（validators）。
//     Step 3 推断「默认导出排序」exportSpec.orderBy —— 取冲突键第一列，给个稳定默认排序。
//   下面在各步骤处再就「为什么这么推断」逐点说明。
// ============================================================================

namespace dbridge::detail {

// ── build：自省结果 → 单表 ProfileSpec ───────────────────────────────────────
// 形参/返回值/错误模式的完整契约见头文件；这里专注「推断规则为什么这样写」。
bool AutoProfileBuilder::build(const TableInfo& table, ProfileSpec* out, QString* err) {
    // —— 顶层固定字段：单表场景下这几项可直接由表名派生，无需推断 ——
    // name="auto_<表名>"：加 "auto_" 前缀以示「机器生成的草稿」，避免与人工 Profile 撞名。
    out->name = QStringLiteral("auto_") + table.name;
    // sheet 默认 = 表名：自省时并不知道用户的真实工作表名，先用表名占位，由用户改。
    out->sheet = table.name;
    // headerRow=1：约定表头在第一行，这是最常见的 Excel 习惯（1 基行号）。
    out->headerRow = 1;
    // 单表草稿固定为 SingleTable 模式（本类只生成单表 Profile，不涉及多表/混合）。
    out->mode = ProfileMode::SingleTable;

    // ── Step 1：选取「冲突键」（UPSERT 用来判定插入还是更新的那组列）──────────
    // Step 1: Choose conflict columns
    // Priority: non-autoincrement composite PK, then smallest UNIQUE index
    // 【翻译保留原注释】第一步：选择冲突列。
    //   优先级：非自增的（复合）主键 → 否则取列数最少的 UNIQUE 索引。
    //
    // 【为什么不能简单地“拿主键当冲突键”】SQLite 里 `INTEGER PRIMARY KEY` 往往是
    //   自增的 rowid 别名（autoIncrement）。这种代理主键在 Excel 里通常根本没有对应列
    //   （用户表格里不会去填自增 id），拿它当冲突键就无法把 Excel 行与库行对上号。
    //   因此推断时要刻意「跳过自增主键」，转而寻找具有业务含义、可重复匹配的键。
    QStringList conflictCols;

    // —— 先收集主键信息：哪些列是主键、其中是否含自增列 ——
    // Collect primary key columns
    // 【翻译保留原注释】收集主键列。
    QStringList pkCols;  // 所有被标为 primaryKey 的列名（保持自省给出的顺序）
    bool hasAutoInc = false;  // 主键中是否「至少有一列」是自增的
    for (const auto& col : table.columns) {
        if (col.primaryKey) {
            pkCols.append(col.name);
            if (col.autoIncrement)
                hasAutoInc = true;
        }
    }

    // —— 按优先级三分支决定冲突键 ——
    if (!pkCols.isEmpty() && !hasAutoInc) {
        // 情形 A：有主键且「无任何自增列」→ 直接用整组主键列作冲突键。
        // 这类主键（如自然主键、人工指定的 code 列、复合主键）是有业务含义、
        // Excel 里能填出来的，最适合做 UPSERT 冲突键。
        // Non-autoincrement PK: use all PK columns
        // 【翻译保留原注释】非自增主键：使用全部主键列。
        conflictCols = pkCols;
    } else if (!pkCols.isEmpty() && hasAutoInc && pkCols.size() > 1) {
        // 情形 B：复合主键且其中含自增列（pkCols.size()>1 表示是「复合」而非单列代理键）。
        // 此时整组主键里既有自增的代理列、也有有业务含义的列；只取「非自增」的那些列
        // 作冲突键——把无意义的自增 id 剔除，保留可在 Excel 里匹配的部分。
        // Composite PK with autoincrement: use non-autoincrement PK columns
        // 【翻译保留原注释】带自增的复合主键：使用其中非自增的主键列。
        for (const auto& col : table.columns) {
            if (col.primaryKey && !col.autoIncrement)
                conflictCols.append(col.name);
        }
    } else {
        // 情形 C：其余情况（无主键，或主键就是单列自增代理键）→ 退而求其次，
        // 从 UNIQUE 索引里找冲突键，并取「列数最少」的那个唯一索引。
        // 为什么挑列数最少：键越短，用户在 Excel 里要填的匹配列越少、越省事，
        //   也更接近「自然业务键」（单列唯一键通常就是 code/编号一类）。
        // Try smallest UNIQUE index
        // 【翻译保留原注释】尝试列数最少的 UNIQUE 索引。
        int minSize = INT_MAX;  // 维护「目前见过的最小唯一索引列数」
        for (const auto& idx : table.indexes) {
            if (!idx.unique)
                continue;  // 只看 UNIQUE 索引，非唯一索引不能用作 UPSERT 冲突目标
            // 注意：这里只比较列数取最短，命中即覆盖 conflictCols。
            // 【一处不直观/已知局限】SchemaCatalog 里 IndexInfo 标有 partial 字段，且其
            //   H-02 fix 提醒「部分索引（partial）不是合法的 UPSERT 冲突目标」（SQLite 要求
            //   ON CONFLICT 子句匹配的是「非部分」的 UNIQUE/PRIMARY KEY）。但此处的筛选
            //   只判断了 idx.unique，并未排除 idx.partial。也就是说，生成的草稿冲突键有可能
            //   落在某个 partial 唯一索引上——这正属于「需要用户在草稿上复核修正」的范畴，
            //   后续 ProfileValidator 在正式载入时也会再把关。（仅说明现状，不改代码。）
            if (idx.columns.size() < minSize) {
                minSize = idx.columns.size();
                conflictCols = idx.columns;
            }
        }
    }

    // M-03 fix: instead of returning false when no conflict key is available, produce a
    // draft profile with executable=false and a descriptive issue entry. This lets callers
    // surface the mapping draft (column list) for the user to review and complete.
    // *err is NOT set here — a draft is not an error; callers check out->executable.
    // 【翻译保留原注释 + 展开】M-03 fix：当「找不到任何可用冲突键」时，不再直接返回
    //   false 让整个生成失败，而是产出一份「草稿」——把 out->executable 置 false，并往
    //   out->issues 追加一条带错误码的可读描述。这样调用方仍能拿到「列映射草稿」展示给
    //   用户，让用户自己去补冲突键 / 加唯一约束后再用。
    //   关键：此处「不」向 *err 写内容——草稿不算错误；调用方应改为检查 out->executable
    //   来区分「可直接执行」与「仅草稿、待人工补全」。
    if (conflictCols.isEmpty()) {
        out->executable = false;  // 标记：这只是草稿，尚不可直接执行导入
        // issues 里拼一条「错误码 + 上下文 + 修复建议」：表既无主键也无唯一约束，
        // 请用户加唯一约束、或手动在 conflict.columns 指定冲突键。
        out->issues.append(QString::fromLatin1(err::E_PROFILE_NO_CONFLICT_KEY) +
                           QStringLiteral(": table '") + table.name +
                           QStringLiteral("' has no PRIMARY KEY or UNIQUE constraint; "
                                          "please add a unique constraint or specify "
                                          "conflict.columns manually"));
        // Fall through: populate column list in the draft so the caller can inspect the
        // field mapping and decide which columns to use as a conflict key.
        // 【翻译保留原注释】此处「不 return」，继续往下执行——照常把列映射填进草稿，
        //   好让调用方查看字段映射、并从中挑选哪些列充当冲突键。
    }

    // —— 准备这条（唯一的）单表路由：目标表 + 上面推断出的冲突键 ——
    RouteSpec route;
    route.table = table.name;
    route.conflict.columns = conflictCols;  // 可能为空（草稿情形）；导出排序处会再判空

    // ── Step 2：挑选「可写列」并生成各列的映射与校验器 ─────────────────────────
    // Step 2: Select writable columns
    // 【翻译保留原注释】第二步：选择可写列。
    //
    // 【列映射的两条默认规则】
    //   (a) 跳过两类「不该由 Excel 写入」的列：自增主键列、生成列（generated）——
    //       它们的值由数据库自己产生，Excel 不提供也不该覆盖。
    //   (b) 对保留的列，默认「Excel 表头名 = DB 列名」（source=col.name）——
    //       这是最省事的默认对应，用户若表头不同名，再到草稿里改 source 即可。
    for (const auto& col : table.columns) {
        if (col.autoIncrement)
            continue;  // skip autoincrement PK
                       // 【翻译保留原注释】跳过自增主键（其值由库自增，Excel 不该提供）。
        if (col.generated)
            continue;  // skip generated columns
                       // 【翻译保留原注释】跳过生成列（其值由表达式算出，不能直接写入）。

        ColumnSpec cs;
        cs.dbColumn = col.name;  // 写库目标：DB 列名
        cs.source = col.name;    // Excel header = column name
                               // 【翻译保留原注释】默认 Excel 表头名 = DB 列名（同名映射）。

        // —— 按列的声明类型，自动附加合适的校验器（validators）token ——
        // Add validators based on type
        // 【翻译保留原注释】依据列类型添加校验器。
        //
        // 【为什么用「子串包含」而不是精确匹配类型】SQLite 是「类型亲和性（type
        //   affinity）」体系——列的声明类型是自由文本，写法五花八门（INTEGER、INT、
        //   BIGINT、VARCHAR、DECIMAL(10,2) ……）。这里把声明类型转大写后用「包含某关键字」
        //   做亲和性归类，正好对上 SQLite 的亲和性规则（含 INT→整数，含 REAL/NUMERIC/
        //   DECIMAL→数值等），比逐一枚举具体类型名更稳健。
        QString typeUp = col.declaredType.toUpper();
        // notNull 校验器：列有 NOT NULL 约束、且「不属于冲突键」时才加。
        // 为什么排除冲突键列：冲突键在导入时另有专门处理路径，不在这里重复挂非空校验。
        if (col.notNull && !conflictCols.contains(col.name)) {
            cs.validatorTokens.append(QStringLiteral("notNull"));
        }
        // 类型亲和性 → 校验器的映射（注意分支顺序：INT 优先，避免被后面的 NUMERIC 抢走）：
        if (typeUp.contains(QStringLiteral("INT"))) {
            // 含 "INT"（INTEGER/INT/BIGINT/...）→ 整数校验。
            cs.validatorTokens.append(QStringLiteral("int"));
        } else if (typeUp.contains(QStringLiteral("REAL")) ||
                   typeUp.contains(QStringLiteral("NUMERIC")) ||
                   typeUp.contains(QStringLiteral("DECIMAL"))) {
            // 含 REAL/NUMERIC/DECIMAL → 十进制小数校验。
            cs.validatorTokens.append(QStringLiteral("decimal"));
        } else if (typeUp.contains(QStringLiteral("DATE"))) {
            // 含 "DATE"（DATE/DATETIME 等）→ 日期校验，给出常见默认格式 yyyy-MM-dd。
            // 这只是「猜测的默认」，用户可在草稿里改成实际使用的日期格式。
            cs.validatorTokens.append(QStringLiteral("date:yyyy-MM-dd"));
        }
        // 说明：文本类（TEXT/VARCHAR/CHAR…）不挂类型校验器——文本几乎接受一切输入，
        //   无需也不宜默认限制；如需长度/正则校验，由用户在草稿上自行补充。

        route.columns.append(cs);
    }

    out->routes.append(route);  // 单表 Profile：routes 里就这一条路由

    // ── Step 3：默认导出排序 ─────────────────────────────────────────────────
    // Export spec
    // 【翻译保留原注释】导出配置。
    // 取冲突键的「第一列」作默认 orderBy，给导出一个稳定、可复现的行序
    //   （冲突键通常是主键/唯一键，按它排序天然不重复、顺序确定）。
    //   若冲突键为空（草稿情形）则不设排序，留空交给用户。
    if (!conflictCols.isEmpty()) {
        out->exportSpec.orderBy.append(conflictCols.first());
    }

    // Return true for both executable and draft profiles; callers check out->executable.
    // 【翻译保留原注释】无论「可执行」还是「草稿」都返回 true；调用方靠 out->executable
    //   来区分二者（详见头文件返回值说明与上面的 M-03 fix）。
    return true;
}

// ── toJson：ProfileSpec → 固定字段序的缩进 JSON ─────────────────────────────
// 输出结构需与 ProfileLoader 的输入格式对齐，以便用户微调后能被重新载入。
// 字段插入顺序在此被刻意固定，以获得稳定、可 diff、可测试的输出。
QString AutoProfileBuilder::toJson(const ProfileSpec& profile) const {
    QJsonObject root;  // JSON 根对象（QJsonObject 内部按键名有序，但我们靠插入顺序读着舒服）

    // —— 顶层标量字段：profileName / sheet / headerRow / mode ——
    root.insert(QStringLiteral("profileName"), profile.name);
    root.insert(QStringLiteral("sheet"), profile.sheet);
    root.insert(QStringLiteral("headerRow"), profile.headerRow);
    // mode 在本类里恒为单表，故直接写死字符串 "singleTable"（与 Loader 约定的取值一致）。
    root.insert(QStringLiteral("mode"), QStringLiteral("singleTable"));
    // —— 仅当这是「草稿」（不可执行）时，才额外输出 executable=false 与 issues 列表 ——
    // 可执行的 Profile 不写这两个字段，保持输出干净；草稿则带上问题清单提示用户。
    if (!profile.executable) {
        root.insert(QStringLiteral("executable"), false);
        QJsonArray issArr;
        for (const auto& iss : profile.issues)
            issArr.append(iss);
        root.insert(QStringLiteral("issues"), issArr);
    }

    // —— 路由相关字段：单表只输出 routes[0]（table / conflict / columns）——
    if (!profile.routes.isEmpty()) {
        const RouteSpec& route = profile.routes[0];  // 单表场景：只有一条路由
        root.insert(QStringLiteral("table"), route.table);

        // conflict 子对象：{ "columns": [ ... ] }，即 UPSERT 冲突键列清单。
        QJsonObject conflictObj;
        QJsonArray conflictCols;
        for (const auto& c : route.conflict.columns)
            conflictCols.append(c);
        conflictObj.insert(QStringLiteral("columns"), conflictCols);
        root.insert(QStringLiteral("conflict"), conflictObj);

        // columns 子对象：键 = DB 列名，值 = { source, [validators] }。
        // 用「DB 列名作 JSON 键」而非数组，方便用户按列名快速定位、编辑某一列的映射。
        QJsonObject colsObj;
        for (const auto& col : route.columns) {
            QJsonObject colObj;
            colObj.insert(QStringLiteral("source"), col.source);  // 对应的 Excel 表头名
            // validators 仅在非空时才写出——没有校验器的列不必出现空数组，输出更简洁。
            if (!col.validatorTokens.isEmpty()) {
                QJsonArray va;
                for (const auto& t : col.validatorTokens)
                    va.append(t);
                colObj.insert(QStringLiteral("validators"), va);
            }
            colsObj.insert(col.dbColumn, colObj);  // 以 DB 列名为键挂入
        }
        root.insert(QStringLiteral("columns"), colsObj);
    }

    // —— export 子对象：当前只可能含 orderBy（且仅在非空时写出）——
    // 注意 export 对象「总是」会被插入（哪怕里面是空的）：保持输出结构齐整、字段恒在，
    //   便于用户知道「这里可以填导出配置」，也利于测试做结构稳定的比对。
    QJsonObject exportObj;
    if (!profile.exportSpec.orderBy.isEmpty()) {
        QJsonArray ob;
        for (const auto& s : profile.exportSpec.orderBy)
            ob.append(s);
        exportObj.insert(QStringLiteral("orderBy"), ob);
    }
    root.insert(QStringLiteral("export"), exportObj);

    // 序列化为 UTF-8 文本，采用 Indented（带缩进/换行）风格，便于人阅读与微调。
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

}  // namespace dbridge::detail
