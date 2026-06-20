# Windows Print Capture Engine (WPCE) Optimized Project Plan

Last updated: 2026-06-19

## 1. Planning Basis

The original requirement source is `readme`. This optimized plan keeps the same product goal but separates the work into feasibility tiers so the project can deliver a working MVP before entering driver/filter-level work.

Primary technical assumptions:

- User-mode print job monitoring can be built with Win32 Spooler APIs.
- Job and device metadata can be collected from `JOB_INFO_2` and `DEVMODE` when drivers expose the fields.
- Page-level PrintTicket and FixedPage capture requires a controlled XPS print path, normally an XPSDrv filter or equivalent controlled printer pipeline.
- Monitoring arbitrary existing printers is not enough to reliably capture Page PrintTicket or FixedPage references.

Official references used for feasibility:

- Microsoft Learn: `FindFirstPrinterChangeNotification`
  https://learn.microsoft.com/en-us/windows/win32/printdocs/findfirstprinterchangenotification
- Microsoft Learn: `JOB_INFO_2`
  https://learn.microsoft.com/en-us/windows/win32/printdocs/job-info-2
- Microsoft Learn: Implementing XPS Filters
  https://learn.microsoft.com/en-us/windows-hardware/drivers/print/implementing-xps-filters
- Microsoft Learn: XPS Filters
  https://learn.microsoft.com/en-us/windows-hardware/drivers/print/xps-filters
- Microsoft Learn: Filter Pipeline Configuration File
  https://learn.microsoft.com/en-us/windows-hardware/drivers/print/filter-pipeline-configuration-file

## 2. Optimized Product Scope

### MVP-A: User-Mode Print Capture

MVP-A is the first shippable milestone. It observes existing printers without installing a custom print driver.

Included:

- Print job event detection.
- JobId, user, document name, printer name, submit time.
- Best-effort print parameter extraction from `JOB_INFO_2.pDevMode`.
- JSON and XML local persistence.
- Async logging and async persistence so print event handling stays lightweight.
- Manual and automated validation against local printers such as Microsoft Print to PDF.

Excluded from MVP-A:

- Page PrintTicket.
- FixedPage reference capture.
- Guaranteed page-level capture for arbitrary existing printers.

### MVP-B: Controlled XPS Capture Path

MVP-B adds page-level capture only after a feasibility gate passes.

Included:

- Minimal XPSDrv filter proof of concept.
- Job, document, and page PrintTicket extraction from the XPS pipeline.
- FixedPage reference capture.
- Controlled printer installation and rollback path.

Required preconditions:

- Complete WDK toolchain, including driver packaging utilities.
- Explicit decision that WPCE may install or control a printer/driver pipeline.
- Driver/filter signing and install strategy for development and release.

## 3. Feasibility Matrix

| Capability | Target Phase | Feasibility | Main Dependency | Decision |
|---|---:|---:|---|---|
| Repository baseline, CMake, docs, Git | Phase 0 | High | Local tooling | Start immediately |
| Print job listening | Phase 1 | High | Spooler service, Winspool.lib | MVP-A core |
| Job metadata collection | Phase 1 | High | `JOB_INFO_2` availability | MVP-A core |
| Print parameter extraction | Phase 2 | Medium-High | Driver `DEVMODE` support | MVP-A best effort, measure accuracy |
| JSON/XML persistence | Phase 2 | High | Local file permissions | MVP-A core |
| XPS Filter loading | Phase 3 | Medium | WDK, COM, pipeline config, signing | Feasibility gate required |
| PrintTicket capture | Phase 3 | Medium | Controlled XPS pipeline | MVP-B only |
| FixedPage reference capture | Phase 4 | Medium-Low | XPS document interfaces | MVP-B only |
| 10000-job stress test | Phase 5 | Medium | Test harness and printer automation | After MVP-A/B |
| 30-day stability target | Phase 5 | Medium | Monitoring and leak checks | Release hardening |

## 4. Revised Phases

### Phase 0: Engineering Baseline

Goal:

- Make the project buildable, testable, and reversible before feature work.

Deliverables:

- Git repository baseline.
- `src/`, `tests/`, `docs/`, `scripts/`, `tools/`, `build/`, `deploy/`.
- CMake project configured for C++20 and MSVC.
- Toolchain bootstrap notes for MSVC, Windows SDK, WDK, GoogleTest, clang-tidy, cppcheck.
- Initial data contracts for job, metadata, PrintTicket, and FixedPage artifacts.
- Logging and persistence design.

