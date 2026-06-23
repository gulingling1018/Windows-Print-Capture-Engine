# 打印捕获引擎 — 前端对接工作交接文档

> **文档目的**：向后端改动完成后，前端工程师能一次性获得全部接口信息和业务需求，无需翻阅代码。
>
> **阅读对象**：前端工程师（Qt6 / C++ / `src/ui/` 目录）
>
> **重要提示**：本文档只描述 **有什么** 和 **要什么**，不设计 **怎么做**。具体 UI 实现方案由前端工程师自行决定。

---

## 一、后端已完成的改动概览

### 1.1 新建模块

| 模块 | 文件 | 角色 |
|------|------|------|
| 定价引擎 | `src/common/pricing_engine.h` / `.cpp` | 矩阵规则匹配 + 双面尾页修正 + 价格计算 |
| 结算桥接 | `src/bridge/settlement_bridge.h` / `.cpp` | 结算订单创建 / 定价快照冻结 / JSON 持久化 |

两个模块均为 `wpce_core` 静态库成员，**不依赖 Qt**，前端可直接 `#include` 调用。

### 1.2 已有模块改动

| 模块 | 改动 |
|------|------|
| `core/data_contracts.h` | `MetadataSnapshot` 新增 `file_extension` 字段；新增 4 个数据结构 |
| `common/app_config.h` | `PricingConfig` 新增 `rules`（规则矩阵）和 `color_price_pending` 两个字段 |
| `collector/metadata_collector.cpp` | 采集阶段自动从文档名提取文件扩展名，写入 `MetadataSnapshot.file_extension` |
| `storage/json_serializer` | 新增结构的 JSON 序列化 |

### 1.3 未修改的模块（保持原样）

`src/ui/` 下全部文件均未改动，`src/ui/pricing_config.h` 中原有的 `calculate_job_cost()` 等函数保持不变，**但建议逐步迁移到新引擎**。

---

## 二、新增数据结构速查

### 2.1 PricingRule — 定价规则

定义位置：`src/core/data_contracts.h` → `core::PricingRule`

```cpp
struct PricingRule {
  std::string rule_id;         // 唯一标识，如 "rule-001"
  std::string paper_size;      // 纸张尺寸，空字符串 = 匹配全部
  std::string duplex_mode;     // "simplex" | "duplex" | "" = 全部
  std::string color_mode;      // "color" | "monochrome" | "" = 全部
  std::string file_extension;  // 文件扩展名，空 = 匹配全部
  double price_per_sheet;      // 每张物理纸的单价（单面=1页/张，双面=2页/张）
  int priority;                // 特异性评分，越大越优先（加载时自动计算）
};
```

**规则匹配逻辑**（供理解用，前端不需要直接调用）：

```
{ A4, simplex, monochrome, ""   , 0.15 } → 匹配所有 A4 单面黑白，无论什么文件格式
{ A4, simplex, ""       , "dwg", 0.50 } → 匹配所有 A4 单面 DWG，无论颜色
{ A2, simplex, ""       , ""   , 2.00 } → 匹配所有 A2 单面
{ "" , ""     , ""       , ""   , 0.10 } → 兜底规则，匹配全部（特异性=0）
```

多个规则同时匹配时，**特异性最高**（非空维度最多）的规则胜出。相同特异性时，列表中靠前的规则胜出。

### 2.2 JobPricingEntry — 单作业定价结果

定义位置：`src/core/data_contracts.h` → `core::JobPricingEntry`

```cpp
struct JobPricingEntry {
  uint32_t job_id;
  int total_pages;              // 逻辑页数
  int copies;                   // 份数
  int effective_sheets;         // 实际消耗纸张数（含双面尾页修正）
  int duplex_full_sheets;       // 完整双面张数
  int simplex_tail_pages;       // 双面奇数页的尾页单面页数
  std::string matched_rule_id;  // 命中的规则ID，空 = 未匹配或走旧模型
  double base_price;            // 基价（未扣折扣）
  double total_price;           // 总价
  bool price_pending;           // ★ true = 价格需要操作员手动填写
  std::string pending_reason;   // "color" | "unmatched_rule" | "unmatched_rule_color" | ""
};
```

