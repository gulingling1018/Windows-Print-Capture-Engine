# WPCE Toolchain

Last updated: 2026-06-19

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

Run available static checks:

```powershell
.\scripts\run-static-analysis.ps1
```

## CMake Presets

- `vs2026-x64`: Visual Studio 2026 x64 generator.
- `ninja-msvc`: Ninja Multi-Config, intended for a Visual Studio developer shell.

## Current Bootstrap Notes

- The initial test target uses CTest with a small executable test to avoid adding third-party dependencies before the baseline is stable.
- GoogleTest remains the planned unit test framework for feature phases.
- XPSDrv work must not start until the WDK/signing gate is resolved.