Acceptance:

- Empty project config builds.
- Static analysis command placeholders exist.
- Development archive records the baseline.

### Phase 1: Print Watcher

Goal:

- Detect print job changes and emit `job.json`.

Core work:

- Open target printer or local print server handle.
- Use printer change notifications on a background worker.
- On job add/write/set events, query job data.
- Deduplicate repeated notifications by printer and JobId.
- Persist only compact job snapshots in the hot path.

Acceptance:

- Captures JobId, User, Printer, Document, SubmitTime.
- Works with Microsoft Print to PDF on Windows 10/11 x64.
- Event handling does not block the caller-facing print flow.

### Phase 2: Metadata Collector and Local Store

Goal:

- Extract common print parameters and persist structured artifacts.

Core work:

- Map `DEVMODE` fields to Copies, Duplex, Color, PaperSize, Orientation.
- Record unknown or unsupported fields explicitly instead of guessing.
- Persist `metadata.json` and optional XML.
- Add schema versioning.

Acceptance:

- Accuracy is measured against a known sample set.
- 95% accuracy applies only to supported printer/driver combinations.
- Unsupported fields are visible in output diagnostics.

### Phase 3: XPS Filter Feasibility Gate

Goal:

- Prove that WPCE can enter a controlled XPS pipeline without breaking printing.

Core work:

- Build minimal COM DLL implementing `IPrintPipelineFilter`.
- Add filter pipeline configuration.
- Verify filter load/unload and no-UI behavior.
- Extract at least one Job or Document PrintTicket.

Gate result:

- If successful, continue to MVP-B.
- If blocked by WDK, signing, installation, or driver model constraints, keep MVP-A as the shippable product and redesign page-level capture.

Acceptance:

- Filter loads in a controlled development printer.
- Normal print output still succeeds.
- Failure path is safe and diagnosable.

### Phase 4: PrintTicket and FixedPage Capture

Goal:

- Capture page-level artifacts in the controlled XPS path.

Core work:

- Extract Job, Document, and Page PrintTicket.
- Capture FixedPage references or serialized `.fpage` artifacts.
- Stream or asynchronously persist large page artifacts.

Acceptance:

- Page artifacts are correlated with JobId and document metadata.
- Capture failures do not fail the print job unless explicitly configured.
- Print delay remains within the phase budget.

### Phase 5: Stability, Performance, and Release

Goal:

- Convert the implementation into a release candidate.

Core work:

- 10000+ print-job stress test.
- 7-day stability test before release; 30-day target for production confidence.
- Monitor CPU, memory, handles, threads, and disk growth.
- Run clang-tidy, cppcheck, unit tests, integration tests, and packaging checks.

Acceptance:

- Crash count: 0.
- Memory leak count: 0.
- Idle CPU < 1%; printing CPU < 5%.
- Resident memory <= 50 MB for MVP-A service.
- Added print-chain delay <= 50 ms for controlled measurement points.

## 5. Risk Controls

| Risk | Control |
|---|---|
| Page capture is not possible from passive monitoring | Treat page capture as MVP-B controlled-pipeline work only |
| Driver-dependent `DEVMODE` fields vary | Store unsupported/unknown state and measure accuracy by driver |
| Print notification APIs can block | Use background workers and bounded queues |
| Synchronous disk I/O affects print performance | Use async persistence and backpressure |
| XPS filter failures break print jobs | Keep minimal filter first, add safe fallback and diagnostics |
| WDK packaging/signing blocks release | Add Phase 3 gate before committing to MVP-B |
| Long-running service leaks handles | Add handle/thread counters to stress test from Phase 1 onward |

## 6. Required Project Skills

The project uses four local Codex skills under `skills/`:

- `wpce-architecture`: architecture, feasibility, module boundaries, print-pipeline decisions.
- `wpce-development`: C++20 implementation, tests, refactoring, Windows API work.
- `wpce-qa`: test strategy, compatibility, stress, leak and performance validation.
- `wpce-devops`: build, CI, packaging, release, rollback, toolchain checks.

These skills are project artifacts. To make them auto-discoverable by Codex sessions, install or copy them into the configured Codex skills directory.

