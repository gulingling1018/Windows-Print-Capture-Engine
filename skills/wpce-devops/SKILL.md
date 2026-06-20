---
name: wpce-devops
description: DevOps workflow for Windows Print Capture Engine. Use when setting up WPCE Git history, CMake builds, MSVC or WDK toolchains, CI checks, clang-tidy, cppcheck, packaging, symbols, changelogs, release reports, signing, rollback, or deployment automation.
---

# WPCE DevOps

## Overview

Use this skill to keep WPCE build, verification, packaging, and release processes reproducible.

## Required Context

Before DevOps work:

- Read `readme`.
- Read `docs/PROJECT_PLAN_OPTIMIZED.md`.
- Read `docs/ARCHITECTURE.md`.
- Check local tool availability before assuming a build or release path.

## Toolchain Targets

- Git for rollback and release traceability.
- CMake for project generation.
- MSVC for Windows builds.
- Ninja when available.
- Windows SDK for user-mode print APIs.
- WDK for XPSDrv work.
- GoogleTest for tests.
- clang-tidy and cppcheck for release gates.
- Symbol generation for release packages.

## Workflow

1. Verify current repository state and avoid overwriting user changes.
2. Confirm the active phase and required tools.
3. Add deterministic build and check commands.
4. Make generated artifacts disposable or place them under ignored build directories.
5. Keep release outputs versioned and reproducible.
6. Update archive notes when build, CI, or release behavior changes.

## Release Package Requirements

Each release must include:

- Source.
- Binary.
- Symbols.
- Changelog.
- Test report.
- Rollback notes.

## CI and Checks

- Configure fast checks first: configure, build, unit tests, static checks.
- Add integration and stress checks as separate jobs because they need printers and longer runtimes.
- Keep WDK and signing jobs separate from MVP-A user-mode builds.
- Fail release gates on missing reports, missing symbols, failed static checks, or untracked packaging inputs.

## Local Environment Notes

If `cl`, `msbuild`, or WDK tools are installed but not in `PATH`, prefer launching through the Visual Studio developer environment or resolving the full installed paths.