### 2.3 SettlementOrder — 结算订单（冻结快照）

定义位置：`src/core/data_contracts.h` → `core::SettlementOrder`

```cpp
struct SettlementOrder {
  int32_t schema_version;
  std::string order_id;             // "SETTLE-20260622-001"
  std::string task_id;              // 关联的 UserTask.taskId
  std::string user_id;
  std::string settled_by;           // 操作员标识（由前端填入）
  std::string settled_at_utc;       // 结算时间戳（后端自动填 UTC）
  std::string period_start_utc;     // 结算时段起点
  std::string period_end_utc;       // 结算时段终点
  std::vector<uint32_t> job_ids;
  std::vector<JobPricingEntry> pricing_entries;
  std::string pricing_snapshot_json; // ★ 结算时刻的定价配置完整快照
  double total_price;
  std::string currency;
};
```

### 2.4 SheetBreakdown — 纸张消耗明细

定义位置：`src/core/data_contracts.h` → `core::SheetBreakdown`

```cpp
struct SheetBreakdown {
  int full_duplex_sheets;   // 完整双面张
  int simplex_tail_pages;   // 尾页单面页数
  int total_sheets;         // 总纸张数
};
```

### 2.5 MetadataSnapshot 新增字段

| 字段 | 类型 | 说明 |
|------|------|------|
| `file_extension` | `FieldValue` | 从文档名提取的扩展名（小写，不含点），如 `"pdf"`, `"dwg"`。未提取到时 state = NotReported |

---

## 三、前端可调用的后端 API

### 3.1 定价引擎（替代旧的 `calculate_job_cost`）

头文件：`#include "common/pricing_engine.h"`  
命名空间：`wpce::common`

**★ 核心 API — 计算单作业价格：**

```cpp
core::JobPricingEntry compute_job_pricing(
    const core::MetadataSnapshot& metadata,
    const PricingConfig& config);
```

- 当 `config.rules` 非空时，使用矩阵规则匹配
- 当 `config.rules` 为空时，回退到旧的加法模型（向后兼容）
- 返回值 `JobPricingEntry.price_pending` 指示是否需要操作员手动定价
- **替代对象**：`src/ui/pricing_config.h` 中的 `calculate_job_cost()`

**辅助 API：**

| 函数 | 用途 |
|------|------|
| `extract_file_extension(doc_name)` | 从文档名提取扩展名 |
| `is_duplex_job(metadata)` | 判双面 |
| `is_color_job(metadata)` | 判彩色 |
| `compute_sheet_breakdown(pages, is_duplex)` | 纸张消耗分解（含双面尾页修正） |
| `sort_rules_by_priority(rules)` | 按特异性排序规则列表 |

### 3.2 结算桥接

头文件：`#include "bridge/settlement_bridge.h"`  
命名空间：`wpce::bridge`  
类：`SettlementBridge`（纯静态方法，无状态）

| 方法 | 用途 |
|------|------|
| `generateOrderId(date_yyyymmdd, counter)` | 生成结算编号，如 `"SETTLE-20260622-001"` |
| `snapshotPricingConfig(config)` | 将 PricingConfig 序列化为 JSON 字符串（用于结算快照） |
| `createSettlementOrder(...)` | 创建完整的 SettlementOrder，自动填入 `settled_at_utc` 和 `total_price` |
| `toJson(order)` / `fromJson(json)` | SettlementOrder 的 JSON 序列化/反序列化 |
| `saveToFile(order, output_dir)` | 写入 `<output_dir>/settlement_orders/<order_id>.json` |
| `loadFromFile(path)` | 从文件读取 |

### 3.3 配置数据

头文件：`#include "common/app_config.h"`  
命名空间：`wpce::common`

`PricingConfig` 新增：

| 字段 | 类型 | 说明 |
|------|------|------|
| `rules` | `std::vector<core::PricingRule>` | 规则矩阵，空 = 回退旧模型 |
| `color_price_pending` | `bool` | true = 彩色作业不自动定价 |

`AppConfig` 的 `save_to_json()` / `load_from_json()` 已支持读写这两个新字段。

