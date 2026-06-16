# **深度研究报告：基于 Qt 与 SQLite 的星型拓扑低带宽离线同步与可视化对比架构设计**

在当前的分布式边缘计算与离线优先（Offline-First）架构演进中，为现有的基于 Qt 和 SQLite 的单机版 Excel 批量导入导出工具赋能，使其具备高度复杂的分布式数据一致性能力，是一项极具挑战性的底层系统工程。本报告基于最新确立的系统需求边界——严苛的 2Mbps 通信带宽瓶颈、完全透明且解耦的第三方物理传输工具依赖、严格的星型拓扑（Star Topology）路由规则，以及类似于 Beyond Compare 的深度可视化差异比对与内存级事务确认机制，进行详尽的底层架构剖析与技术路径推演。通过对全球范围内顶级开源数据库同步引擎与 Qt 图形渲染框架的深度剖析，本报告旨在为该数据同步架构的重构提供兼具理论严谨性与工业落地价值的设计蓝图。

## **全球开源同步范式与可视化对比项目深度解构**

在全球开源生态中，针对 SQLite 的离线同步、分布式合并以及可视化差异对比（Diffing），已经衍生出多条截然不同的技术路线。深入剖析这些经典项目的底层架构，对于规避当前系统设计中的潜在技术债务具有决定性的指导意义。

### **数据库层面的分布式同步开源引擎**

针对边缘节点与中心节点的同步需求，业界存在多种成熟的解决方案，其核心差异在于对网络状态的假设以及对合并冲突处理权属的界定。  
SymmetricDS 是一个历史悠久且功能完备的企业级星型拓扑同步系统，专门为异构数据库和多节点边缘网络设计1。从底层实现机制来看，SymmetricDS 采用的是基于触发器（Trigger-based）的变更数据捕获（Change Data Capture, CDC）机制2。它通过向业务表中动态注入特定的 SQLite 触发器，将数据的变更日志（包括旧值和新值）异步记录到专门的系统影子表中。系统随后通过后台线程将这些变更记录打包为批次，并根据路由规则分发到星型拓扑的其他节点1。对于当前项目而言，SymmetricDS 最具参考价值的在于其高度结构化的冲突解决策略体系。在星型拓扑的双向同步中，如果主节点和边缘节点同时修改了同一条记录，SymmetricDS 允许配置丰富的解决器，例如“节点优先级（Node Priority）”机制，这与当前需求中“子节点 B 所同步的指定数据优先级最高，覆盖子节点 C”的逻辑高度契合5。此外，AMPLI-SYNC 项目同样提供了极具参考价值的边缘到中心同步框架，其明确定义了“客户端权威（Client Authority）”与“服务器权威（Server Authority）”的概念，为我们在中心节点 A 判定数据优先级时提供了理论范式7。  
近年来，基于无冲突复制数据类型（Conflict-free Replicated Data Types, CRDTs）的 SQLite 扩展成为了研究热点，其中以 sqlite-sync 及其核心底层库 cr-sqlite 为标杆8。cr-sqlite 作为 SQLite 的一个运行时可加载扩展，通过 C 语言级别的底层拦截，将传统的关系型表自动升级为 CRDT 结构。底层引擎为每一列自动维护复杂的逻辑时钟（Logical Clocks）和版本向量11。然而，深入的架构分析表明，基于 CRDT 的方案在当前约束下存在致命的副作用。首先，CRDT 为了记录因果历史，其元数据会显著膨胀数据库文件的体积，这在仅有 2Mbps 的低带宽环境下，极易引发传输通道的物理拥塞11。其次，CRDT 的核心哲学是“无冲突绝对合并”，这与当前需求中明确要求的“Beyond Compare 样式的人工干预对比”、“选择性丢弃（放弃）”以及“子节点 B 的绝对特权覆盖”在语义上是完全相悖的12。因此，全自动的 CRDT 方案必须被排除。

### **可视化数据对比与 Diff 渲染的开源实现**

