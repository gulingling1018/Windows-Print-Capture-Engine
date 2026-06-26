# 优化记录

## 2026-06-24: 计费矩阵优化 — 颜色维度替换为打印机维度

### 问题原因
原计费矩阵维度为 `{纸张, 单|双面, 颜色, 文件类型 | 单价}`，存在以下问题：

1. **"颜色"维度语义模糊**：黑白打印机只能打印黑白任务，"颜色"参数对黑白打印机无意义；彩色打印机虽然可以打印黑白和彩色任务，但程序通过 DEVMODE/PrintTicket 获取的颜色信息并不可靠（部分驱动不报告或报告不准确），导致实际无法准确区分黑白/彩色任务。
2. **定价与打印机脱节**：实际打印店的计价逻辑是按打印机来定费率——每个打印机有自己的价格表，而非按抽象的"颜色"维度。

### 业务设计目标
将计费矩阵改为 `{纸张, 单|双面, 打印机, 文件类型 | 单价(元/张)}`，以打印机作为核心计费维度：

- **黑白打印机**：所有任务统一按规则自动计价，不再考虑颜色维度（因为物理上只能输出黑白）
- **彩色打印机**：保留 `color_price_pending` 机制。当规则未指定具体打印机（通配规则匹配）且检测到彩色任务时，标记为待操作员手动定价；当操作员为该打印机设置了特定规则时，视为已确认价格，自动应用
- **纸张参数优化**：纸张备选项从打印机配置扫描获取；选中打印机时显示该打印机支持的纸张，未选中时显示所有打印机纸张并集

### 实现方案

**数据结构层：**
- `PricingRule`: `color_mode` 字段替换为 `printer_name` + `printer_name_set`（支持管道多值）
- `JobPricingEntry`: 新增 `matched_printer_name` 字段供 UI 展示
- `PrinterProfile`: 新增 `supported_paper_sizes` 字段，通过 `DeviceCapabilitiesW(DC_PAPERNAMES)` 扫描

**定价引擎：**
- `find_best_rule()`: 移除 `effective_color` 参数，改为匹配 `metadata.printer_name`
- `compute_job_pricing()`: 颜色待填价逻辑改为检查匹配规则是否为通配打印机
- `rule_specificity()`: 打印机维度参与特异性计算

**配置层：**
- 6 条工厂默认规则的打印机维度设为通配（空字符串）
- JSON 序列化/反序列化适配新字段，旧 `color_mode` 安静跳过
- 打印机配置 JSON 新增 `paper_sizes` 数组持久化

**界面层：**
- 设置对话框定价矩阵列标题 "颜色" → "打印机"
- 颜色下拉框替换为打印机下拉框，从 `PrinterProfileConfig` 动态填充
- 新增纸张-打印机联动：切换打印机时纸张下拉框自动更新
- 快速预设按钮更新（去掉"黑白"后缀）

**共享基础设施：**
- 纸张尺寸映射表从 `metadata_collector` 提取到 `src/common/paper_size_map.h/.cpp`，供多模块复用

### 影响文件
| 文件 | 变更类型 |
|------|---------|
| `src/core/data_contracts.h` | 结构变更 |
| `src/common/pricing_engine.h/.cpp` | 接口+逻辑变更 |
| `src/common/printer_profile.h/.cpp` | 结构+逻辑增强 |
| `src/common/paper_size_map.h/.cpp` | 新建共享模块 |
| `src/common/app_config.cpp` | 配置序列化适配 |
| `src/collector/metadata_collector.cpp` | 引用调整 |
| `src/storage/json_serializer.cpp` | 字段名更新 |
| `src/ui/settings_dialog.cpp` | UI+逻辑适配 |
| `tests/pricing_engine_tests.cpp` | 测试更新+新增 |

---

## 2026-06-24: 结算窗口优化 — 分色计费 + 列重排 + 旧代码清理

### 问题原因
1. **结算窗口列不合理**：旧的 `{#, 文档, 份, 单双, 页, 纸张, 打印机, ¥}` 列中"纸张"和"#"占用空间但价值低，缺少"总页数"汇总
2. **彩色任务无法区分计价**：操作员无法标记彩印页数，只能通过 `color_price_pending` 触发完全人工定价
3. **旧 UI 逻辑无效**：响应式列隐藏、无活动自动跳转、优先级导航在实际使用中无效果

### 业务设计目标
- 列重排为 `{文档, 打印机, 份数, 单|双面, 页数, 总页数, 价格}`，单|双面显示单面/长边/短边
- 分色显示：黑白任务 → 纯数字；彩色任务 → "m+n"格式（m=彩印默认0，操作员填入）
- 分色计费：m>0 按比例拆分物理张数，彩印张按彩色费率、黑白张按黑白费率；m=0 价格为0待填
- PricingRule 新增 `color_price_per_sheet` 字段，与 `price_per_sheet` 区分黑白/彩色单价
- 删除 `updateColumnVisibility`、`mostRelevantRow`、`inactivityTimer`、尺寸限制等无效代码

### 实现方案摘要
- 数据模型：PricingRule + `color_price_per_sheet`；JobPricingEntry + `color_pages/mono_pages/color_subtotal/mono_subtotal`
- 定价引擎：`compute_job_pricing()` 新增 `color_pages` 参数，按比例拆分物理张数分别计价
- QuickSettleDialog：列重构为7列，彩色任务页数显示"m+n"，价格列显示"待填彩印"/金额
- 清理：删除 BillingBatchModel 中 4 个优先级导航函数，QuickSettleDialog 中 3 个无效功能

