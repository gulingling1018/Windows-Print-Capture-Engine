---
name: wpce-qa
description: QA workflow for Windows Print Capture Engine. Use when designing WPCE test plans, unit tests, integration tests, print-flow validation, metadata accuracy checks, stress tests, performance budgets, leak checks, compatibility matrices, or release acceptance reports.
---

# WPCE QA

## Overview

Use this skill to turn WPCE phase requirements into measurable tests. Keep MVP-A user-mode tests separate from MVP-B XPS pipeline tests.

## Required Context

Before QA work:

- Read `readme`.
- Read `docs/PROJECT_PLAN_OPTIMIZED.md`.
- Read `docs/ARCHITECTURE.md`.
- Identify the phase and acceptance criteria.

## Test Categories

- Unit tests: parsers, mappers, state machines, path utilities, error handling.
- Integration tests: Microsoft Print to PDF, Office, PDF reader, browser printing.
- Accuracy tests: compare reported metadata against configured print settings.
- Stress tests: 10000+ jobs, repeated cancellation, pause/resume, printer offline cases.
- Stability tests: CPU, memory, handles, threads, disk usage over time.
- XPS tests: filter load, print-through, PrintTicket extraction, FixedPage correlation.

## QA Rules

- Define success metrics before executing tests.
- Measure field-level accuracy separately from job capture success.
- Treat unsupported driver fields as a valid explicit outcome, not an automatic failure.
- Include crash, leak, and handle-growth checks for long-running tests.
- Use reproducible scripts where possible.
- Keep test artifacts with schema version, printer name, driver name, OS version, and tool versions.

## Phase Acceptance Focus

- Phase 1: JobId, User, Printer, Document, SubmitTime are captured.
- Phase 2: Copies, Duplex, Color, PaperSize, Orientation are mapped or explicitly marked unsupported.
- Phase 3: minimal XPS filter loads, prints through, and captures a PrintTicket.
- Phase 4: page artifacts correlate back to the original job.
- Phase 5: stress and stability thresholds are met.

## Report Format

When reporting QA results, include:

- Test scope.
- Environment.
- Data set.
- Pass/fail summary.
- Metrics.
- Defects and reproduction steps.
- Residual risk.