在满足“比对并展示子节点 B 与中心节点 A 之间 SQLite 数据库中表数据的差异”这一核心需求方面，多个基于 Qt 或 C++ 的开源数据库管理工具提供了经典的用户界面（UI）与底层比对算法参考。  
官方血统的 DB Browser for SQLite (DB4S) 是一个极具代表性的高质量 Qt 开源项目13。深入查阅其源代码可以发现，DB4S 内部实现了一套针对 SQLite 数据类型的精细化比较逻辑，例如其底层源码中包含的 sqlite\_compare\_utf16ci 函数，能够以极高的性能处理字符串级别的差异对比15。虽然 DB4S 作为一个通用工具缺乏直接的双库差异高亮界面，但其对 SQLite 底层数据类型与 Qt 模型视图（Model/View）框架的融合方式，为当前项目提供了极其稳健的基础代码范式。  
另一个具有深度参考价值的项目是 sqlite-gui，该项目专为 Windows 平台打造，明确支持“数据库比较（Databases comparison）”功能16。sqlite-gui 能够对两个不同的 SQLite 数据库实例进行 Schema（表结构）和 Data（数据记录）的双重比对，并直观地反馈差异18。同时，备受开发者欢迎的 SQLiteStudio 在其漫长的演进历史中，虽然核心定位于数据库管理，但其社区积累了大量关于如何通过外部插件或脚本实现数据库间差异协调的工程讨论19。社区用户在需求追踪库中多次探讨了“比较数据库结构与数据，并提供更新同步的 UI（Diff/Merge databases）”的实现路径21。这些开源社区的痛点反馈与特性演进，反复验证了“在本地将远程数据库数据进行双屏并列显示、在内存中确认差异后再行提交”这一交互范式的不可或缺性。

## **星型拓扑下的极低带宽架构与物理隔离传输设计**

当前项目的网络架构处于极端的受限环境中：各节点间的通信带宽上限仅为 2Mbps（即约 250 KB/s 的极限物理传输速率），且数据库引擎自身被剥夺了直接建立网络套接字（Socket）的能力，一切数据的物理流转必须强制委托给既定的“第三方工具”进行黑盒传输。这种架构彻底阻断了采用诸如 MySQL 主从复制（Binlog Dump）或基于 Raft 共识算法的流式分布式 SQLite（如 rqlite 23）的可能性。

### **SQLite Session Extension：极简二进制序列化引擎**

为了在极低带宽下实现高效的数据捕获与传输，系统底层应直接集成 SQLite 官方维护的 Session Extension（会话扩展）引擎。与基于全表扫描比对或生成海量纯文本 SQL 语句补丁（如 sqldiff 工具生成的冗长文本流25）的传统方案不同，Session 扩展在 C/C++ 底层级别深度挂载于 SQLite 的 B-Tree 存储引擎之上，能够以纳秒级的极低开销精准捕获数据的变更状态27。  
具体而言，当通过 sqlite3session\_create() 创建会话并通过 sqlite3session\_attach() 绑定指定表后，任何对本地 SQLite 数据库的操作（Insert/Update/Delete）都会被记录入一个内存缓冲区30。在捕获周期结束时，调用 sqlite3session\_changeset() 可以提取出一个纯粹的二进制变更集（Changeset）Blob。这个变更集的体积被压缩到了理论极限：例如，对于一行包含五十个字段的记录，如果仅修改了其中一个整型字段，该二进制变更集将只编码被修改记录的主键标识符以及这唯一一个被修改字段的新旧数值，其余四十九个未变更字段的数据完全不参与网络传输31。  
结合 2Mbps 的物理带宽限制，哪怕面临上万条高频数据更新，原始二进制变更集的大小通常也仅在数百 KB 级别。为了进一步逼近极限性能，系统在将该 Blob 移交给第三方传输工具前，应引入标准的高效无损压缩算法（如在 Qt 生态中无缝集成的 zlib 库）。压缩后的二进制制品文件能够在大约一到两秒内被第三方工具穿越低速链路完成投递，从而彻底消除了带宽瓶颈造成的系统卡顿32。

### **第三方工具的握手契约与全双工异步管道**

鉴于网络传输权柄完全由独立的第三方工具掌控，核心同步系统的架构必须从“在线流式调用”重构为“基于离线制品（Artifacts）的事件驱动状态机”。数据库与传输通道之间应确立严格的异步握手契约。

