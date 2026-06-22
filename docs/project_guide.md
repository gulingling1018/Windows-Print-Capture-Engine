# WPCE 项目开发指南（初学者版）

> 本文档面向 C++ 初学者，手把手教你如何从零理解、开发、调试、测试和部署本项目。

---

## 目录

1. [项目是做什么的](#1-项目是做什么的)
2. [项目架构总览](#2-项目架构总览)
3. [开发环境搭建](#3-开发环境搭建)
4. [第一次编译运行](#4-第一次编译运行)
5. [代码目录结构详解](#5-代码目录结构详解)
6. [如何调试 Debug](#6-如何调试-debug)
7. [如何添加新功能](#7-如何添加新功能)
8. [如何部署到其他电脑](#8-如何部署到其他电脑)
9. [如何测试项目](#9-如何测试项目)
10. [常见问题排查](#10-常见问题排查)

---

## 1. 项目是做什么的

WPCE（Windows Print Capture Engine）是一个**打印捕获系统**，运行在 Windows 10/11 上，能够：

- **实时监听**所有本地打印机的打印任务
- **采集打印参数**：页数、份数、单双面、颜色、纸张大小、方向等
- **追踪打印生命周期**：从任务提交 → 假脱机 → 打印中 → 完成/失败/取消
- **自动计算打印费用**：根据纸张、颜色、双面等参数计费
- **按用户分组管理**：一个用户可能连续打印多个文档，系统自动归组
- **提供图形界面**：实时查看打印状态、参数详情、费用统计

### 核心价值

传统方式无法知道用户打印时到底选了什么参数（颜色？双面？A3？）。本项目通过**两层采集**解决这个问题：

| 层级 | 数据来源 | 能采集什么 |
|------|----------|-----------|
| 第一层：Spooler 监听 | Win32 打印队列 API | 任务状态、用户、文档名、页数进度 |
| 第二层：XPS 过滤器 | 拦截打印管道的 PrintTicket XML | 真实的打印参数（颜色、双面、份数、选页等） |

---

## 2. 项目架构总览

```
用户按"打印"
    │
    ▼
┌─────────────────────────────────────────────────┐
│  Windows 打印管道（Spooler → 驱动 → 打印机）       │
│                                                   │
│  ┌──────────────────────────────────────────┐     │
│  │  wpce_xps_filter.dll（XPS 过滤器）        │     │
│  │  拦截 PrintTicket XML → 写入 JSON 文件     │     │
│  └──────────────────────────────────────────┘     │
│                      │                            │
│                      ▼                            │
│  ┌──────────────────────────────────────────┐     │
│  │  PrintWatcher（Spooler 监听器）            │     │
│  │  用 Win32 API 监听打印队列变化             │     │
│  └──────────────────────────────────────────┘     │
└─────────────────────────────────────────────────┘
          │                        │
          ▼                        ▼
┌─────────────────┐    ┌──────────────────────┐
│  MetadataMerger  │    │  WatcherBridge (Qt)  │
│  合并两层数据     │    │  连接后端和前端       │
└─────────────────┘    └──────────────────────┘
                                │
                                ▼
                     ┌─────────────────────┐
                     │  GUI 界面 (wpce_ui)  │
                     │  用户分组/详情/费用   │
                     └─────────────────────┘
```

### 技术栈

| 项目 | 技术 |
|------|------|
| 编程语言 | C++20 |
| 构建工具 | CMake 3.25+ |
| 编译器 | Visual Studio 2022/2026（MSVC） |
| GUI 框架 | Qt 6.8+ |
| 测试框架 | GoogleTest 1.16 |
| 静态分析 | clang-tidy, cppcheck |
| 代码风格 | clang-format（Microsoft 风格） |

---

## 3. 开发环境搭建

### 3.1 必须安装的软件

#### ① Visual Studio 2022（或更新版本）

1. 从 https://visualstudio.microsoft.com/ 下载 **Community** 版（免费）
2. 安装时勾选：
   - ✅ **使用 C++ 的桌面开发**（Desktop development with C++）
   - ✅ 右侧详细信息中确保勾选：
     - MSVC v143 生成工具
     - Windows 10/11 SDK（最新版）
     - C++ CMake 工具
3. 安装路径假设为 `D:\download\Microsoft Visual Studio18_Community`（可自定义）

#### ② CMake

1. 从 https://cmake.org/download/ 下载安装
2. 安装时选择 **Add CMake to the system PATH**
3. 验证：打开 PowerShell，运行 `cmake --version`

#### ③ Qt 6.8+

1. 从 https://www.qt.io/download 下载 Qt 在线安装器
2. 登录/注册 Qt 账号
3. 选择安装：
   - **Qt 6.8.2** → 勾选 **MSVC 2022 64-bit**
4. 安装路径假设为 `C:\Qt\6.8.2\msvc2022_64`

#### ④ Git

1. 从 https://git-scm.com/ 下载安装
2. 安装时选择默认选项即可

### 3.2 可选安装的工具

| 工具 | 用途 | 安装方式 |
|------|------|----------|
| clang-format | 代码格式化 | VS 自带或 `pip install clang-format` |
| clang-tidy | 静态分析 | VS 自带 |
| cppcheck | 静态分析 | https://cppcheck.github.io/ |
| DebugView | 查看调试日志 | Sysinternals 套件 |
| Ninja | 更快的构建 | `choco install ninja` |

### 3.3 验证环境

```powershell
# 在项目根目录运行环境检查脚本
.\scripts\check-toolchain.ps1
```

输出中应该看到 `cmake`、`git`、`msbuild` 等工具的路径，不应有 `<missing>`。

---

## 4. 第一次编译运行

### 4.1 获取代码

```powershell
# 克隆项目（如果还没有的话）
cd D:\code
git clone <仓库地址> "Windows Print Capture Engine"
cd "Windows Print Capture Engine"
```

### 4.2 编译（不带 GUI，仅核心库 + 测试）

这种方式不需要安装 Qt，最快验证代码能否编译通过：

```powershell
# 方式一：使用项目脚本
.\scripts\build-msvc.ps1

# 方式二：手动执行 CMake 命令
# 第 1 步：配置（生成 VS 工程文件）
cmake --preset vs2026-x64

# 第 2 步：编译（Debug 模式）
cmake --build --preset vs2026-debug

# 第 3 步：运行测试
ctest --preset vs2026-debug
```

如果看到类似 `[PASSED] 27 tests`，说明编译和测试全部通过！

### 4.3 编译（带 GUI）

需要先安装好 Qt 6.8+：

```powershell
# 配置（启用 Qt）
cmake --preset vs2026-x64-qt

# 编译 Debug 版本
cmake --build --preset vs2026-debug

# 运行 GUI（需要管理员权限才能安装 XPS 过滤器）
.\build\vs2026-x64-qt\src\ui\Debug\wpce_ui.exe
```

### 4.4 编译 Release 版本

```powershell
# Release 版本用于部署，体积更小、速度更快
cmake --build build/vs2026-x64-qt --config Release
```

### 4.5 理解 CMake Presets

项目使用 `CMakePresets.json` 预定义了编译方案：

| Preset 名 | 说明 |
|-----------|------|
| `vs2026-x64` | 纯核心库，不带 GUI，不需 Qt |
| `vs2026-x64-qt` | 核心库 + GUI，需要 Qt |
| `ninja-msvc` | 使用 Ninja 构建（更快） |

编译产物位置：

```
build/
├── vs2026-x64/           # 不带 GUI 的编译产物
│   ├── src/Debug/
│   │   ├── wpce.exe      # CLI 工具
│   │   └── wpce_core.lib # 核心库
│   └── tests/Debug/
│       ├── wpce_contract_tests.exe  # 合约测试
│       └── wpce_parser_tests.exe    # 解析器测试
│
└── vs2026-x64-qt/        # 带 GUI 的编译产物
    └── src/
        ├── Debug/
        │   ├── wpce_ui.exe           # GUI 程序
        │   └── wpce_xps_filter.dll   # XPS 过滤器 DLL
        └── ui/Debug/
            └── wpce_ui.exe           # GUI（带 Qt 依赖）
```

---

## 5. 代码目录结构详解

```
Windows Print Capture Engine/
├── CMakeLists.txt          # 顶层 CMake 配置（项目入口）
├── CMakePresets.json       # 编译预设方案
├── .clang-format           # 代码格式化规则
│
├── src/                    # ===== 源代码 =====
│   ├── CMakeLists.txt      # 核心库 + CLI 定义
│   │
│   ├── core/               # 【数据层】核心数据结构
│   │   ├── data_contracts.h/cpp    # 定义 JobSnapshot、MetadataSnapshot 等数据结构
│   │   └── perf_monitor.h/cpp      # 性能监控（CPU、内存、句柄数）
│   │
│   ├── collector/          # 【采集层】DEVMODE 元数据提取
│   │   └── metadata_collector.h/cpp  # 从 JOB_INFO_2W 提取份数、双面、颜色等
│   │
│   ├── parser/             # 【解析层】PrintTicket XML 解析
│   │   └── print_ticket_parser.h/cpp # 从 PrintTicket XML 提取真实打印参数
│   │
│   ├── watcher/            # 【监听层】打印队列监听
│   │   └── print_watcher.h/cpp      # 用 Win32 API 监听打印机变化
│   │
│   ├── filter/             # 【过滤层】XPS 管道过滤器
│   │   ├── filter_pipeline.h/cpp     # IPrintPipelineFilter 实现
│   │   └── print_ticket_extractor.h/cpp # 从 IPartPrintTicket 提取 XML
│   │
│   ├── bridge/             # 【桥接层】合并两层数据
│   │   └── metadata_merger.h/cpp     # 合并 Spooler 数据和 PrintTicket 数据
│   │
│   ├── storage/            # 【存储层】数据持久化
│   │   ├── json_serializer.h/cpp     # JSON 序列化
│   │   ├── xml_serializer.h/cpp      # XML 序列化
│   │   ├── job_snapshot_writer.h/cpp # 异步写入线程
│   │   ├── page_artifact_writer.h/cpp# 页面级工件写入
│   │   └── file_utils.h/cpp          # 原子文件写入工具
│   │
│   ├── ipc/                # 【通信层】进程间通信
│   │   └── named_event.h/cpp         # 命名事件（过滤器→UI 通知）
│   │
│   ├── common/             # 【公共层】通用工具
│   │   ├── app_config.h/cpp          # 应用配置（JSON 配置文件读写）
│   │   ├── logger.h/cpp              # 环形缓冲日志
│   │   └── build_info.h/cpp          # 版本号信息
│   │
│   ├── app/                # 【CLI 程序】命令行工具
│   │   └── main.cpp                  # wpce.exe 入口
│   │
│   └── ui/                 # 【GUI 程序】Qt 界面
│       ├── CMakeLists.txt            # GUI 构建配置
│       ├── main.cpp                  # wpce_ui.exe 入口
│       ├── main_window.h/cpp         # 主窗口（所有 UI 布局和交互）
│       ├── watcher_bridge.h/cpp      # 后端↔前端桥接（多线程→Qt信号）
│       ├── user_group_model.h/cpp    # 用户分组树模型（核心数据模型）
│       ├── user_group_delegate.h     # 树视图自定义绘制（头文件，header-only）
│       ├── job_detail_panel.h/cpp    # 右侧作业详情面板
│       ├── job_table_model.h/cpp     # 作业表格模型
│       ├── user_task_model.h/cpp     # 用户任务模型（按天汇总）
│       ├── user_task_panel.h/cpp     # 任务面板
│       ├── pricing_config.h          # 价格计算逻辑
│       ├── statistics_panel.h/cpp    # 统计面板
│       ├── lifecycle_timeline.h/cpp  # 生命周期时间线
│       ├── settings_dialog.h/cpp     # 设置对话框
│       └── ...
│
├── tests/                  # ===== 测试 =====
│   ├── CMakeLists.txt
│   ├── contract_serialization_tests.cpp  # 序列化合约测试
│   └── print_ticket_parser_tests.cpp    # 解析器单元测试
│
├── tools/                  # ===== 工具 =====
│   ├── simulator.cpp       # 打印模拟器（模拟打印机行为）
│   ├── stress_test.cpp     # 压力测试工具
│   └── filter_test_harness.cpp # 过滤器测试工具
│
├── scripts/                # ===== 脚本 =====
│   ├── build-msvc.ps1            # 一键编译脚本
│   ├── check-toolchain.ps1       # 环境检查
│   ├── install-xps-filter.ps1    # 安装 XPS 过滤器（需管理员）
│   ├── package-release.ps1       # 打包发布版本
│   ├── run-print-simulation.ps1  # 运行打印模拟测试
│   ├── run-scenario-tests.ps1    # 运行场景集成测试
│   ├── setup-virtual-printers.ps1# 创建虚拟打印机
│   └── run-static-analysis.ps1   # 运行静态分析
│
├── docs/                   # 文档
└── artifacts/              # 测试产物（自动生成）
```

### 数据流向图

```
打印发生
    │
    ├─→ XPS 过滤器 (filter/) ──→ PrintTicket XML ──→ parser/ ──→ MetadataSnapshot
    │                                                    ↑
    │                                              解析颜色、双面、
    │                                              份数、纸张等
    │
    └─→ Spooler 监听 (watcher/) ──→ JOB_INFO_2W ──→ collector/ ──→ MetadataSnapshot
                                                              ↑
                                                        提取页数、
                                                        状态等
                                │
                                ▼
                    MetadataMerger (bridge/)
                    合并两层数据（PrintTicket 优先）
                                │
                                ▼
                    WatcherBridge (ui/)
                    Qt 信号/槽 连接后端和前端
                                │
                                ▼
                    UserGroupModel + JobDetailPanel
                    用户分组树 + 参数详情面板
```

---

## 6. 如何调试 Debug

### 6.1 方法一：Visual Studio 图形调试（推荐新手）

#### 步骤：

1. **打开 VS 解决方案**：

   ```
   双击 build/vs2026-x64-qt/WPCE.sln
   ```

   或者在 VS 中：文件 → 打开 → CMake → 选择项目根目录的 `CMakeLists.txt`

2. **设为启动项目**：
   - 在解决方案资源管理器中，右键 `wpce_ui` → **设为启动项目**

3. **设断点**：
   - 在你关心的代码行左侧灰色区域点击，出现红色圆点
   - 例如：在 `src/ui/main_window.cpp` 的 `onStartClicked` 函数打断点

4. **按 F5 启动调试**：
   - 程序运行到断点处会暂停
   - 你可以查看变量值、调用栈、内存状态

5. **常用调试操作**：

   | 快捷键 | 功能 |
   |--------|------|
   | F5 | 继续运行 |
   | F10 | 单步跳过（不进入函数内部） |
   | F11 | 单步进入（进入函数内部） |
   | Shift+F11 | 从当前函数返回 |
   | Ctrl+Shift+F5 | 重新启动调试 |
   | Shift+F5 | 停止调试 |

6. **查看变量**：
   - 鼠标悬停在变量上可以看到值
   - 在"监视"窗口添加想跟踪的变量
   - 在"即时窗口"可以执行表达式

### 6.2 方法二：命令行 GDB 风格调试

```powershell
# 编译 Debug 版本
cmake --build build/vs2026-x64 --config Debug

# 用 VS 调试器启动
devenv /debugexe build/vs2026-x64\src\Debug\wpce.exe --watch-printer "HP LaserJet"
```

### 6.3 方法三：用 DebugView 查看实时日志

WPCE 的 XPS 过滤器运行在 Spooler 进程内（不是你的程序），无法用断点调试。这时用 **DebugView**（Sysinternals 工具）查看日志：

1. 下载 DebugView：https://docs.microsoft.com/en-us/sysinternals/downloads/debugview
2. 以管理员身份运行 DebugView
3. 在 GUI 中点击菜单：Capture → **Capture Global Win32**
4. 使用 WPCE GUI 或 CLI 启动监听
5. DebugView 会实时显示过滤器输出的日志，格式如：

   ```
   [HP LaserJet|D|init] Printer opened, change notification registered
   [HP LaserJet|I|phase] Job 42: 0x10 -> "printing"
   [HP LaserJet|D|meta] Metadata for Job 42: pages=3 printed=1 copies=2 duplex=long-edge (book)
   ```

### 6.4 方法四：启用详细日志输出

CLI 工具支持 `--verbose` 标志：

```powershell
.\build\vs2026-x64\src\Debug\wpce.exe --watch-printer "HP LaserJet" --verbose
```

输出会包含详细的决策日志：
```
[12:34:56] [INFO ] [init] Printer "HP LaserJet" opened
[12:34:57] [DEBUG] [notify] Notification fired, change_flags=0x80, 1 job(s)
[12:34:57] [DEBUG] [getjob] Job 42: fetched JOB_INFO_2W (status=spooling, pages=3/5)
[12:34:57] [DEBUG] [phase] Job 42: 0x8 -> "spooling"
```

### 6.5 方法五：查看输出文件

WPCE 会将采集的数据写入文件。检查这些文件可以确认数据是否正确：

```powershell
# 查看输出目录（在 wpce_config.json 中配置）
ls .\data\capture-*/

# 查看作业快照
cat .\data\capture-*\jobs\job-42.json

# 查看生命周期摘要
cat .\data\capture-*\jobs\job-42-lifecycle.json

# 查看 PrintTicket 元数据（如果有 XPS 过滤器）
cat .\data\capture-*\job-*-pt-metadata.json
```

### 6.6 常见调试场景

#### 场景 A：打印任务没被检测到

1. 检查打印队列是否有任务：`Get-PrintJob -PrinterName "打印机名"`
2. 确认打印机名称完全匹配（区分大小写、空格）
3. 查看 `--verbose` 日志中是否有 `OpenPrinterW failed`

#### 场景 B：颜色/双面/份数显示为"-"（未报告）

1. 检查是否安装了 XPS 过滤器：`reg query HKLM\CLSID\{B1C8E3F0-...}`
2. 在 DebugView 中查看是否有 `[WPCE]` 开头的日志
3. 查看输出目录中的 `*-pt-metadata.json` 文件是否存在
4. 如果是 v3（GDI）驱动，PrintTicket 不可用，只能依赖 DEVMODE

#### 场景 C：程序崩溃

1. 用 Visual Studio 调试模式运行，崩溃时会自动断在崩溃点
2. 查看调用栈（Call Stack）确定崩溃位置
3. 检查 `artifacts/` 目录下是否有 dump 文件

---

## 7. 如何添加新功能

### 7.1 添加一个新的打印参数（如"水印"）

假设你要采集一个新的打印参数"水印"（Watermark），以下是完整步骤：

#### 第 1 步：在数据结构中添加字段

编辑 `src/core/data_contracts.h`：

```cpp
struct MetadataSnapshot
{
  // ... 已有字段 ...
  FieldValue media_type;
  FieldValue page_range;
  FieldValue watermark;      // ← 新增：水印设置
  std::string dm_fields_hex;
};
```

#### 第 2 步：在 PrintTicket 解析器中提取

编辑 `src/parser/print_ticket_parser.cpp`，在 `parse_print_ticket_xml` 函数末尾添加：

```cpp
  // 水印
  {
    std::string opt = find_feature_option(pt_xml, "psk:PageWatermark");
    set_if_present(snapshot.watermark, map_watermark_option(opt));
  }
```

#### 第 3 步：在序列化器中输出

编辑 `src/storage/json_serializer.cpp`，在 `to_json(const MetadataSnapshot&)` 中添加：

```cpp
  output << ",\n  ";
  append_field_value(output, "watermark", snapshot.watermark);
```

#### 第 4 步：在 UI 中显示

编辑 `src/ui/job_detail_panel.cpp`，在参数网格中添加：

```cpp
  root_->addWidget(makeParamGrid({
    {QStringLiteral("颜色"),   &metadata.color},
    {QStringLiteral("介质"),   &metadata.media_type},
    {QStringLiteral("质量"),   &metadata.print_quality},
    {QStringLiteral("水印"),   &metadata.watermark},  // ← 新增
  }));
```

#### 第 5 步：编译并测试

```powershell
cmake --build build/vs2026-x64 --config Debug --target wpce_parser_tests
.\build\vs2026-x64\tests\Debug\wpce_parser_tests.exe
```

### 7.2 代码风格要求

本项目使用 **clang-format** 统一代码风格。在提交代码前：

```powershell
# 格式化单个文件
clang-format -i src/parser/print_ticket_parser.cpp

# 格式化所有文件
Get-ChildItem src -Recurse -Include *.cpp,*.h | ForEach-Object { clang-format -i $_.FullName }
```

关键风格规则（`.clang-format`）：
- 缩进：2 个空格（不用 Tab）
- 大括号：Allman 风格（另起一行）
- 行宽限制：120 字符
- 命名：函数用 `snake_case`，类用 `PascalCase`，常量用 `kCamelCase`

---

## 8. 如何部署到其他电脑

### 8.1 方式一：打包便携版（推荐）

```powershell
# 一键打包脚本
.\scripts\package-release.ps1
```

打包完成后，在 `release/wpce-portable/` 目录下会有完整的可发布文件夹：

```
wpce-portable/
├── wpce_ui.exe            # GUI 程序
├── wpce.exe               # CLI 工具
├── wpce_xps_filter.dll    # XPS 过滤器
├── wpce_config.json       # 配置文件
├── Qt6Core.dll            # Qt 运行时
├── Qt6Widgets.dll
├── msvcp140.dll           # VC++ 运行时
├── vcruntime140.dll
├── platforms/             # Qt 平台插件
├── Launch-WPCE.bat        # 启动脚本
├── Launch-CLI.bat         # CLI 启动脚本
└── README.txt             # 说明文件
```

将整个文件夹复制到目标电脑即可运行。

### 8.2 方式二：手动打包

如果打包脚本报错，可以手动操作：

```powershell
# 1. 编译 Release 版本
cmake --build build/vs2026-x64-qt --config Release

# 2. 收集 Qt 依赖
windeployqt.exe --release --no-translations build/vs2026-x64-qt\src\ui\Release\wpce_ui.exe

# 3. 复制 VC++ 运行时
# 从 VS 安装目录找到 msvcp140.dll 和 vcruntime140.dll
```

### 8.3 部署 XPS 过滤器（关键步骤）

要获取真实的打印参数（颜色、双面等），必须在目标电脑上安装 XPS 过滤器：

#### 方法 A：GUI 自动安装

1. **以管理员身份**运行 `wpce_ui.exe`
2. 程序会自动检测并安装 XPS 过滤器
3. 安装完成后会重启打印服务（Print Spooler）
4. 之后**不需要管理员权限**即可正常使用

#### 方法 B：脚本安装

```powershell
# 以管理员身份运行 PowerShell
.\scripts\install-xps-filter.ps1
```

#### 方法 C：手动安装

```powershell
# 1. 复制 DLL
copy wpce_xps_filter.dll "C:\Program Files\WPCE\"

# 2. 注册 COM 服务器
regsvr32 "C:\Program Files\WPCE\wpce_xps_filter.dll"

# 3. 设置环境变量
[Environment]::SetEnvironmentVariable("WPCE_OUTPUT_DIR", "C:\data", "Machine")

# 4. 重启打印服务
net stop spooler && net start spooler
```

#### 验证安装

```powershell
# 检查 COM 注册
reg query "HKLM\CLSID\{B1C8E3F0-2D4A-5B6C-7D8E-9F0A1B2C3D4E}"

# 检查环境变量
[Environment]::GetEnvironmentVariable("WPCE_OUTPUT_DIR", "Machine")
```

### 8.4 目标电脑要求

| 项目 | 最低要求 |
|------|----------|
| 操作系统 | Windows 10 x64 或 Windows 11 |
| 运行时 | Visual C++ Redistributable 2022+（打包版已包含） |
| 权限 | XPS 过滤器安装需要一次管理员权限 |
| 打印机 | 至少一台本地安装的打印机 |

### 8.5 卸载 XPS 过滤器

```powershell
# 1. 注销 COM 服务器
regsvr32 /u "C:\Program Files\WPCE\wpce_xps_filter.dll"

# 2. 恢复 FilterPipelineConfig.xml 备份
# 备份文件位于 DriverStore 目录，扩展名为 .wpce-backup

# 3. 重启打印服务
net stop spooler && net start spooler
```

---

## 9. 如何测试项目

### 9.1 单元测试

项目使用 **GoogleTest** 框架。测试代码在 `tests/` 目录。

#### 运行所有单元测试

```powershell
# 方式一：通过 CTest 运行
ctest --preset vs2026-debug

# 方式二：直接运行测试可执行文件
.\build\vs2026-x64\tests\Debug\wpce_contract_tests.exe
.\build\vs2026-x64\tests\Debug\wpce_parser_tests.exe
```

#### 运行单个测试

```powershell
# 只运行解析器测试
.\build\vs2026-x64\tests\Debug\wpce_parser_tests.exe

# 只运行某个测试用例
.\build\vs2026-x64\tests\Debug\wpce_parser_tests.exe --gtest_filter="*PageRange*"

# 列出所有测试用例
.\build\vs2026-x64\tests\Debug\wpce_parser_tests.exe --gtest_list_tests
```

#### 编写新测试

在 `tests/` 目录创建新文件，参考已有的 `print_ticket_parser_tests.cpp`：

```cpp
#include "parser/print_ticket_parser.h"
#include <gtest/gtest.h>

TEST(MyNewTest, HandlesSpecialCase)
{
  wpce::core::MetadataSnapshot snapshot;
  wpce::parser::parse_print_ticket_xml(R"(
    <psf:PrintTicket xmlns:psf="..." xmlns:psk="...">
      <psf:Feature name="psk:PageOutputColor">
        <psf:Option name="psk:Monochrome" />
      </psf:Feature>
    </psf:PrintTicket>
  )", snapshot);

  EXPECT_EQ(snapshot.color.state, wpce::core::FieldState::Reported);
  EXPECT_EQ(snapshot.color.value, "monochrome");
}
```

然后在 `tests/CMakeLists.txt` 中添加新的测试目标。

### 9.2 打印模拟测试

项目包含一个打印模拟器 `wpce_sim.exe`，可以在没有真实打印机的情况下测试：

#### 创建虚拟打印机

```powershell
# 以管理员身份运行
.\scripts\setup-virtual-printers.ps1
```

这会创建两个虚拟打印机：
- **WPCE Sim Printer**：正常打印机（端口 nul:，数据直接丢弃）
- **WPCE Fault Printer**：故障打印机（端口指向不存在的网络地址）

#### 运行模拟测试

```powershell
# 运行所有场景测试
.\scripts\run-scenario-tests.ps1

# 运行单个场景
.\scripts\run-scenario-tests.ps1 -Scenario "normal"
.\scripts\run-scenario-tests.ps1 -Scenario "stall"
.\scripts\run-scenario-tests.ps1 -Scenario "cancel-mid"
```

#### 可用的测试场景

| 场景名 | 说明 |
|--------|------|
| `normal` | 正常打印流程 |
| `fast-disappear` | 任务很快从队列消失 |
| `cancel-mid` | 打印中途取消 |
| `stall` | 打印停滞（打印机暂停） |
| `printer-error` | 打印机故障 |
| `high-concurrency` | 高并发（50 个任务同时打印） |
| `param-variety` | 多种参数组合 |

#### 运行打印模拟器

```powershell
# 直接运行模拟器
.\build\vs2026-x64\tools\Debug\wpce_sim.exe --target-printer "WPCE Sim Printer" --scenario normal --job-count 10
```

### 9.3 压力测试

```powershell
# 运行压力测试
.\build\vs2026-x64\tools\Debug\wpce_stress_test.exe --job-count 1000
```

### 9.4 性能测试

```powershell
# CLI 工具支持性能日志输出
.\build\vs2026-x64\src\Debug\wpce.exe --watch-printer "WPCE Sim Printer" --perf-log perf.csv --duration 60

# 查看性能数据
Import-Csv perf.csv | Format-Table
```

性能指标说明：

| 指标 | 目标值 |
|------|--------|
| 打印延迟增加 | ≤ 50ms |
| 常驻内存 | ≤ 50MB |
| 空闲 CPU | < 1% |
| 打印期间 CPU | < 5% |

### 9.5 静态分析

```powershell
# 运行 clang-tidy 和 cppcheck
.\scripts\run-static-analysis.ps1
```

### 9.6 测试矩阵

完整的测试应覆盖以下组合：

| 维度 | 变量 |
|------|------|
| 打印机类型 | v4 (XPSDrv) / v3 (GDI) |
| 驱动品牌 | HP / Canon / Epson / Brother |
| 参数组合 | 单面彩色 / 双面黑白 / A3 高质量 / N-up |
| 并发度 | 1 任务 / 10 任务 / 50 任务 |
| 异常情况 | 打印机离线 / 纸张用完 / 用户取消 |

---

## 10. 常见问题排查

### Q1：编译报错 "找不到 Qt6"

```
CMake Error: Could not find a package configuration file provided by "Qt6"
```

**解决**：确保安装了 Qt 6.8+ 的 MSVC 2022 64-bit 组件，并在 CMake 配置时指定路径：

```powershell
cmake --preset vs2026-x64-qt -DCMAKE_PREFIX_PATH="C:/Qt/6.8.2/msvc2022_64"
```

### Q2：编译报错 "找不到 Windows SDK"

```
error C1083: Cannot open include file: 'windows.h': No such file or directory
```

**解决**：在 Visual Studio Installer 中安装 Windows 10/11 SDK。

### Q3：运行 GUI 时提示 "无法启动此程序，因为计算机中丢失 Qt6Core.dll"

**解决**：确保 Qt 的 bin 目录在 PATH 中，或者使用 `windeployqt` 收集依赖：

```powershell
windeployqt.exe build\vs2026-x64-qt\src\ui\Debug\wpce_ui.exe
```

### Q4：XPS 过滤器安装后打印参数仍显示"未报告"

**排查步骤**：
1. 确认打印机使用的是 v4 驱动（不是 v3 GDI 驱动）
2. 重启打印服务：`net stop spooler && net start spooler`
3. 用 DebugView 查看是否有 `[WPCE]` 日志
4. 检查环境变量：`[Environment]::GetEnvironmentVariable("WPCE_OUTPUT_DIR", "Machine")`

### Q5：GoogleTest 下载失败

```
CMake Error: Could not download GoogleTest
```

**解决**：需要网络连接（用于从 GitHub 下载 GoogleTest）。如果网络受限，可以手动下载放到 `build/_deps/` 目录。

### Q6：打印服务（Spooler）重启后过滤器失效

**原因**：XPS 过滤器通过 FilterPipelineConfig.xml 注入，如果驱动更新可能覆盖配置。

**解决**：重新运行安装脚本：`.\scripts\install-xps-filter.ps1`

---

## 附录 A：CMake 命令速查

```powershell
# 配置
cmake --preset vs2026-x64           # 不带 GUI
cmake --preset vs2026-x64-qt        # 带 GUI

# 编译
cmake --build --preset vs2026-debug           # Debug
cmake --build build/vs2026-x64-qt --config Release  # Release
cmake --build build/vs2026-x64 --target wpce_parser_tests  # 只编译某个目标

# 测试
ctest --preset vs2026-debug              # 运行所有测试
ctest --preset vs2026-debug -R parser    # 运行匹配名称的测试

# 清理（重新编译）
Remove-Item -Recurse -Force build/vs2026-x64
cmake --preset vs2026-x64
```

## 附录 B：PowerShell 命令速查

```powershell
# 查看打印机列表
Get-Printer | Select-Object Name, DriverName, PortName

# 查看打印队列
Get-PrintJob -PrinterName "HP LaserJet"

# 清空打印队列
Get-PrintJob -PrinterName "HP LaserJet" | Remove-PrintJob

# 暂停/恢复打印机
$printer = Get-CimInstance Win32_Printer -Filter "Name='HP LaserJet'"
Invoke-CimMethod -InputObject $printer -MethodName "Pause"
Invoke-CimMethod -InputObject $printer -MethodName "Resume"

# 重启打印服务
Restart-Service -Name Spooler -Force
```

## 附录 C：项目关键文件索引

| 如果你想修改... | 编辑这个文件 |
|----------------|-------------|
| 打印监听逻辑 | `src/watcher/print_watcher.cpp` |
| DEVMODE 参数提取 | `src/collector/metadata_collector.cpp` |
| PrintTicket XML 解析 | `src/parser/print_ticket_parser.cpp` |
| 两层数据合并策略 | `src/ui/watcher_bridge.cpp`（`merge_field` 函数） |
| 数据结构定义 | `src/core/data_contracts.h` |
| 价格计算 | `src/ui/pricing_config.h` |
| 用户分组逻辑 | `src/ui/user_group_model.cpp` |
| GUI 主窗口布局 | `src/ui/main_window.cpp` |
| 参数详情面板 | `src/ui/job_detail_panel.cpp` |
| 树视图自定义绘制 | `src/ui/user_group_delegate.h` |
| JSON 序列化 | `src/storage/json_serializer.cpp` |
| 配置文件格式 | `src/common/app_config.h` |
| XPS 过滤器 | `src/filter/filter_pipeline.cpp` |
| 测试用例 | `tests/print_ticket_parser_tests.cpp` |

---

> 本文档最后更新：2026-06-21
> 版本：WPCE v0.1.0
