# WPCE Architecture Baseline

Last updated: 2026-06-19

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

### Job Snapshot

```json
{
  "schema_version": 1,
  "job_id": 12,
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
  "orientation": { "value": "portrait", "state": "reported" }
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