| 生命周期阶段 | 核心动作与机制 | 隔离与异常处理策略 |
| :---- | :---- | :---- |
| **快照与制品生成（Snapshot & Build）** | 子节点 B 的应用层触发同步逻辑，关闭当前活动的 Session，将二进制变更集持久化到本地文件系统（如生成名为 sync\_payload\_B\_to\_A\_1715829100.bin 的压缩包）。 | 数据库连接完全释放，不持有任何长时间运行的网络锁，防止 UI 线程阻塞。 |
| **透明路由委托（Routing Delegation）** | Qt 宿主程序通过标准的进程间通信（IPC）管道或命令行唤醒指定的第三方传输工具，传入目标节点标识（中心节点 A）及制品文件的绝对路径。 | 核心系统在此阶段进入休眠等待或释放状态，将网络重试、断点续传、超时断连等复杂网络异常处理职责全部卸载给第三方工具。 |
| **事件反向唤醒（Event Callback）** | 中心节点 A 的第三方工具成功接收制品后，通过本地 Socket 或文件系统监控（如 QFileSystemWatcher）触发节点 A 的 Qt 守护进程。 | 接收端无需维持活跃的心跳检测，实现了真正的离线优先（Offline-First）零功耗待机。 |
| **仲裁与引擎注入（Arbitration & Apply）** | 节点 A 的守护进程解压制品，挂载 SQLite 引擎，并调用 sqlite3changeset\_apply() 将二进制变更合并至中心数据库。 | 在此期间触发自定义的 C++ 冲突解决回调（Conflict Callback），实现权限判定。 |

这种基于制品的异步流转机制不仅完美满足了特定第三方工具依赖的需求，还赋予了整个系统在极端恶劣网络条件下的超强抗脆弱性。

## **基于特权路由的节点仲裁与 C++ 底层冲突解决**

在确立了“子节点 B \-\> 中心节点 A \-\> 全域广播”的单向传递基石后，系统面临的最严峻挑战在于应对分布式网络中必然产生的并发写入冲突。根据明确的需求约束：“中心节点 A 将所有同步到自身的数据广播到其他节点。在同步过程中，如果子节点 C 自身的数据与子节点 B 所同步的指定数据发生冲突，以子节点 B 所同步的指定数据为准”。这一需求实质上确立了在特定一次同步会话中，**源节点（Originator Node）具有绝对的数据覆盖特权（Absolute Data Supremacy）**。

### **sqlite3changeset\_apply 的回调机制与冲突枚举**

当中心节点 A 接收到来自子节点 B 的变更集，或者子节点 C 接收到来自中心节点 A（携带着 B 的变更）的变更集并调用 sqlite3changeset\_apply() 时，如果目标数据库的当前状态与变更集中记录的预期历史状态不一致，SQLite 引擎将立即中断常规流程，并向开发者注册的 C/C++ 冲突回调函数（Conflict Resolution Callback）抛出特定的冲突类型常量29。  
在此回调框架下，系统必须对以下核心常量进行精准的路由与仲裁逻辑编写：

1. **SQLITE\_CHANGESET\_DATA 冲突仲裁**：这是最典型的“写-写冲突（Write-Write Conflict）”。当节点试图应用一个 UPDATE 或 DELETE 操作时，发现本地记录中对应字段的值与变更集携带的“旧值”不匹配，即表明在数据流转期间，本地数据被其他节点或本地操作修改过31。  
   * **应对策略**：由于需求要求子节点 B 的数据具有最高优先级，当子节点 C 捕获到该冲突（且元数据标记该变更来源于节点 B/A的推送）时，C 节点的 C++ 回调函数必须无条件返回 SQLITE\_CHANGESET\_REPLACE29。这一返回值将强迫 SQLite 引擎无视本地的修改，强制使用来自变更集的新数据覆盖本地记录，从而在物理层面兑现了“以子节点 B 为准”的业务承诺。  
2. **SQLITE\_CHANGESET\_CONFLICT 冲突仲裁**：当变更集试图 INSERT 一条新记录，但目标数据库中已经存在相同主键的记录时触发（主键冲突）31。  
   * **应对策略**：如果允许子节点各自独立生成 ID，强覆盖可能会导致严重的业务逻辑混乱。系统通常在应用层必须采用全局唯一的标识符（UUID）策略以规避此类问题。如果不可避免地触发了此冲突，在 B 节点特权模式下，依旧返回 SQLITE\_CHANGESET\_REPLACE，引擎会先物理删除本地冲突行，再重新插入 B 节点提供的数据35。  
3. **SQLITE\_CHANGESET\_FOREIGN\_KEY 冲突阻断**：当合并操作违反了关系型数据库的底层外键约束时（例如 B 节点删除了某个父项，而 C 节点中还有依赖该父项的子项）31。  
   * **应对策略**：这种约束性破裂往往意味着深层次的数据隔离问题。回调机制必须严格返回 SQLITE\_CHANGESET\_ABORT 以回滚整个事务33。在此场景下，系统应生成详细的系统日志并将异常状态推送到 UI 层，因为强行破坏关系完整性将导致系统后续出现不可恢复的读取崩溃。