**配置 JSON 新增片段示例**（`wpce_config.json` 中 `pricing` 节）：

```json
{
  "pricing": {
    "single_sided_per_page": 0.15,
    "double_sided_per_sheet": 0.25,
    "color_price_pending": true,
    "rules": [
      {"rule_id":"1","paper_size":"A4","duplex_mode":"simplex",
       "color_mode":"monochrome","file_extension":"","price_per_sheet":0.15},
      {"rule_id":"2","paper_size":"A4","duplex_mode":"simplex",
       "color_mode":"","file_extension":"dwg","price_per_sheet":0.50},
      {"rule_id":"3","paper_size":"A2","duplex_mode":"simplex",
       "color_mode":"","file_extension":"","price_per_sheet":2.00}
    ]
  }
}
```

---

## 四、业务需求转述（来自产品方）

以下需求由产品方提出，后端已完成数据结构层面的支撑，前端需根据自身设计判断具体实现方式。

### 需求 1：结算编号与用户绑定

**背景**：当前只有单一 `taskId`（`YYYYMMDD-NNN`），缺少显式的结算订单号概念。

**期望**：
- 打印任务编号（`job_id`）→ 用户结算编号（`SettlementOrder.order_id`）→ 用户、价格信息的完整三层关联
- 结算编号格式：`SETTLE-YYYYMMDD-NNN`，每日自增不重复
- 结算时记录时间段（`period_start_utc` / `period_end_utc`）和操作员（`settled_by`）
- 结算时刻的定价配置应被冻结保存（后端已提供 `pricing_snapshot_json`）

**后端支撑**：`SettlementOrder` 结构 + `SettlementBridge` 类已就绪。

### 需求 2：价格矩阵配置

**背景**：当前只有单一的加法模型（基础价 + 几个附加费），无法为不同纸张/颜色/文件类型设置独立价格。

**期望**：
- 支持 `{纸张, 单|双面, 颜色, 文件类型, 单价}` 五维定价矩阵
- 每个维度留空 = 通配（匹配该维度的所有情况）
- 特异性更高的规则覆盖更通用的规则
- 设置界面应提供规则表格编辑器：
  - 列：纸张尺寸 | 单/双面 | 颜色 | 文件类型 | 单价(元/张)
  - 支持添加/删除/编辑行
  - 空单元格以灰字显示"全部"
- 旧设置界面的 5 个 DoubleSpinBox 可保留作为"无规则时的回退价格"

**后端支撑**：`PricingRule` 结构 + `PricingConfig.rules` 向量 + JSON 序列化已就绪。

### 需求 3：双面奇数页尾页单面计费

**背景**：旧公式 `ceil(pages/2) × 双面价` 对 3 页双面算 2 张双面价 = 0.50 元，实际应为 1 张双面 + 1 页单面。

**期望**：3 页双面 = 1 张双面 (×0.25) + 1 页单面 (×0.15) = 0.40 元。

**后端支撑**：`compute_sheet_breakdown()` + `calculate_job_cost_legacy()` + `compute_job_pricing()` 均已修正。

**前端影响**：如果用新 API `compute_job_pricing()` → 自动获得修正结果。如仍用旧 `calculate_job_cost()` → 需调用 `calculate_job_cost_legacy()`。

### 需求 4：彩色打印价格留空，等待操作员填写

**背景**：彩印价格通常按实际耗材情况灵活定价，不适合自动计算。

**期望**：
- 配置项：彩色是否自动定价（开关）
- 当开关打开时，彩色作业自动标记为"待填写"
- 结算对话框中待填写项价格栏可编辑，操作员手动输入
- 不填写时该项价格视为 0，参与总价计算

**后端支撑**：
- `PricingConfig.color_price_pending` 控制此行为
- `JobPricingEntry.price_pending = true` + `pending_reason = "color"` 标识待填写
- 规则矩阵中，如果命中的规则 `color_mode` 明确指定了 `"color"`，则认为该规则已包含彩色定价，不再标记 pending

### 需求 5：未匹配定价规则的项目 — 醒目 UI 标识

