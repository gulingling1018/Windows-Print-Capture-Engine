---
name: wpce-architecture
description: Architecture and feasibility guidance for Windows Print Capture Engine. Use when designing WPCE modules, optimizing the project plan, reviewing Win32 Spooler or XPSDrv tradeoffs, deciding MVP scope, creating architecture docs, or evaluating PrintTicket, FixedPage, WDK, COM, performance, and driver-pipeline constraints.
---

# WPCE Architecture

## Overview

Use this skill to keep WPCE architecture decisions aligned with the project plan and Windows printing constraints. Treat `readme`, `docs/PROJECT_PLAN_OPTIMIZED.md`, and `docs/ARCHITECTURE.md` as the main project sources.

## Required Context

Before architecture work:

- Read `readme` for the original requirement.
- Read `docs/PROJECT_PLAN_OPTIMIZED.md` for feasibility gates and revised phases.
- Read `docs/ARCHITECTURE.md` for module boundaries and data contracts.
- Use Microsoft Learn as the source of truth for Win32 Spooler, XPSDrv, COM, and PrintTicket assumptions.

## Decision Rules

- Separate MVP-A user-mode capture from MVP-B controlled XPS capture.
- Do not promise Page PrintTicket or FixedPage capture from passive monitoring of arbitrary printers.
- Add a feasibility gate before XPSDrv implementation.
- Keep hot-path event work bounded and asynchronous.
- Prefer narrow modules: watcher, collector, storage, filter, common, core.
- Treat driver-specific fields as optional unless verified by a sample set.
- Record risks as explicit gates rather than hidden implementation assumptions.

## Architecture Workflow

1. Identify whether the request touches MVP-A, MVP-B, or both.
2. Map the request to affected modules and data contracts.
3. Check whether it changes performance, install, signing, or rollback constraints.
4. Prefer extending existing planned modules over adding new boundaries.
5. Update architecture or plan docs when the decision changes project behavior.
6. Call out blocked assumptions early, especially WDK, signing, driver installation, and page capture guarantees.

## Output Expectations

- Give a feasibility rating when evaluating scope.
- Include dependencies, acceptance criteria, and risk controls.
- Use concrete phase names from `docs/PROJECT_PLAN_OPTIMIZED.md`.
- When an assumption is based on external Windows behavior, cite official Microsoft documentation.