通过在 C++ 层面利用这一套底层回调机制，系统实现了将复杂的分布式业务仲裁规则下沉至数据库引擎内核，极大地保障了星型拓扑多节点并发操作下数据的最终一致性（Eventual Consistency）与拓扑权威。

## **Beyond Compare 范式下的深度可视化差异比对架构**

对于终端用户而言，底层数据复制的严谨性是不可见的，用户直接感知的是系统呈现的图形用户界面（GUI）。需求明确要求在正式应用覆盖之前，必须提供一个类似于“Beyond Compare”的交互界面。用户需要在本地子节点 B 预先查看当前节点数据与从中心节点 A 获取的数据之间的具体差异，能够以极高的可读性识别增删改的具体字段，并拥有最终的“保存（Save）”或“放弃（Discard）”的事务决定权。这种高度集中的人工干预层对 Qt 前端渲染架构与内存管理机制提出了极为严峻的挑战。

### **表级概览（Table-Level Diff）：红绿标签体系的极致优化**

需求指出，首层界面应当列出数据库内的全部表，并通过右侧的标签直观反映状态：绿色表示两端表数据完全一致，红色表示存在差异。  
如果针对数十张可能包含海量记录的表逐一拉取数据并在内存中进行全文比对，即使是千兆带宽也会瞬间被压垮，更遑论当前极为脆弱的 2Mbps 通信环境。因此，表级别的差异判别必须依赖一种零负荷的元数据（Metadata）校验机制。具体实施方案如下：在中央节点 A 响应子节点 B 的“同步预检查”请求时，节点 A 不需要回传全量数据，而是为每张表计算一个高效的“哈希指纹（Hash Fingerprint）”或最高水位线（High-Water Mark）。系统可以通过维护一张额外的审计表（Audit Table），利用 SQLite 触发器在任意表发生写入时更新该审计表中的 last\_modified 时间戳或校验和。当子节点 B 发起比对时，A 仅通过第三方工具回传极小的元数据字典。子节点 B 接收后与本地的审计表进行极速内存匹配，瞬间即可驱动 UI 模型刷新表级红绿视图，实现了接近零延迟的响应体验。

### **行级差异呈现（Row-Level Diff）：克服 QTreeView 的性能陷阱**

当用户双击标记为红色的表行时，界面将进入深度的行级比对视图。需求图例中明确标注了使用 QTreeView 的格式来展现左右两屏（左侧为子节点 B，右侧为中心节点 A）对等并排的数据对比关系。这一需求在实际的 Qt 工程实现中隐藏着巨大的性能陷阱。  
在 Qt 的 Model/View 架构中，如果涉及呈现数以万计甚至百万计的数据行，直接使用 QTreeView 并构建具有层级关系的树形结构模型，会引发致命的渲染延迟。行业研究与基准测试（Benchmark）明确指出：针对百万级别的数据展示，QTableView 能够利用其严格的二维网格虚拟化渲染机制，在不到 0.1 秒内完成绘制；而 QTreeView 往往需要耗费数十倍的时间（长达数秒甚至更久）去递归计算节点的高度与折叠状态，甚至在快速滚动时导致程序假死36。  
因此，本报告提出一种“以表代树”的折中与优化渲染架构：**在底层模型中摒弃真实的树形数据结构嵌套，采用 QTableView 配合从 QAbstractTableModel 派生的自定义平面化数据模型，并在视觉表现上通过重写自定义委托（Delegate）来“模拟”树形结构的缩进与展开视觉效果**。如果在强烈要求下必须使用 QTreeView，则必须强制在视图层设置 setUniformRowHeights(true) 属性以旁路高度计算瓶颈，并确保底层的 QAbstractItemModel 在重写相关方法时严格优化 parent() 和 index() 的查找算法，且在根节点层面即启用懒加载（Lazy Loading / fetchMore 机制）36。  
为实现面对面（Side-by-Side）的精准对比，我们需要在主界面中水平放置两个同步锁定的视图容器38。

1. **视图联动同步**：通过拦截左侧视图的 QScrollBar::valueChanged 信号，强制绑定右侧视图的滚动位置，反之亦然，确保两端视口始终处于同一条数据的平面39。  
2. **空间占位与幽灵行对齐**：当某一侧发生独占的数据增删时（例如左侧节点 B 存在记录而右侧节点 A 没有），为保持比对行的一一对应，底层数据模型必须能够动态向缺失的一侧注入“幽灵空行（Ghost Rows）”，并在 UI 层通过空白填充来维持严格的横向视线锚定39。

### **基于 QStyledItemDelegate 的高对比度视觉引擎**

