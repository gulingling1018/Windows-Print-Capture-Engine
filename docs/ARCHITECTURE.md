# WPCE Architecture Baseline

Last updated: 2026-10-24

## 1. Architecture Principle

WPCE has two architecture tracks:

- **User-mode capture service:** lightweight observer for existing printers. This is the MVP-A path.
- **Controlled XPS capture pipeline:** XPSDrv filter path for PrintTicket and FixedPage. This is the MVP-B path and requires a feasibility gate.

Do not mix these two tracks in one module. Passive watching cannot promise page-level XPS artifacts.

## 2. Module Boundaries

### `src/core`

Shared lifecycle, configuration, service orchestration, error handling, and bounded async queues.

Rules:

- Keep Windows handles wrapped in RAII types.
- Make shutdown idempotent.
- Avoid blocking the main service control path.

### `src/watcher`

Print job event monitoring through Win32 Spooler APIs.

Rules:

- Run notification waits on worker threads.
- Keep printer handles valid until notification handles are closed.
- Deduplicate noisy events by printer name, JobId, status, and timestamp window.

### `src/collector`

Job and print parameter extraction.

Rules:

- Treat `JOB_INFO_2` as the source for job-level data.
- Treat `DEVMODE` as driver-dependent.
- Store `unknown`, `unsupported`, or `not_reported` rather than inventing values.

### `src/storage`

JSON, XML, and page-artifact persistence.

Rules:

- Use schema versions.
- Write through async queues.
- Use atomic write patterns for final artifacts when practical.
- Keep large FixedPage artifacts out of the hot event path.
- Latest job and metadata snapshots are per printer:
  `latest/<printer>/job.json` and `latest/<printer>/metadata.json`.
  The global `latest/manifest.json` points at the newest printer bucket.
- Job archives are per printer under `jobs/<printer>/`.
- Billing has one authoritative bucket per business date:
  `data/YYYY-MM-DD/orders.json`. Schema v4 stores the complete task list,
  inline job records, task numbers and immutable settlement orders. Runtime
  code does not double-write `_state/today_active.json`, per-batch job files,
  or one-file-per-order history. Those paths are legacy migration inputs only.
- Billing bucket replacement is atomic. Repository writes deduplicate orders
  by both `order_id` and `task_id`/`batch_id`.

### `src/filter`

Controlled XPSDrv filter implementation.

Rules:

- Implement COM lifetime correctly.
- Implement `IPrintPipelineFilter` for filter entry.
- Do not display UI from the filter.
- Do not depend on .NET or Java.
- Keep filter functionality isolated and order-tolerant.

### `src/common`

Logging, path utilities, time conversion, string conversion, JSON/XML helpers, and shared result types.

Rules:

- Keep this module platform-conscious but not feature-specific.
- Avoid business logic in common helpers.

## 3. Data Contracts

Current business rules:

- Business dates, daily directories, and counters use the PC system local
  date. If conversion fails or is invalid, `BusinessClock` falls back to
  Beijing time (UTC+8). UTC timestamps remain only for audit/event fields
  ending in `_utc`.
- Cross-day startup clears active, suspended, and unsettled current-day state.
  Settled orders remain in their own immutable daily buckets.
- `JobSnapshot.job_id` is the application-internal stable ID;
  `external_job_id` preserves the raw spooler JobId and `idempotency_key`
  identifies one physical print job across duplicate listener notifications.
- Config load failures must not overwrite the user's existing config. Keep
  defaults in memory and preserve the unreadable file with an invalid backup.
- Retention cleanup applies only to dated capture/temp buckets. It must not
  delete config, current state, or settlement history.

### Job Snapshot

```json
{
  "schema_version": 1,
  "job_id": 12,
  "external_job_id": 42,
  "idempotency_key": "printer\\u001f42\\u001fphysical-identity",
  "user": "DOMAIN\\user",
  "document_name": "example.pdf",
  "printer_name": "Microsoft Print to PDF",
  "submit_time_utc": "2026-06-19T11:30:00Z",
  "status": "spooling",
  "source": "winspool"
}
```

### Metadata Snapshot

```json
{
  "schema_version": 1,
  "job_id": 12,
  "copies": { "value": 1, "state": "reported" },
  "duplex": { "value": "simplex", "state": "reported" },
  "color": { "value": "color", "state": "reported" },
  "paper_size": { "value": "A4", "state": "reported" },
  "orientation": { "value": "portrait", "state": "reported" },
  "file_extension": { "value": "dwt", "state": "reported" },
  "file_extension_source": "shd_path",
  "file_extension_confidence": 3
}
```

Allowed state values:

- `reported`
- `unsupported`
- `not_reported`
- `unknown`
- `error`

### Page Artifact Index

```json
{
  "schema_version": 1,
  "job_id": 12,
  "document_sequence": 1,
  "page_sequence": 1,
  "job_print_ticket": "job.pt.xml",
  "document_print_ticket": "document-001.pt.xml",
  "page_print_ticket": "page-001.pt.xml",
  "fixed_page": "page-001.fpage",
  "source": "xps-filter"
}
```

