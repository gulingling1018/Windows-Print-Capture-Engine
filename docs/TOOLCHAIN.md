# WPCE Toolchain

Last updated: 2026-10-24

## Required for Phase 0-2

- Windows 10/11 x64.
- CMake 3.25 or newer.
- MSVC toolchain.
- Windows SDK.
- Git.
- cppcheck for static analysis.

## Required Before Phase 3

- WDK tooling for XPSDrv development.
- Driver/filter packaging utilities such as `inf2cat` and `stampinf`.
- Signing strategy for development and release.

## Local Commands

Check the local environment:

```powershell
.\scripts\check-toolchain.ps1
```

Configure, build, and test with Visual Studio 2026 generator:

```powershell
.\scripts\build-msvc.ps1
```

If Visual Studio is installed outside the default discovery paths, pass the instance root:

```powershell
.\scripts\build-msvc.ps1 -VisualStudioRoot "D:\download\Microsoft Visual Studio18_Community"
```

Qt Debug/Release 使用绑定到同一 Qt 目录的配置、构建和测试预设：

```powershell
.\scripts\build-msvc.ps1 -WithQt -Configuration Debug
.\scripts\build-msvc.ps1 -WithQt -Configuration Release
```

严格发布静态检查要求两个分析器、MSVC/Qt 实际编译数据库与 CMake File API 目标模型；缺一项返回非零。先在 x64 VS 开发者命令行配置并构建 Ninja 目录：

```powershell
cmake --preset ninja-msvc-qt
cmake --build build/ninja-msvc-qt --config Debug
.\scripts\run-static-analysis.ps1 -BuildDirectory build/ninja-msvc-qt -Configuration Debug -ReportPath artifacts/static-strict.json
```

CMake 3.25/3.26 在首次创建查询后需再配置一次；3.27+ 使用 cmake_file_api 在本次生成目标模型。相同源文件在不同目标中的编译命令分别检查；生成代码、依赖和未参与目标的旧源文件不混入本项目手写源码范围。

局部检查必须明确文件，不可当作发布门禁通过：

```powershell
.\scripts\run-static-analysis.ps1 -Mode Local -Files src/filter/filter_entry.cpp -ReportPath artifacts/static-local.json
```

Local 使用简化 Qt 宏且跳过系统头缺失提示；Strict 使用真实配置，cppcheck 非零或 clang-tidy 告警/解析失败均阻断。报告保留实际范围、执行/跳过项和 release_gate 标志。当前机器 clang-tidy 未安装，严格检查会失败，不能借 Local 结果宣称发布通过。

## CMake Presets

- `vs2026-x64`: Visual Studio 2026 x64 generator.
- `ninja-msvc`: Ninja Multi-Config, intended for a Visual Studio developer shell.
- `vs2026-x64-qt`：Qt 配置；配套 `vs2026-qt-debug`、`vs2026-qt-release` 构建与测试预设。
- `ninja-msvc-qt`：Qt/测试/编译数据库导出，用于严格静态分析。

## XPS 安装与只读检查

启动 GUI 只读探测；“XPS 设置”提供明确安装和重启入口。脚本可指定准确 Release 产物目录或 DLL，二者互斥；不递归挑选旧产物：

```powershell
.\scripts\install-xps-filter.ps1 -BuildDirectory build/fix-20261024-08 -OutputDir C:/WPCE-data -DryRun
.\scripts\install-xps-filter.ps1 -BuildDirectory build/fix-20261024-08 -OutputDir C:/WPCE-data -Probe -AsJson
```

DryRun/Probe 不需管理员且不修改系统。真实安装在约定的隔离设备中使用 x64 管理员 PowerShell，去掉 DryRun；只有显式指定 RestartSpooler 才会停止/启动打印服务。未重启记录为 restart_required，配置检查通过仍需真实试打验证。正确 COM 位置为 `HKLM\SOFTWARE\Classes\CLSID\{B1C8E3F0-2D4A-5B6C-7D8E-9F0A1B2C3D4E}`。

旧 sentinel 不作成功依据。`%ProgramFiles%/WPCE/filter-installation.json` 保存版本、DLL 哈希及目标配置哈希；`_install-transactions` 保存原始字节、注册值、环境变量及其值类型。失败自动回滚并核对，回滚不完整或进程中断时保留事务并阻断重复安装，需在维护窗口根据事务中的原件恢复与核对。详见 [08]。

## Current Bootstrap Notes

- The initial test target uses CTest with a small executable test to avoid adding third-party dependencies before the baseline is stable.
- GoogleTest remains the planned unit test framework for feature phases.
- XPSDrv work must not start until the WDK/signing gate is resolved.