### 影响文件
| 文件 | 变更类型 |
|------|---------|
| `src/core/data_contracts.h` | PricingRule +color_price_per_sheet; JobPricingEntry +4分色字段 |
| `src/common/pricing_engine.h/.cpp` | compute_job_pricing +color_pages参数，分色计算分支 |
| `src/ui/quick_settle_dialog.h/.cpp` | 列重构7列，分色显示"m+n"，清理无效代码 |
| `src/ui/billing_batch_model.h/.cpp` | 删除 mostRelevantRow/currentActiveRow 等4个函数 |
| `src/ui/main_window.cpp` | 适配 currentActiveRow 删除 |
| `src/common/app_config.cpp` | color_price_per_sheet 序列化 |
| `src/storage/json_serializer.cpp` | 分色字段序列化 |
| `src/bridge/settlement_bridge.cpp` | schema_version 修复 (1→2) |
| `tests/pricing_engine_tests.cpp` | 更新旧测试 + 新增4个分色计费测试 |

---

## 2026-06-24: Watcher 稳定性加固 — 大文件并发打印崩溃修复

### 问题原因
打印 20MB+ PPT 并发多任务时，Watcher 线程崩溃导致监控完全中断：

1. **主循环单层 try/catch**：任何瞬态异常（EnumJobsW 缓冲区竞态、FindNextPrinterChangeNotification 失败）都会导致 watcher 线程永久退出
2. **128MB 缓冲区限制抛异常**：大并发 PPT 的 JOB_INFO_2W 含大 pDevMode，超限后 throw 杀死线程
3. **EnumJobsW 仅重试 2 次**：高并发场景下缓冲区需求快速变化，2次不够
4. **DISCARDED 通知刷新路径抛异常**：高并发时 spooler 频繁设置 DISCARDED，刷新失败即致命
5. **job_states_ 无清理**：终态任务累积导致内存持续增长

### 实现方案

- EnumJobsW 缓冲区超 128MB 时 soft-return 空列表（不再 throw），周期 rescan 下次重试
- EnumJobsW 重试次数从 2 提升到 4，所有重试均允许在 ERROR_INSUFFICIENT_BUFFER 时扩容
- 主循环内 WaitForSingleObject 失败改为 log + sleep + continue（不再 throw）
- FindNextPrinterChangeNotification 失败改为 log + continue
- DISCARDED 刷新失败改为 log 告警而非 throw
- 通知处理段包裹在 per-iteration try/catch，瞬态异常不再杀死线程（500ms 退避后继续）
- 外层 try/catch 仅捕获真正致命错误（打印机打开失败等）
- 新增 job_states_ 定期清理：当条目超 256 个时，删除 10 分钟前的终态条目

### 影响文件
| 文件 | 变更类型 |
|------|---------|
| `src/watcher/print_watcher.cpp` | enumerate_printer_jobs soft-failure + retry 4x; run() 双层异常处理 + job_states 清理 |

---

## 2026-06-24: 快速结算小窗折叠态

### 问题原因
快速结算窗口在打印店日常操作时始终占据较大屏幕空间，遮挡主窗口或其他工作窗口。

### 实现方案
- 标题栏新增折叠/展开按钮（▽/△），快捷键 C
- 折叠状态下隐藏首尾文档锚点和任务表格，仅保留标题栏 + 汇总条 + 操作栏
- 展开时恢复记住的窗口尺寸
- 使用 `adjustSize()` 自适应折叠后的最小高度

### 影响文件
| 文件 | 变更类型 |
|------|---------|
| `src/ui/quick_settle_dialog.h` | +collapsed_ +collapseBtn_ +frame pointers +toggleCollapsed() |
| `src/ui/quick_settle_dialog.cpp` | 折叠按钮UI + toggleCollapsed实现 + Key_C 快捷键 |

---

## 2026-06-24: 打印监控时序优化 — 可配置轮询参数

### 问题原因
Watcher 的 `wait_timeout`(250ms) 和 `rescan_interval`(5s) 为硬编码常量，无法针对不同打印环境调优（高速激光打印机 vs 网络打印机延迟差异大）。

### 实现方案
- `MonitoringConfig` 新增 `rescan_interval_seconds`(默认5) 和 `poll_timeout_ms`(默认250) 字段
- `WatcherBridge` 新增 `setRescanInterval()` / `setPollTimeout()` setter，传递到 `WatcherConfig`
- `MainWindow::onStartClicked()` 从 `config_.monitoring` 读取并设置
- JSON 配置持久化（序列化/反序列化）

### 影响文件
| 文件 | 变更类型 |
|------|---------|
| `src/common/app_config.h` | MonitoringConfig +2字段 |
| `src/common/app_config.cpp` | defaults + serialize + deserialize |
| `src/ui/watcher_bridge.h` | +2 setter + 2 member |
| `src/ui/watcher_bridge.cpp` | setter实现 + WatcherConfig构造使用新字段 |
| `src/ui/main_window.cpp` | startWatching前设置新参数 |

---

## 2026-06-24: 主窗口批次列表增强 — 首尾文档 + 总价标识

### 问题原因
主窗口树形列表的批次行仅显示 "用户#NN" 名称，操作员难以快速识别该批次属于哪个客户（需展开查看具体任务）。

### 实现方案
- `ColName` 列：批次行追加首尾文档名缩略（如 `用户#01  [论文.pdf … 报告.docx]`）
- `ColPrice` 列：批次行加 ¥ 前缀 + 待定价提示（如 `¥12.50 (2待)`）
- 文档名超 20 字符截断显示

### 影响文件
| 文件 | 变更类型 |
|------|---------|
| `src/ui/billing_batch_model.cpp` | ColName 首尾文档拼接 + ColPrice ¥前缀 |