### Pricing Matrix

Current matrix dimensions are printer, paper, duplex, color, file type, and
price per sheet. Paper, printer, duplex, and file type may be wildcarded where
the existing UI/config permits it. Color is not a wildcard dimension:

- `color_mode` must resolve to `monochrome` or `color`.
- Empty or invalid legacy `color_mode` values are migrated to `monochrome`.
- Empty driver color values, `"-"`, and jobs on non-color-capable printers are
  treated as monochrome.
- Explicit color jobs on color-capable printers with `color_pages <= 0` are
  always `price_pending` with `pending_reason = "awaiting_color_pages"`.

## 4. Performance Budget

| Area | Budget |
|---|---:|
| Hot event processing | <= 50 ms per observed event batch |
| MVP-A resident memory | <= 50 MB |
| Idle CPU | < 1% |
| Printing CPU | < 5% |
| Disk writes | Async, bounded, backpressure-aware |

Measure budget violations with logs and counters instead of ad hoc observation.

## 5. Build and Tooling Assumptions

Required:

- C++20.
- MSVC toolchain for Windows builds.
- CMake.
- GoogleTest.
- Windows SDK.
- WDK before XPSDrv work.
- cppcheck and clang-tidy before release gates.

Allowed:

- Small C# management tooling only.

Prohibited:

- Electron.
- Java.
- UI inside print filters.

## 6. Development Gates

| Gate | Required Before |
|---|---|
| Git and CMake baseline | Any feature implementation |
| Phase 1 integration test | Metadata collector expansion |
| Metadata accuracy sample set | Claiming 95% parameter accuracy |
| WDK/signing/toolchain check | XPS filter implementation |
| Minimal filter print-through | PrintTicket and FixedPage capture |
| Stress harness | Release candidate |

## 7. 2026-10-24 订单安全修复基线

- 捕获保留期只清理日期桶内 jobs、latest、pages 三类已知目录，跳过符号链接和重解析点；不递归删除日期桶，不删除 orders.json、备份、迁移标记、配置或未知文件。
- 日桶加载采用完整校验后的事务提交，区分缺失、正常、备份恢复、损坏、不可读与不支持的 schema。先识别 schema 再解释任务，字段顺序不得造成未来版本被错误降级。
- 损坏文件无可读备份时禁止覆盖；有备份时先把损坏字节保留为唯一 .invalid 文件，再恢复上一可读版本。orders.json.bak 保存上一次成功保存前的可读版本，因此恢复后须核对恢复点之后的订单。
- 原子写入统一检查写入、flush、close、FlushFileBuffers 和替换结果；临时文件使用原生路径和进程内唯一序号。跨卷替换失败，不采用删除目标后复制的降级行为。
- BusinessDayCoordinator 显式绑定当前日桶日期；旧桶成功保存、新桶完整读取及计数器准备成功后才能换日。换日失败保留旧状态并告警；同一进程内的暂存事件带原始业务日期重放。
- 单号分配使用已绑定日期，并以已加载任务/订单号抬高计数器下界；计数器损坏时停止分配。恢复旧计数器和日期回拨不得重用当前日桶内已有编号。
- 已结算作业的计价、计费页数、批次财务汇总和订单保持冻结；迟到元数据、生命周期及重复作业别名只允许更新采集进度，人工改价、彩页修改与删除入口不得绕过冻结。
- 旧账务迁移为纯 C++ 库函数；按 SETTLE 单号中的原业务日期归档，不按 UTC 的日历日直接分桶。来源冲突或日期不明时保留原件并报告，迁移标记为可重试记录，不能替代目标校验。
- 快照在进入写入队列时确定捕获日期；监听结束的生命周期汇总按作业首次采集日分桶。过滤器页面产物与消息消费的其他已知问题仍按 [03] 的第二批计划处理。

详细实施、验证结果与限制见 docs/[04]订单安全与跨日修复实施记录-2026-10-24.md。

## 8. 2026-10-24 退出与过滤器安全基线

- 标准 PrintJobId 按 VT_UI4 读取，桥接 JSON 使用完整 UInt32 身份；缺失/零身份禁发可消费捕获，不影响原文档透传。
- 可选捕获异常与打印转发隔离，所有 part 使用 COM RAII；读取/发送/关闭传播首个错误。取消只设置状态，操作持有接口直到收尾，FilterFinished 只调用一次。
- 普通关闭先停止并加入所有监听线程，定向处理其已排队 MetaCall，再调用 BillingService::prepareShutdown 同步重放原业务日事件并保存。超时保留线程，保存失败保留窗口，禁止强杀线程掩盖退出失败。
- 退出业务位于 billing_service_shutdown.cpp，线程加入复用 worker_shutdown.h；恢复导出是带原日期和物理身份的独立 JSON，需人工核对，不能替代权威订单或自动修复原始采集丢失。

详细记录与限制见 docs/[06]退出保护与打印透传修复实施记录-2026-10-24.md。

## 9. 2026-10-24 消息与存储可靠性基线