**背景**：使用规则矩阵后，部分作业可能没有任何规则匹配（如 A1 纸张且无对应规则）。

**期望**：
- 此类项目在前端应呈现醒目视觉标识（如橙色边框/黄色背景/警告图标）
- 结算对话框中未定价项目统一汇总，提示操作员逐项填写
- 不填写时该项价格视为 0

**后端支撑**：`JobPricingEntry.price_pending = true` + `pending_reason = "unmatched_rule"` 或 `"unmatched_rule_color"`。

### 需求 6：结算卡片增强

**期望**：
- 小票预览卡片中显示结算时间段（如 `"14:30 ~ 15:02"`）
- 存在待定价项目时卡片边框变为警告色
- 已结算卡片显示结算时间戳

**后端支撑**：`SettlementOrder` 提供 `period_start_utc`、`period_end_utc`、`settled_at_utc`。

---

## 五、前端可复用的现有资源

以下文件/函数当前仍在 `src/ui/` 中正常工作，可在迁移过程中逐步替换：

| 现有资源 | 位置 | 后续建议 |
|----------|------|----------|
| `calculate_job_cost()` | `src/ui/pricing_config.h` | 逐步替换为 `common::compute_job_pricing()` |
| `is_duplex_job()` / `is_color_job()` 等 | `src/ui/pricing_config.h` | 逐步替换为 `common::pricing_engine.h` 同名函数 |
| `UserTaskModel` | `src/ui/user_task_model.h` | 在 `onMetadata()` 中接入新引擎，存储 `JobPricingEntry` |
| `CompletionDialog` | `src/ui/completion_dialog.h` | 对接 `SettlementBridge`，支持可编辑价格行 |
| `SettingsDialog` | `src/ui/settings_dialog.h` | "价格方案"标签页增加规则矩阵表格 |
| `ReceiptPreviewPanel` | `src/ui/receipt_preview_panel.h` | 展示时间段、待定价警告色 |
| `JobDetailPanel` | `src/ui/job_detail_panel.h` | 展示定价明细和待定价原因 |

---

## 六、文件系统桥接

结算订单通过文件系统持久化（与现有的 `user_tasks.json` 模式一致）：

```
<output_dir>/
  user_tasks.json                          ← 已有，UserTaskModel 读写
  settlement_orders/
    SETTLE-20260622-001.json               ← 新增，SettlementBridge 读写
    SETTLE-20260622-002.json
```

前端不需要关心 JSON 格式细节——使用 `SettlementBridge::saveToFile()` / `SettlementBridge::loadFromFile()` 即可。

---

## 七、向后兼容说明

为确保改动不破坏现有功能，后端做了以下兼容处理：

1. **`PricingConfig.rules` 为空数组** → `compute_job_pricing()` 自动回退到旧加法模型
2. **`color_price_pending` 默认 true** → 但如果前端不处理 `price_pending` 字段，旧代码仍自动计算彩色价格（见第 3 条）
3. **旧 `calculate_job_cost()` 函数未删除** → 前端迁移前可继续使用
4. **`file_extension` 默认 NotReported** → 旧采集流程不受影响
5. **所有新增字段都有合理默认值** → 旧 JSON 配置文件加载正常

---

## 八、建议的前端工作优先级

| 优先级 | 工作项 | 原因 |
|--------|--------|------|
| P0 | 将现有 `calculate_job_cost()` 调用替换为 `compute_job_pricing()` | 立即获得双面尾页修正（影响计费准确性） |
| P0 | 表格和结算对话框中对 `price_pending=true` 的作业做醒目标识 | 确保操作员不遗漏待定价项目 |
| P1 | 结算对话框支持编辑待定价项目的价格 | 彩色留空需求的核心交互 |
| P1 | 结算时调用 `SettlementBridge` 创建冻结订单 | 审计追溯能力 |
| P2 | 设置界面增加规则矩阵表格编辑器 | 规则矩阵的日常维护入口 |
| P2 | 小票预览面板增强（时间段、警告色） | 操作效率提升 |
| P3 | 详情面板展示完整定价明细（命中规则、纸张分解） | 调试和透明度 |