差异数据的可读性完全依赖于视图层的颜色高亮反馈。在 Qt 中，直接修改模型的 data 角色并不能满足高复杂度的渲染需求。系统应通过继承 QStyledItemDelegate 并深度重写其 paint() 虚函数来接管每一枚单元格的渲染管线40。  
在自定义数据模型中，除了通过 Qt::DisplayRole 返回基础数据文本外，还需要设立一个自定义角色（例如 CustomDiffStatusRole），用于向上层反馈当前记录行或单元格的具体状态（如 Status\_Added, Status\_Deleted, Status\_Modified\_Conflict）。  
当 Qt 的事件循环触发 paint() 重绘时，委托代码将拦截这些状态枚举。对于被判定为存在差异的区域，直接绕过默认系统画笔，调用 painter-\>fillRect() 在该单元格的几何边界（option.rect）内填充警告色（例如浅红色代表冲突，浅绿色代表新增）40。为了保证在用户鼠标点击选中某一行（option.state & QStyle::State\_Selected 被触发）时数据依然清晰可辨，系统必须利用 option.palette.highlightedText() 动态反转字体前景色，从而提供不逊色于专业版 Beyond Compare 工具的卓越交互体验42。

## **本地内存缓冲区机制与事务化生命周期演进**

在行级差异对比界面中，需求定义了一个极端关键的中间态（Intermediate State）：在用户点击“保存（Save）”或“放弃（Discard）”之前，从右侧（中心节点 A）同步到左侧（子节点 B）的数据，必须仅存在于“本地内存缓存中（数据缓存到本地内存中）”，绝对不能提前污染子节点 B 的持久化 SQLite 数据库。  
这一需求要求我们将传统的“直接落盘”的数据操作模型彻底切断，重构为具备“撤销/重做”能力的“三明治层（Sandwich Layer）”架构体系。

### **基于双模型的内存挂载机制**

当子节点 B 决定比对并拉取中心节点 A 的数据时，A 通过极低带宽网络发送的是基于请求表结构的轻量级数据切片或变更制品。子节点 B 的宿主进程在内存中实例化一个 C++ std::vector 支持的内存缓冲模型（Memory Buffer Model）。  
此时，界面左侧的 QTreeView/QTableView 并非直接挂载 SQLite 数据库表。相反，左侧视图绑定的是一个“代理模型（Proxy Model）”。该代理模型的原始数据源自本地 SQLite 库，但在运行期间，如果用户通过 UI 将右侧 A 的某些数据通过同步操作“拉入”了左侧 B，这些变动将仅作为“修改元组（Mutation Tuples）”追加到内存代理模型中。视图会即时刷新，红绿色高亮会实时变换，呈现出预合并后的效果。在此整个交互生命周期内，本地 SQLite 数据库文件由于未执行任何 COMMIT 事务操作，依然保持在原有的绝对隔离状态。

### **“保存”与“放弃”的终局仲裁**

一旦用户在经过审慎的可视化比对后，点击界面上的“放弃（Discard）”按钮，系统的响应逻辑极其轻量化：宿主进程仅仅需要销毁内存中的 Memory Buffer Model 实例对象并执行垃圾回收，断开代理模型，界面瞬间恢复到初始比对状态，数据库层未发生任何物理写操作，实现了零成本的逻辑回滚。  
而当用户确认差异无误并点击“保存（Save）”按钮时，系统进入最为关键的“持久化落盘阶段”。此时，内存中积攒的所有确认修改的“元组”，将被构建为一条连续的数据库指令执行流。系统可以利用 SQLite 原生的显式事务开启命令 BEGIN IMMEDIATE TRANSACTION 获取独占写锁（防止期间发生额外的并发污染），将内存缓冲区中的数据转换为对应的 INSERT、UPDATE 语句，直接覆盖写入子节点 B 的持久化本地数据库中。或者更高级地，系统可以在内存中生成一个内部的伪变更集，在内存中直接调用 sqlite3changeset\_apply() 将缓存合并到磁盘。完成操作后，提交 COMMIT 释放锁资源。  
随后，子节点 B 正式成为该批数据的权威拥有者。系统随即开启全新的 Session 跟踪，抓取这部分已保存的物理变更，生成属于节点 B 的强优先权二进制文件包，最终交由第三方传输工具打包推送至中心节点 A，完成星型拓扑下一个闭环轮次的权威重塑。

## **结论**

