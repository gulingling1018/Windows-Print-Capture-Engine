---
name: wpce-development
description: C++20 development workflow for Windows Print Capture Engine. Use when implementing WPCE modules, CMake targets, Win32 Spooler watchers, metadata collectors, storage, XPS filters, tests, refactors, RAII wrappers, async queues, or Windows-specific error handling according to the approved project plan.
---

# WPCE Development

## Overview

Use this skill for implementation work in WPCE. Follow the optimized plan and architecture baseline before writing code.

## Required Context

Before coding:

- Read `readme`.
- Read `docs/PROJECT_PLAN_OPTIMIZED.md`.
- Read `docs/ARCHITECTURE.md`.
- Search for existing code before adding modules or helpers.

## Implementation Rules

- Use C++20.
- Prefer MSVC-compatible code and CMake targets.
- Wrap Windows `HANDLE`, printer handles, notification handles, COM pointers, and allocated buffers in RAII.
- Keep print notification waits off UI or service control threads.
- Keep disk I/O out of the print event hot path.
- Store driver-dependent fields as reported, unsupported, not reported, unknown, or error.
- Make shutdown idempotent and cancellation-aware.
- Do not introduce Electron or Java.
- Use small C# only for management tooling if explicitly planned.

## Module Guidance

- `src/core`: lifecycle, config, queues, service orchestration.
- `src/watcher`: Win32 Spooler notification and job lookup.
- `src/collector`: `JOB_INFO_2` and `DEVMODE` extraction.
- `src/storage`: JSON, XML, and artifact persistence.
- `src/filter`: XPSDrv filter code only after Phase 3 gate.
- `src/common`: shared utilities without feature-specific behavior.

## Coding Workflow

1. Confirm the phase and acceptance criteria.
2. Inspect existing files and avoid duplicate helpers.
3. Make the smallest module-level change that satisfies the phase.
4. Add focused unit tests or a reproducible manual check.
5. Run the relevant build, tests, and static checks available in the repo.
6. Inspect the diff before reporting completion.

## Verification Expectations

- Phase 1: local print job can produce `job.json`.
- Phase 2: known samples validate metadata mappings.
- Phase 3: filter loads, prints through, and extracts a minimal PrintTicket.
- Always report skipped checks and why.