- 新捕获目录使用 `p-<SHA-256(打印机名称的 UTF-8 字节)>`，原名称仍保存在 JSON 中；历史目录不覆盖式迁移，旧命名碰撞产生的历史损失不能自动还原。
- 过滤器为每次管道创建唯一 capture_id，输出 `<capture-id>.pt-message.json`，包含唯一 message_id、原始 JobId、打印机与物理身份（打印机、用户、文档、UTC 提交时间）。GUI 与过滤器须配套更新。
- 单一后台 Inbox 负责目录扫描、原子认领 `.claim`、持久确认和重试。每个目录独立限制扫描预算，并用持久游标轮转；多消费者通过文件租约互斥。Qt 主线程只做身份匹配、元数据合并及账务保存确认，后台不等待主线程。
- 只有物理身份严格匹配且 BillingService 确认保存，后台快照也已持久接受后，才写 `_pt-acks` 并删除原消息。确认写入失败保留认领文件；跨保存/确认崩溃采用至少一次交付与业务幂等，不承诺跨文件事务恰好一次。
- 缺身份旧消息、畸形和冲突消息保留在 `_pt-quarantine`；未匹配作业的认领文件保留重试，启动不清扫。已存作业在监听启动时恢复身份映射，避免进程重启后仅凭原始 JobId 误归属。
- JobSnapshotWriter 与 PageArtifactWriter 复用 ReliableFileWriter；`enqueue=true` 表示 `_writer-spool/<queue>/` 已完成原子暂存。正式目标逐项幂等写入，索引最后发布；一次只加载一个事务，故障退避重试，停止失败保留暂存，后续启动重放。停止边界只包含启动时已有及本实例接受的序号，不等待其他生产者未来数据。
- 页面 schema v2 包含 capture_id、fixed_page_state。文件名只生成一次，实体与索引使用同名；仅采集可恢复位置的真实 FixedPage 流，不能读取则明确 unsupported/error 并留空引用。PT/页面流上限 8 MiB；诊断样本有界，完整索引按页暂存。
- 页面暂存安排在当前页面转发之后，正式写盘放后台；可靠接受仍包含同步日志 I/O。性能预算需真机测量，不能用模拟压力结果证明整机 50ms/50MB 达标。页面标记也不等同包含字体和图片资源的完整可重印 XPS。
- 暂存目录不可手工当缓存清空；待写磁盘量、错误及停止结果是恢复依据。界面接入停止屏障，页面恢复器最多覆盖本次已发现的 8 个目录；未完成或无法接受均明确报告。

实施范围、验证证据和运行边界见 docs/[07]消息与存储可靠性优化实施记录-2026-10-24.md。

## 10. 2026-10-24 部署与工程门禁基线

- 安装领域服务位于 scripts/lib/Wpce.Install.psm1，系统操作表位于 Wpce.Install.Native.psm1；CLI 与 Qt 管理服务调用同一后端。GUI 启动仅做异步只读探测，“XPS 设置”明确区分检查、安装不重启、安装并重启。安装过程中保留对话框等待事务收尾，不杀死修改中的进程。
- DLL 要求 x64 PE DLL，复制摘要与实际 COM 路径逐步验证；注册写到 HKLM/SOFTWARE/Classes/CLSID，检查每次注册表写入，Win32 LONG 错误显式转换失败 HRESULT，不能用 SUCCEEDED 判断正错误码。
- 目标从已安装打印机的 v4 驱动关联文件定位；不全盘猜测 INF 目录。XML 禁止 DTD/外部实体，按命名空间解析，保留原下游接口；接受标准文档或字节流输入，拒绝未知自定义接口。新增过滤器用 IXpsDocumentProvider/Consumer GUID，不写虚构 InputPart 配置。仅代表列出的管道配置，不能证明所有驱动均支持捕获。
- 所有权威修改前持久保存事务原件并逐字节校验；同卷原子替换 DLL、配置及记录，核对摘要。相同 DLL 不重复替换。失败恢复原件、注册值、机器环境和值类型并核对；失败/中断事务保留并阻断后续安装，旧 sentinel 不代替探测。
- 服务控制用托管 ServiceController 查询、超时等待及最终状态核对。显式重启时在复制前停止服务，完成配置后启动；失败尝试恢复运行。未显式重启不调用停止/启动，记录 restart_required；配置核对完成不等于已通过真实滤镜加载和打印验证。
- Qt 配套 Debug/Release build/test presets，逐阶段非零即停；共享 Wpce.Process 处理进程 LANG 和原生命令失败，打包脚本复用。GUI 构建的独立资源目标在增量构建时也同步过滤器与脚本，打包保留同一版本后端。
- Strict 静态模式核对真实 MSVC/Qt/C++20 编译数据库、配置及 File API 目标源码集合，保留同源不同目标编译变体；必要工具/数据库/目标模型缺失或分析失败均非零。Local 明确范围和跳过项，不设置 release_gate。当前环境 clang-tidy 缺失，因此真实发布门禁尚未通过。

本节优先于历史自动安装与旧注册表路径说明；部署命令及完成边界见 TOOLCHAIN 与 docs/[08]部署与工程门禁修复实施记录-2026-10-24.md。