本系统架构基于 Qt 与 SQLite 的深厚底层特性，针对严格受限的 2Mbps 通信带宽、完全隔离的第三方网络传输工具以及错综复杂的星型拓扑冲突处理，提炼出了一套高度优化的异步离线同步与深度可视化解决方案。  
摒弃不切实际的全连接网络协议与重负荷的 CRDT 数据结构，直接利用 SQLite 原生 Session 扩展构建的二进制变更具体现了极简主义的数据序列化哲学，完美适配了带宽枯竭型的边界约束。在此基础之上，通过在 C/C++ 底层重写 sqlite3changeset\_apply 的回调机制，实现了中心化星型网络中对特权节点数据的绝对权威维护。同时，凭借 Qt 强大的 Model/View 架构体系与对 QStyledItemDelegate 的精细化控制，系统不仅绕过了展示百万级复杂数据时可能触发的渲染死锁，还成功在内存维度构建了隔离层，实现了类似于 Beyond Compare 的直观视觉比对与无损的事务撤销机制。这一从数据库内核延伸至终端 GUI 的全链路贯通架构，不仅具有极高的技术抗压能力，也为同类恶劣网络环境下的边缘计算同步提供了不可多得的工业级实现范式。

#### **Works cited**

1. Keeping Databases in Sync “Open Source Style” using SymmetricDS | by Chris Henson | Data Weekly by Jumpmind | Medium, accessed June 16, 2026, [https://medium.com/data-weekly/keeping-databases-in-sync-open-source-style-using-symmetricds-d2a2cec0ff1c](https://medium.com/data-weekly/keeping-databases-in-sync-open-source-style-using-symmetricds-d2a2cec0ff1c)  
2. Top 12 Database Replication Tools for 2025 \- Streamkap, accessed June 16, 2026, [https://streamkap.com/resources-and-guides/database-replication-tools](https://streamkap.com/resources-and-guides/database-replication-tools)  
3. JETIR Research Journal \- IJNRD, accessed June 16, 2026, [https://ijnrd.org/papers/IJNRD2504210.pdf](https://ijnrd.org/papers/IJNRD2504210.pdf)  
4. SymmetricDS scheduled sync \- Database Administrators Stack Exchange, accessed June 16, 2026, [https://dba.stackexchange.com/questions/110640/symmetricds-scheduled-sync](https://dba.stackexchange.com/questions/110640/symmetricds-scheduled-sync)  
5. SymmetricDS 3.15 User Guide, accessed June 16, 2026, [https://symmetricds.sourceforge.net/doc/3.15/html/user-guide.html](https://symmetricds.sourceforge.net/doc/3.15/html/user-guide.html)  
6. About \- SymmetricDS, accessed June 16, 2026, [https://symmetricds.org/about/](https://symmetricds.org/about/)  
7. GitHub \- AMPLIFIER-sp-z-o-o/ampli-sync: Offline-first data sync framework: bidirectional synchronization between SQLite and MS SQL, MySQL, Oracle, and PostgreSQL., accessed June 16, 2026, [https://github.com/sqlite-sync/SQLite-sync.com](https://github.com/sqlite-sync/SQLite-sync.com)  
8. sqliteai/sqlite-sync-dev: SQLiteSync is a local-first SQLite extension using CRDTs for seamless, conflict-free data sync and real-time collaboration across devices. \- GitHub, accessed June 16, 2026, [https://github.com/sqliteai/sqlite-sync-dev](https://github.com/sqliteai/sqlite-sync-dev)  
9. GitHub \- sqliteai/sqlite-sync: CRDT-based offline-first sync for SQLite. Syncs automatically with SQLite Cloud, PostgreSQL, and Supabase. No conflicts, no data loss, no backend to build. For offline-first apps and AI agents., accessed June 16, 2026, [https://github.com/sqliteai/sqlite-sync](https://github.com/sqliteai/sqlite-sync)  
10. SQLite Sync \- Offline-first CRDT sync for SQLite, accessed June 16, 2026, [https://www.sqlite.ai/sqlite-sync](https://www.sqlite.ai/sqlite-sync)  
11. GitHub \- vlcn-io/cr-sqlite: Convergent, Replicated SQLite. Multi-writer and CRDT support for SQLite, accessed June 16, 2026, [https://github.com/vlcn-io/cr-sqlite](https://github.com/vlcn-io/cr-sqlite)  
12. The Architecture Of Local-First Web Development \- Smashing Magazine, accessed June 16, 2026, [https://www.smashingmagazine.com/2026/05/architecture-local-first-web-development/](https://www.smashingmagazine.com/2026/05/architecture-local-first-web-development/)  
13. GitHub \- sqlitebrowser/sqlitebrowser: Official home of the DB Browser for SQLite (DB4S) project. Previously known as "SQLite Database Browser" and "Database Browser for SQLite". Website at, accessed June 16, 2026, [https://github.com/sqlitebrowser/sqlitebrowser](https://github.com/sqlitebrowser/sqlitebrowser)  
14. Database (DB) Browser for SQLite (DB4S) \- Veterans Affairs, accessed June 16, 2026, [https://www.oit.va.gov/Services/TRM/ToolPage.aspx?tid=8800](https://www.oit.va.gov/Services/TRM/ToolPage.aspx?tid=8800)  
15. sqlitebrowser/src/sqlitedb.cpp at master \- GitHub, accessed June 16, 2026, [https://github.com/sqlitebrowser/sqlitebrowser/blob/master/src/sqlitedb.cpp](https://github.com/sqlitebrowser/sqlitebrowser/blob/master/src/sqlitedb.cpp)  
16. My SQLite editor for Windows \- Reddit, accessed June 16, 2026, [https://www.reddit.com/r/sqlite/comments/nldmtn/my\_sqlite\_editor\_for\_windows/](https://www.reddit.com/r/sqlite/comments/nldmtn/my_sqlite_editor_for_windows/)  
17. Home · little-brother/sqlite-gui Wiki \- GitHub, accessed June 16, 2026, [https://github.com/little-brother/sqlite-gui/wiki](https://github.com/little-brother/sqlite-gui/wiki)  
18. sqlite-gui: The Lightweight, Powerful, and User-Friendly SQLite Manager for Windows, accessed June 16, 2026, [https://christianbaghai.medium.com/sqlite-gui-the-lightweight-powerful-and-user-friendly-sqlite-manager-for-windows-ca793b692d5c](https://christianbaghai.medium.com/sqlite-gui-the-lightweight-powerful-and-user-friendly-sqlite-manager-for-windows-ca793b692d5c)  
19. SQLiteStudio · pawelsalawa/letos Wiki \- GitHub, accessed June 16, 2026, [https://github.com/pawelsalawa/sqlitestudio/wiki/SQLiteStudio/4cf092272e870d82a71b3761114a907c0d7e6755](https://github.com/pawelsalawa/sqlitestudio/wiki/SQLiteStudio/4cf092272e870d82a71b3761114a907c0d7e6755)  
20. SQLiteStudio vs. dbKoda Comparison \- SourceForge, accessed June 16, 2026, [https://sourceforge.net/software/compare/SQLiteStudio-vs-dbKoda/](https://sourceforge.net/software/compare/SQLiteStudio-vs-dbKoda/)  
21. Diff/Merge databases · Issue \#4102 · pawelsalawa/sqlitestudio \- GitHub, accessed June 16, 2026, [https://github.com/pawelsalawa/sqlitestudio/issues/4102](https://github.com/pawelsalawa/sqlitestudio/issues/4102)  
22. Database comparator plugin · Issue \#3357 · pawelsalawa/letos \- GitHub, accessed June 16, 2026, [https://github.com/pawelsalawa/sqlitestudio/issues/3357](https://github.com/pawelsalawa/sqlitestudio/issues/3357)  
23. rqlite 9.0: Real-time Change Data Capture for Distributed SQLite \- Philip O'Toole, accessed June 16, 2026, [https://philipotoole.com/rqlite-9-0-real-time-change-data-capture-for-distributed-sqlite/](https://philipotoole.com/rqlite-9-0-real-time-change-data-capture-for-distributed-sqlite/)  
24. Features and Use Cases | rqlite, accessed June 16, 2026, [https://rqlite.io/docs/features/](https://rqlite.io/docs/features/)  
25. Release History Of SQLite, accessed June 16, 2026, [https://www.sqlite.org/changes.html](https://www.sqlite.org/changes.html)  
26. Using SQLite as storage for web server static content | Hacker News, accessed June 16, 2026, [https://news.ycombinator.com/item?id=41963996](https://news.ycombinator.com/item?id=41963996)  
27. xqlite \- Hex.pm, accessed June 16, 2026, [https://hex.pm/packages/xqlite](https://hex.pm/packages/xqlite)  
28. src/sqlite3/sqlite3.h · preprod · Jérémie Dudouet / tkn-lib \- GitLab, accessed June 16, 2026, [https://gitlab-preprod.in2p3.fr/dudouet/tkn-lib/-/blob/preprod/src/sqlite3/sqlite3.h?ref\_type=heads](https://gitlab-preprod.in2p3.fr/dudouet/tkn-lib/-/blob/preprod/src/sqlite3/sqlite3.h?ref_type=heads)  
29. SQLite Session Module C/C++ Interface, accessed June 16, 2026, [https://sqlite.org/session.html](https://sqlite.org/session.html)  
30. The Session Extension \- SQLite, accessed June 16, 2026, [https://sqlite.org/sessionintro.html](https://sqlite.org/sessionintro.html)  
31. SQLite | Node.js v26.3.0 Documentation, accessed June 16, 2026, [https://nodejs.org/api/sqlite.html](https://nodejs.org/api/sqlite.html)  
32. fxdeniz/NeSync: Local file sync & backups \- GitHub, accessed June 16, 2026, [https://github.com/fxdeniz/NeSync](https://github.com/fxdeniz/NeSync)  
33. sqlite3session.c \- Scipion, accessed June 16, 2026, [https://scipion.cnb.csic.es/downloads/scipion/software/external/SQLite-1a584e49/ext/session/sqlite3session.c](https://scipion.cnb.csic.es/downloads/scipion/software/external/SQLite-1a584e49/ext/session/sqlite3session.c)  
34. sqlite package \- crawshaw.io/sqlite \- Go Packages, accessed June 16, 2026, [https://pkg.go.dev/crawshaw.io/sqlite](https://pkg.go.dev/crawshaw.io/sqlite)  
35. SQLite3 Red/System binding \- GitHub Gist, accessed June 16, 2026, [https://gist.github.com/Oldes/a806110a64a0b44a136cdd3a25af1671](https://gist.github.com/Oldes/a806110a64a0b44a136cdd3a25af1671)  
36. QTreeView with lots of items is really slow. Can it be optimised or is something buggy?, accessed June 16, 2026, [https://forum.qt.io/topic/159449/qtreeview-with-lots-of-items-is-really-slow-can-it-be-optimised-or-is-something-buggy/18](https://forum.qt.io/topic/159449/qtreeview-with-lots-of-items-is-really-slow-can-it-be-optimised-or-is-something-buggy/18)  
37. QTreeView with lots of items is really slow. Can it be optimised or is something buggy?, accessed June 16, 2026, [https://forum.qt.io/topic/159449/qtreeview-with-lots-of-items-is-really-slow-can-it-be-optimised-or-is-something-buggy](https://forum.qt.io/topic/159449/qtreeview-with-lots-of-items-is-really-slow-can-it-be-optimised-or-is-something-buggy)  
38. Diff \- 8455ed26f7242212444dc2fa44f0d65c3d0e8adf^1..8455ed26f7242212444dc2fa44f0d65c3d0e8adf \- platform/external/qt \- Git at Google \- Android GoogleSource, accessed June 16, 2026, [https://android.googlesource.com/platform/external/qt/+/8455ed26f7242212444dc2fa44f0d65c3d0e8adf%5E1..8455ed26f7242212444dc2fa44f0d65c3d0e8adf/](https://android.googlesource.com/platform/external/qt/+/8455ed26f7242212444dc2fa44f0d65c3d0e8adf%5E1..8455ed26f7242212444dc2fa44f0d65c3d0e8adf/)  
39. Link rows from two QTableView for visual comparaison \- Stack Overflow, accessed June 16, 2026, [https://stackoverflow.com/questions/46051554/link-rows-from-two-qtableview-for-visual-comparaison](https://stackoverflow.com/questions/46051554/link-rows-from-two-qtableview-for-visual-comparaison)  
40. c++ \- Qt \- QTableview row color with delegates \- Stack Overflow, accessed June 16, 2026, [https://stackoverflow.com/questions/42180303/qt-qtableview-row-color-with-delegates](https://stackoverflow.com/questions/42180303/qt-qtableview-row-color-with-delegates)  
41. Painting the background of a QTableView (with a custom QStyledItemDelegate), accessed June 16, 2026, [https://stackoverflow.com/questions/56735853/painting-the-background-of-a-qtableview-with-a-custom-qstyleditemdelegate](https://stackoverflow.com/questions/56735853/painting-the-background-of-a-qtableview-with-a-custom-qstyleditemdelegate)  
42. \[SOLVED\] \- QStyledItemDelegate \- set text color when row is selected | Qt Forum, accessed June 16, 2026, [https://forum.qt.io/topic/41463/solved-qstyleditemdelegate-set-text-color-when-row-is-selected](https://forum.qt.io/topic/41463/solved-qstyleditemdelegate-set-text-color-when-row-is-selected)
