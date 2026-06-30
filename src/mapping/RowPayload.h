#pragma once
// ============================================================================
// mapping/RowPayload.h — 转发头（别名 / 兼容垫片）
// ============================================================================
//
// 【单一职责】
//   本文件不定义任何类型，只是把公共头 include/dbridge/RowPayload.h「转发」进来，
//   让 mapping 子系统内部用短路径 #include "RowPayload.h" 即可拿到 RowContext /
//   RoutePayload（它们的真正定义在 dbridge/RowPayload.h，已在那里详尽注释）。
//
// 【为什么需要它（看似多余的一行 include）】
//   · 路径便利：mapping 下的 Mapper/FkInjector/BatchUniqueness 等大量引用这两个结构，
//     用本地相对路径比每处都写 "dbridge/RowPayload.h" 更简洁、对包含目录更宽容。
//   · 历史兼容：RowContext/RoutePayload 早期可能位于 mapping 局部，后上移到公共
//     include/dbridge 下；保留此转发头使老的 #include 路径继续可用，不必改动各调用方。
//
// 【真正的类型定义与文档】见 include/dbridge/RowPayload.h：
//   · RoutePayload —— 「写一张目标表」所需材料（table/dbColumns/binds/conflictKey…）。
//   · RowContext   —— 「一整行 Excel」的内部表示（含多个 RoutePayload + 失败标记）。
//
// 【命名空间】被转发的类型位于 dbridge::detail（见公共头）。
// ============================================================================
#include "dbridge/RowPayload.h"
