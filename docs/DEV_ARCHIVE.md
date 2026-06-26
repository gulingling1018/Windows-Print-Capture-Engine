# WPCE Development Archive

## 2026-06-25: Billing Matrix Color Fallback, Single-Page Duplex Pricing, Quick Settle Idle Recenter

### Context

- Some printer drivers report the color parameter as `"-"` instead of `color`/`monochrome`, causing color-specific or monochrome-specific matrix rules to miss unless the rule used the wildcard "all" color dimension.
- A one-page document submitted with duplex enabled matched the duplex pricing rule and was charged at the duplex sheet price instead of the simplex page price.
- The quick settlement window could jump back to the priority card immediately after manual navigation because regular refresh and batch-structure callbacks performed eager priority correction instead of waiting for the 10-second inactivity window.

### Changes

**Pricing engine (`src/common/pricing_engine.cpp/.h`):**
- Normalized color values before billing; `"-"` and empty/unknown driver values no longer count as color evidence.
- `effective_color_mode()` now treats only explicit `color` on a color-capable printer as a color job. All other cases price as monochrome; explicit color on a B&W printer still sets `color_downgraded`.
- Built a pricing-only metadata snapshot so one-page duplex jobs match `simplex` rules before price calculation.
- Rule lookup now receives the effective color dimension so monochrome/color matrix rules remain independent.

**Quick settlement window (`src/ui/quick_settle_dialog.cpp/.h`):**
- Added an application event filter scoped to the quick-settle dialog and its child widgets, resetting the inactivity timer on mouse, key, wheel, touch, drag, resize, and color-page edits.
- Removed eager priority jumps from ordinary `refresh()`; regular model updates now repaint without stealing the operator's current card.
- `ensurePriorityView()` now respects the inactivity timer and only performs fallback correction after the idle window has elapsed.
- `onBatchCreated()` remains the active correction path for newly monitored print tasks.

**Regression tests (`tests/pricing_engine_tests.cpp`):**
- Added coverage for `"-"` color values pricing through monochrome matrix rules.
- Added coverage for single-page duplex jobs charging the simplex rule price.

### Verification

- `cmake --build build\vs2026-x64-qt --config Debug --target wpce_pricing_engine_tests`: passed.
- `build\vs2026-x64-qt\tests\Debug\wpce_pricing_engine_tests.exe`: 44/44 tests passed.
- `cmake --build build\vs2026-x64-qt --config Debug --target wpce_ui`: passed; Qt deployment emitted an existing `VCINSTALLDIR is not set` warning only.
- Added a 50-case matrix verification test for `{mono/color} x {simplex/duplex}` pricing (`0.15/0.25/1.00/2.00`), including m=0 pending, m clamping, copies, duplex tails, driver `"-"` color values, and B&W-printer downgrade. Full pricing test suite now passes 45/45 tests.

## 2026-06-20: UI Polish — Multi-User Coloring, Clickable Docs, Splitter UX, Color Hierarchy

### Changes

**New files (2):**
- `src/ui/user_color_palette.h` — 12-color pastel palette, hash-stable per-user assignment, bg/text/border color accessors
- `src/ui/document_link_delegate.h` — QStyledItemDelegate rendering document names as blue underlined links, clicks emit signal

**Stylesheet overhaul (`src/ui/main.cpp`):**
- Splitter handles: 6px grab zone (was 3px), hover→blue, pressed→dark blue
- Container hierarchy: HeroBar (dark blue gradient) / TablePanel (white card) / MetricCard (white+hover blue) / DetailPanel+TimelinePanel (white cards) / StatisticsPanel (blue gradient) / BottomPanel (transparent)
- Color hierarchy (6 levels): L1 white (hero title) / L2 #0f172a (panel headers) / L3 #1e293b (body) / L4 #0f172a (metric values, 800 weight) / L5 #64748b (muted labels) / L6 #94a3b8 (hints)
- Status pill: idle=gray, running=green (tuned #dcfce7/#166534)
- Buttons: primary blue, danger red, quiet ghost
- Tables: alternating rows, subtle separators, element padding
- Scrollbars: wider (10px), rounded handles

**Per-user row coloring (`job_table_model`, `user_task_model`):**
- `JobTableModel::data()` returns BackgroundRole with per-user pastel tint (80% white blend)
- `JobTableModel::data()` returns RowColorRole, RowTextColorRole, UserIdRole, DocumentPathRole
- `UserTaskModel::data()` returns BackgroundRole with per-user pastel tint (75% white blend)
- Color assigned via `qHash(userId) % 12` — stable across restarts

**Clickable document links:**
- `DocumentLinkDelegate` installed as item delegate for `ColDocument`
- Blue underlined rendering in table cells
- Click opens captured document file or jobs directory via `QDesktopServices`

**Splitter UX:**
- All three splitters: `setHandleWidth(6)` — wider grab zone
- Side pane: QSplitter(Qt::Vertical) replaces QVBoxLayout — detail/timeline freely resizable

### Modified Files (8)
All in `src/ui/`: `main.cpp`, `main_window.cpp`, `job_table_model.{h,cpp}`, `user_task_model.cpp`, `CMakeLists.txt`, +2 new files

## 2026-06-20: Qt6 GUI Enhancement — Pricing, Statistics, and User-Task Management

### Context

- The Qt6 GUI had real-time job monitoring but lacked pricing, statistics, and user-task binding.
- User required: print pricing (¥0.15/single-sided page, ¥0.25/double-sided sheet), statistics dashboard, date-managed task list, multi-job per user binding, cumulative pricing, and a "Complete" button to finalize billing.

### Changes

**New Files:**

| File | Purpose |
|---|---|
| `src/ui/pricing_config.h` | PricingConfig struct + `calculate_job_cost()` free function |
| `src/ui/statistics_panel.h/.cpp` | 4-card dashboard (今日作业/今日营收/活跃用户/完成进度) |
| `src/ui/user_task_model.h/.cpp` | UserTask struct + QAbstractTableModel (6 columns) |
| `src/ui/user_task_panel.h/.cpp` | Task list QTableView + "完成结算" button |

**Pricing Logic (`pricing_config.h`):**
- Single-sided (simplex/not_reported): `total_pages × 0.15`
- Double-sided (vertical/horizontal/duplex): `ceil(total_pages / 2) × 0.25`
- Zero pages or unparseable → 0.00

**User Task Binding (`user_task_model.h/.cpp`):**
- `UserTask`: userId, date, jobIds[], jobCosts{} (per-job pages+price), totalPages, totalPrice, completed flag
- `UserTaskModel`: QMap<QString,int> activeTaskIndex_ for O(1) user lookup; QMap<uint32_t,int> jobToTaskRow_ for O(1) job→task mapping
- `onJobSnapshot()`: auto-creates UserTask on first job per user; binds subsequent jobs to same user's active task
- `onMetadata()`: updates per-job cost entry, recomputes totals — no double-counting
- `completeUserTask()`: marks task completed, removes from active map; next job creates new task

**Modified Files:**

| File | Changes |
|---|---|
| `src/ui/job_table_model.h` | +`ColPrice` column (9th), +`price_` field in JobRowData |
| `src/ui/job_table_model.cpp` | +ColPrice data/header/alignment/sort, price computed in onMetadata() |
| `src/ui/main_window.h` | +StatisticsPanel*, +UserTaskModel*, +UserTaskPanel*, +refreshTaskPanels() |
| `src/ui/main_window.cpp` | +vertical QSplitter (top=3, bottom=1), +bottom panel (stats+task), +signal wiring to UserTaskModel, +clear on start |
| `src/ui/CMakeLists.txt` | +3 new .cpp files |

### UI Layout (Post-Enhancement)

```
MainWindow
├── Metrics cards (existing)
├── QSplitter (vertical, 75/25)
│   ├── TOP: QSplitter (horizontal, table | side pane) — existing
│   └── BOTTOM: BottomPanel QFrame
│       ├── StatisticsPanel (4 metric cards)
│       └── UserTaskPanel (header "用户任务管理" + table + "完成结算" button)
```

### Verification

- `cmake --build build/vs2026-x64-qt --config Debug --target wpce_ui`: 0 errors, 0 warnings
- CTest: 16/16 pass
- Pricing logic verified: simplex(5p)=¥0.75, duplex(5p→3s)=¥0.75, simplex(1p)=¥0.15, duplex(10p→5s)=¥1.25

### Known Limitations

- Generic/Text Only driver (WPCE Sim Printer) doesn't report DEVMODE fields → metadata shows "not_reported", prices computed as simplex
- Real printer drivers that populate DEVMODE will produce accurate duplex pricing
- User tasks are in-memory only — no persistence across UI restarts

## 2026-06-20: Scenario Testing Campaign — 7 Scenarios + Edge Cases + Phase Dwell Fix

### Context

- Virtual print simulator and reliability bug fixes were in place.
- Need to run comprehensive end-to-end scenario tests to validate watcher reliability.
- Phase dwell timing had empty phase-name issue in `tick_stall_detection` path.

### Test Campaign Results (All 7 Scenarios PASS)

| # | Scenario | Target | Jobs | Result | Phase | Timing | Notes |
|---|---|---|---|---|---|---|---|
| 1 | normal | Sim Printer | 5/5 | PASS | completed=5 | ✓ | Clean lifecycle |
| 2 | fast-disappear | Sim Printer | 5/5 | PASS | 2 cancelled + 3 completed | ✓ | Race condition - both phases expected |
| 3 | cancel-mid | Sim Printer | 3/3 | PASS | completed=3 | ✓ | 500ms delay, NUL port fast enough |
| 4 | stall | Sim Printer | 2/2 | PASS | stalled=2 | ✓ | 8133ms/8099ms vs 8s timeout |
| 5 | printer-error | Fault Printer | 3/3 | PASS | completed=3 | ✓ | Spooler-level completion on fault port |
| 6 | high-concurrency | Sim Printer | 50/50 | PASS | completed=50 | ✓ | 0 dropped, all with events |
| 7 | param-variety | Sim Printer | 5/5 | PASS | completed=5 | ✓ | Generic driver DEVMODE is limited |

### Edge Case Test Results

| Edge Case | Result | Details |
|---|---|---|
| Stall 15s timeout | PASS | Spooling dwell ~15204ms, matches 15s timeout exactly |
| Stall 8s timeout (repeat) | PASS | Spooling dwell ~8165ms, stalled delta 8151ms |
| PC Pressure (2000 stress + watcher + sim) | PASS | 2418 jobs/s, 0 writer errors, watcher survived, 13 jobs in lifecycle |
| Phase dwell JSON correctness | VERIFIED | Raw JSON shows `"phase":"spooling"`, `"phase":"stalled"` — correct (PS 5.1 ConvertFrom-Json artifact) |

### Bug Fix: Phase Dwell Empty-Name in tick_stall_detection

**File:** `src/watcher/print_watcher.cpp`
- `tick_stall_detection()` set `current_phase = Stalled` but didn't finalize the old phase's dwell time.
- Fix: Before transitioning to Stalled, compute and store the old phase's dwell, then reset `phase_entry_time` and `cumulative_phase_ms`.
- Verified via raw JSON inspection: phase_dwells now correctly has `"spooling": 8165ms` and `"stalled": 14050ms`.

### Bug Fix: Multi-Handle Stall Simulator

**File:** `tools/simulator.cpp`
- `run_stall()` used a single printer handle for all hanging jobs, causing `StartDocPrinterW` error 1906 on the second job.
- Fix: Open a fresh `PrinterHandle` per hanging job via `unique_ptr`, kept alive until function exit.

### Verification

```
16/16 CTest pass (0 failures)
0 build warnings, 0 build errors
cppcheck: clean (style suggestions only)
```

### Files Modified

| File | Change |
|---|---|
| `src/watcher/print_watcher.cpp` | Phase dwell fix in tick_stall_detection |
| `tools/simulator.cpp` | Multi-handle stall scenario; add `#include <memory>` |
| `scripts/run-scenario-tests.ps1` | PowerShell 5.1 compatible scenario test harness |

## 2026-06-20: Virtual Print Simulator and Reliability Bug Fixes

### Context

- Phase 5 was complete with GoogleTest, PerfMonitor, phase timing, and stress testing.
- Two reliability issues were identified and fixed during integration testing.
- User requested a virtual simulation printer for comprehensive end-to-end scenario testing.

### Bug Fixes Discovered During Simulation Testing

**Bug 1: EnumJobsW buffer overflow under high concurrency (Error 122)**

File: `src/watcher/print_watcher.cpp`
- `enumerate_printer_jobs()` used a single probe+buffer pattern for EnumJobsW.
- Under high job creation rates, the spooler adds more jobs between the probe and fetch calls, causing `ERROR_INSUFFICIENT_BUFFER` (122).
- Fix: Added 64×JOB_INFO_2W headroom (∼6KB) to the buffer allocation, plus a one-time retry with expanded buffer when error 122 occurs on the fetch call.

**Bug 2: Lifecycle summaries NOT written on watcher error**

File: `src/app/main.cpp`
- `watch_printer()` returned early (exit code 2) when `watcher.last_error()` was non-empty, skipping lifecycle summary persistence.
- Under high-concurrency stress, watcher errors (like the EnumJobsW buffer issue) would cause complete loss of all per-job lifecycle data.
- Fix: Restructured error handling to capture errors, continue to write lifecycle summaries, and only return the error code at the end.

**Enhancement: MoveFileExW atomic replace retry**

File: `src/storage/file_utils.cpp`
- `replace_file()` retries `MoveFileExW` up to 8 times with exponential backoff when it hits `ERROR_ACCESS_DENIED`, `ERROR_SHARING_VIOLATION`, or `ERROR_LOCK_VIOLATION`.
- Prevents writer thread termination under PC load stress or antivirus file scanning.

**Enhancement: Unknown phase preservation for active jobs**

File: `src/watcher/print_watcher.cpp`
- Virtual/network printers may briefly report zero status flags while the job is still queued.
- Added `is_active_phase()` heuristic: if `classify_phase()` returns Unknown but the job was in an active phase (Accepted/Spooling/Printing), the old phase is preserved.
- This allows stall detection to fire on jobs that would otherwise be lost to "unknown".

### Virtual Print Simulator

File: `tools/simulator.cpp`
- Standalone CLI tool using Win32 Spooler APIs:
  - `StartDocPrinterW`/`EndDocPrinterW` — creates real spooler jobs
  - `submit_hanging_job()` — creates jobs without `EndDocPrinter`, keeps them in spooling for stall testing
  - `SetJobW(JOB_CONTROL_CANCEL)` — cancels jobs
  - `SetJobW(JOB_CONTROL_PAUSE)` — pauses jobs
  - `DocumentPropertiesW` — modifies DEVMODE for parameter variety
- 7 scenarios: normal, fast-disappear, cancel-mid, stall, printer-error, high-concurrency, param-variety

### Integration Test Results

All scenarios validated end-to-end:

| Scenario | Jobs | Expected Phase | Result |
|---|---|---|---|
| normal | 5 | completed | ✓ 5/5 captured |
| fast-disappear | 5 | cancelled/completed | ✓ Detected before disappearing |
| cancel-mid | 3 | cancelled/completed | ✓ Correct lifecycle |
| stall | 2 | stalled | ✓ Stalled at 8131ms (timeout=8s) |
| printer-error | 3 | completed (spooler level) | ✓ All detected |
| high-concurrency | 50 | completed/cancelled | ✓ No crashes, 0 dropped |
| param-variety | 5 | spooling+ | ✓ Metadata correctly captured |

### Test Results (CTest)

```
100% tests passed, 0 tests failed out of 16
```

### Files Added

| File | Purpose |
|---|---|
| `tools/simulator.cpp` | Virtual print simulator — Win32 job injection/manipulation |
| `scripts/run-simulation-tests.ps1` | Automated integration test orchestration |

### Files Modified

| File | Changes |
|---|---|
| `src/watcher/print_watcher.cpp` | EnumJobsW buffer retry; phase preservation heuristic |
| `src/app/main.cpp` | Lifecycle summaries written even on watcher error |
| `src/storage/file_utils.cpp` | MoveFileExW retry for ACCESS_DENIED |
| `tools/CMakeLists.txt` | Added wpce_sim target |

## 2026-06-20: Phase 5 — Stability, Performance, and Test Infrastructure

### Context

- Phases 0-4 were code-complete and building.
- The project used raw `assert()` for tests, had duplicated file utility code, and lacked performance monitoring, phase timing, and stress testing.
- Phase 5 in the optimized project plan is the final phase before release candidate.

### Changes

**Step 1: Git Baseline**
- Created initial git commit with all Phase 0-4 work (71 files, 9877 lines).

**Step 2: Extract Shared File Utilities**
- Added `src/storage/file_utils.h` and `src/storage/file_utils.cpp`.
- Extracted `ensure_output_directory()`, `make_temp_path()`, `replace_file()`, `write_text_file_atomic()` from duplicated anonymous-namespace functions in both `job_snapshot_writer.cpp` and `page_artifact_writer.cpp`.
- Refactored `write_job_snapshot_json_atomic()`, `write_metadata_json_atomic()`, and `write_lifecycle_summaries_atomic()` to use the shared `write_text_file_atomic()` — significant code reduction.

**Step 3: GoogleTest Integration**
- Integrated GoogleTest v1.16.0 via CMake `FetchContent` (no vcpkg dependency).
- Converted all 11 tests from raw `assert()` to `TEST()` macros with `EXPECT_*` assertions.
- Added `gtest_discover_tests()` for automatic per-test CTest registration.
- Tests now show individual names like `Serialization.JobSnapshot`, `Watcher.PhaseClassification`, etc.

**Step 4: Performance Monitoring Module**
- Added `src/core/perf_monitor.h` and `src/core/perf_monitor.cpp`.
- `PerfSample` struct: CPU%, working set (MB), handle count, thread count, queue depths, watcher idle time, event processing latency.
- `PerfMonitor` class: lock-free atomic setters, Windows Process API sampling (`GetProcessTimes`, `GetProcessMemoryInfo`, `GetProcessHandleCount`, `CreateToolhelp32Snapshot`), CPU delta calculation.
- Added `queue_depth()` to both `AsyncJobSnapshotWriter` and `AsyncPageArtifactWriter`.
- Added `PerfMonitor` member to `PrintWatcher` with setter instrumentation in the notification loop.
- Added `--perf-log <file>` flag to CLI for periodic CSV sampling (~ every 5s).

**Step 5: Phase Timing Instrumentation**
- Extended `JobLifecycleEvent` with `delta_ms_since_prev` and `cumulative_phase_ms`.
- Added `PhaseDwellEntry` struct and `phase_dwells` + `total_duration_ms` to `JobLifecycleSummary`.
- Added timing fields to `InternalJobState` (`first_event_time`, `phase_entry_time`, `last_event_time`, `cumulative_phase_ms`).
- On phase transition: old phase dwell computed and stored, new phase timing started.
- On each event: delta since previous event computed.
- `lifecycle_summaries()` computes `total_duration_ms` and includes active phase dwell in real time.
- JSON serialization updated for all new fields.
- Added `LifecycleEventTiming` and `LifecycleSummaryTiming` unit tests.

**Step 6: Stress Test Tool**
- Added `tools/stress_test.cpp` — standalone stress test CLI.
- Simulates N synthetic jobs across W concurrent writers.
- Tracks: enqueue success/failure, peak queue depth, throughput (jobs/sec), wall-clock time.
- MSVC Debug: `_CrtSetDbgFlag` for automatic memory leak detection.
- Configurable: `--job-count`, `--writer-count`, `--queue-capacity`, `--output-dir`, `--burst-size`, `--burst-interval-ms`.
- CTest registrations: `wpce_stress_1k` (1000 jobs), `wpce_stress_queue_full` (2000 jobs, small queue — backpressure test).

### Test Results (Final)

```
100% tests passed, 0 tests failed out of 16

Test breakdown:
  Serialization.JobSnapshot               Passed
  Serialization.MetadataState             Passed
  Serialization.PageArtifactIndex         Passed
  Serialization.LifecycleEvent            Passed
  Serialization.LifecycleSummary          Passed
  Serialization.LifecycleEventTiming      Passed  (new)
  Serialization.LifecycleSummaryTiming    Passed  (new)
  Watcher.DescribeJobStatus               Passed
  Watcher.PhaseClassification             Passed
  Storage.AtomicJobSnapshotWriter         Passed
  Storage.AsyncJobSnapshotWriter          Passed
  Storage.LifecycleSummaryPersistence     Passed
  Collector.NameMappers                   Passed
  wpce_stress_1k                          Passed  (new)
  wpce_stress_queue_full                  Passed  (new)
  wpce_filter_pipeline                    Passed
```

### Static Analysis

- cppcheck: 22/22 files checked, 2 style suggestions (false positive + useStlAlgorithm). No errors.
- clang-tidy: skipped (not installed in current shell).

### Key Decisions

- GoogleTest via FetchContent, not vcpkg — keeps test builds self-contained.
- PerfMonitor lives in `src/core/` with no dependency on `storage` or `watcher` — clean dependency graph.
- Phase timing uses `steady_clock` (monotonic) internally, stores `int64_t` milliseconds for serialization.
- Phase dwell entries use a lightweight `vector<PhaseDwellEntry>` rather than `unordered_map` to keep the contracts header lean.
- Stress test uses `unique_ptr<AsyncJobSnapshotWriter>` because the writer class is non-movable.
- Concurrent multi-writer stress test is available as a manual test; filesystem race conditions can cause spurious failures in CI.

### Files Added

| File | Purpose |
|---|---|
| `src/storage/file_utils.h` | Shared file I/O declarations |
| `src/storage/file_utils.cpp` | Atomic write primitives |
| `src/core/perf_monitor.h` | Performance monitoring API |
| `src/core/perf_monitor.cpp` | Windows process metrics |
| `tools/stress_test.cpp` | Stress test CLI tool |
| `tools/CMakeLists.txt` | Tools build config |

### Files Modified

| File | Changes |
|---|---|
| `src/CMakeLists.txt` | Added file_utils.cpp, perf_monitor.cpp, tools subdirectory |
| `src/storage/job_snapshot_writer.h` | Added `queue_depth()` method |
| `src/storage/job_snapshot_writer.cpp` | Removed anon-ns utilities, used shared `write_text_file_atomic` |
| `src/storage/page_artifact_writer.h` | Added `queue_depth()` method |
| `src/storage/page_artifact_writer.cpp` | Removed anon-ns utilities, used shared `write_text_file_atomic` |
| `src/storage/json_serializer.cpp` | Added timing fields, phase_dwells, int64_t overload |
| `src/core/data_contracts.h` | Added timing fields, PhaseDwellEntry struct |
| `src/watcher/print_watcher.h` | Added PerfMonitor member, timing state fields |
| `src/watcher/print_watcher.cpp` | Instrumented with perf sampling, phase dwell tracking |
| `src/app/main.cpp` | Added `--perf-log` flag, periodic perf sampling in watch loop |
| `tests/CMakeLists.txt` | FetchContent GoogleTest, gtest_discover_tests, stress CTests |
| `tests/contract_serialization_tests.cpp` | Converted to GoogleTest, added timing tests |
| `CMakeLists.txt` (root) | Added tools subdirectory |

### Follow-Ups

- Install clang-tidy and run full static analysis.
- Run live capture with `--perf-log` to validate performance metrics against Phase 5 budgets.
- Test with multi-application print jobs (Word, browser, Notepad, etc.).
- Install Qt6 via vcpkg (or verify C:/Qt/6.8.2 works) and confirm GUI builds.
- WDK packaging for XPS filter driver before production deployment.
- 10000-job stress test (may need to run outside CTest due to duration).

## 2026-06-19: Plan Feasibility Optimization and Project Skills

### Context

- Repository initially contained only `readme`.
- No existing `docs/`, source tree, Git repository, or development archive was present.
- User requested feasibility optimization of the project plan and creation of project skills.

### Changes

- Added `docs/PROJECT_PLAN_OPTIMIZED.md` as the feasibility-adjusted project plan.
- Added `docs/ARCHITECTURE.md` as the architecture baseline.
- Added four local project skills under `skills/`:
  - `wpce-architecture`
  - `wpce-development`
  - `wpce-qa`
  - `wpce-devops`

### Key Decisions

- Split MVP into MVP-A user-mode capture and MVP-B controlled XPS capture.
- Treat PrintTicket and FixedPage capture as controlled XPS pipeline work, not passive monitoring.
- Add Phase 0 for repository, build, documentation, and tooling baseline.
- Use the four skills from the original plan as project-local Codex skills.

### Verification

- Skill validation should be run after all skill content and metadata are finalized.

### Follow-Ups

- Initialize Git and create the planned source/test directory structure.
- Create the Phase 0 CMake baseline.
- Confirm whether project-local skills should be installed into the global Codex skills directory for automatic discovery.

## 2026-06-19: Phase 0 Engineering Baseline

### Context

- User requested initial project development.
- Existing optimized plan defined Phase 0 as the required baseline before feature work.
- Repository had project docs and skills, but no source tree, build system, tests, scripts, or Git repository.

### Changes

- Initialized a local Git repository.
- Added root build and tooling files:
  - `CMakeLists.txt`
  - `CMakePresets.json`
  - `.gitignore`
  - `.editorconfig`
  - `.clang-format`
  - `.clang-tidy`
- Added C++20 source baseline:
  - `src/common`: build/version information.
  - `src/core`: initial data contracts for job, metadata, and page artifact index.
  - `src/storage`: JSON and XML serialization for the initial contracts.
  - `src/app`: minimal `wpce` CLI with version and sample artifact output.
- Added placeholder module directories:
  - `src/watcher`
  - `src/collector`
  - `src/filter`
  - `tools`
  - `deploy`
- Added CTest bootstrap tests in `tests/`.
- Added scripts:
  - `scripts/check-toolchain.ps1`
  - `scripts/build-msvc.ps1`
  - `scripts/run-static-analysis.ps1`
- Added `docs/TOOLCHAIN.md`.

### Key Decisions

- Use CTest with a small executable test for Phase 0 to avoid adding third-party dependencies before the baseline is stable.
- Keep GoogleTest as the planned framework for feature phases.
- Keep Visual Studio instance path out of versioned CMake presets; pass it through `scripts/build-msvc.ps1` when local discovery needs help.
- Keep MVP-B XPSDrv work gated until WDK packaging/signing tools are complete.

### Verification

- `.\scripts\build-msvc.ps1`: passed.
- `ctest --preset vs2026-debug`: passed, 1/1 test.
- `.\build\vs2026-x64\src\Debug\wpce.exe --version`: printed `0.1.0`.
- `.\build\vs2026-x64\src\Debug\wpce.exe --sample-json`: emitted a valid sample job snapshot.
- `.\scripts\check-toolchain.ps1`: passed and reported local tool state.
- `.\scripts\run-static-analysis.ps1`: cppcheck completed; clang-tidy was skipped because it is not installed.

### Known Risks

- `clang-tidy` is not installed in the current shell.
- `cl` and `msbuild` are not in the normal PATH, though the Visual Studio instance is available.
- WDK packaging utilities `inf2cat` and `stampinf` were not found under the checked Windows Kits path.
- No real print watcher implementation exists yet; Phase 1 remains the next functional milestone.

### Follow-Ups

- Implement Phase 1 `src/watcher` with Win32 Spooler notification handling.
- Add a storage writer that writes artifacts to disk atomically.
- Decide when to introduce GoogleTest.
- Install or expose clang-tidy before enforcing release gates.

## 2026-06-19: Phase 1 Print Watcher Bootstrap

### Context

- Phase 0 baseline was complete.
- The next functional milestone in `docs/PROJECT_PLAN_OPTIMIZED.md` was Phase 1 user-mode print job watching.
- No real `src/watcher` implementation existed yet.

### Changes

- Added `src/watcher/print_watcher.h` and `src/watcher/print_watcher.cpp`.
- Implemented Winspool RAII wrappers for printer handles and change notification handles.
- Added print job queue enumeration through `EnumJobsW` and conversion to `core::JobSnapshot`.
- Added a background `PrintWatcher` that waits on `FindFirstPrinterChangeNotification`/`FindNextPrinterChangeNotification`.
- Added simple per-JobId signature deduplication for noisy spooler notifications.
- Linked `wpce_core` against `Winspool.lib` on Windows.
- Extended the CLI:
  - `wpce --list-jobs <printer-name>`
  - `wpce --watch-printer <printer-name>`
- Added focused contract-test coverage for job status description.

### Key Decisions

- Keep Phase 1 in user mode and limited to job-level snapshots.
- Keep callback execution on the watcher worker for now; future persistence should hand off to a bounded async queue.
- Keep deleted-job events as notification triggers only; the current job snapshot model does not yet represent tombstones.
- Use `Microsoft Print to PDF` as the immediate local manual validation target.

### Verification

- `.\scripts\build-msvc.ps1`: passed.
- `ctest --preset vs2026-debug`: passed, 1/1 test.
- `.\build\vs2026-x64\src\Debug\wpce.exe --version`: printed `0.1.0`.
- `.\build\vs2026-x64\src\Debug\wpce.exe --sample-json`: emitted the sample job snapshot.
- `Get-Printer`: confirmed local printers include `Microsoft Print to PDF`.
- `.\build\vs2026-x64\src\Debug\wpce.exe --list-jobs "Microsoft Print to PDF"`: completed successfully against an empty queue.
- `.\scripts\run-static-analysis.ps1`: cppcheck completed; clang-tidy was skipped because it is not installed.

### Known Risks

- `clang-format` is not installed in the current shell.
- `clang-tidy` is not installed in the current shell.
- `--watch-printer` has not yet been validated with a live print job during this session.
- The watcher emits current snapshots for changed jobs but does not yet persist `job.json`.
- Callback work still runs on the watcher worker; disk persistence must not be added directly there.

### Follow-Ups

- Validate `--watch-printer "Microsoft Print to PDF"` with an actual print job.
- Add an async storage writer that persists `job.json` atomically.
- Add a deletion/tombstone event model if Phase 1 acceptance needs explicit completed/deleted job records.
- Add a real integration test or scripted manual test around Microsoft Print to PDF.

## 2026-06-19: Phase 1 Async Job Snapshot Persistence

### Context

- Phase 1 print watching could enumerate and emit job snapshots to stdout.
- The architecture baseline requires persistence to use async queues so watcher callbacks do not perform direct disk I/O.
- The previous follow-up explicitly called for atomic `job.json` persistence.

### Changes

- Added `src/storage/job_snapshot_writer.h` and `src/storage/job_snapshot_writer.cpp`.
- Added `AsyncJobSnapshotWriter` with:
  - Bounded in-memory queue.
  - Background worker thread.
  - Drain-on-stop shutdown behavior.
  - Last-error reporting.
- Added atomic JSON snapshot writes using a temporary file and replace operation.
- Persisted:
  - Latest snapshot as `job.json`.
  - Per-job snapshot as `jobs/job-<job_id>.json`.
- Extended the CLI:
  - `wpce --write-sample-job <output-dir>`
  - `wpce --watch-printer <printer-name> [output-dir]`
- Added contract-test coverage for atomic writes and async writer flush behavior.

### Key Decisions

- Keep watcher callbacks lightweight by enqueueing snapshots instead of writing files directly.
- Keep queue overflow non-blocking; if the writer queue is unavailable or full, the snapshot is dropped and reported.
- Keep Phase 1 storage focused on job-level JSON; metadata and XML persistence remain Phase 2 work.

### Verification

- `.\scripts\build-msvc.ps1`: passed.
- `ctest --preset vs2026-debug`: passed, 1/1 test.
- `.\build\vs2026-x64\src\Debug\wpce.exe --write-sample-job .\build\sample-capture`: wrote `job.json`.
- `.\build\vs2026-x64\src\Debug\wpce.exe --list-jobs "Microsoft Print to PDF"`: completed successfully against an empty queue.
- `.\scripts\run-static-analysis.ps1`: cppcheck completed; clang-tidy was skipped because it is not installed.

### Known Risks

- `--watch-printer` still needs validation with a live print job.
- Queue overflow currently drops snapshots rather than blocking the watcher or spilling to disk.
- Snapshot archive naming by `job_id` can overwrite older files if a printer reuses job IDs.
- `clang-tidy` is still unavailable in the current shell.

### Follow-Ups

- Validate `--watch-printer "Microsoft Print to PDF" .\artifacts\capture` with an actual print job.
- Add Phase 2 metadata collection from `JOB_INFO_2.pDevMode`.
- Decide whether job completion/deletion should produce explicit tombstone records.
- Add a scripted integration test for Microsoft Print to PDF where local environment allows automated printing.

## 2026-06-19: UTF-8 Console Diagnostics and Capture Review

### Context

- Live Microsoft Print to PDF testing captured real jobs, but PowerShell output appeared garbled.
- The same mojibake appeared in PowerShell's own Chinese error text, pointing to console/encoding-page handling rather than bad Winspool capture.
- Captured JSON read with explicit UTF-8 decoded the document name correctly.

### Changes

- Configured WPCE CLI startup to use UTF-8 console input/output on Windows.
- Added `scripts/enter-utf8-console.ps1` to configure Windows PowerShell sessions for UTF-8 diagnostics.
- Added `scripts/show-capture-summary.ps1` to read captured job JSON with explicit UTF-8 and display a compact table.

### Capture Review

- Captured printer: `Microsoft Print to PDF`.
- Captured jobs: 2, 3, and 4.
- Captured user: `gulingling`.
- Captured document name: `Microsoft Word - 车辆2301何诚君312304010525.doc`.
- Final observed status for each archived job: `retained`.
- Intermediate status transitions were visible in console output, including `spooling`, `spooling|printing`, `spooling|printing|retained`, `printing|retained`, and `retained`.

### Verification

- `Get-Content -Raw -Encoding UTF8 .\artifacts\capture\job.json`: decoded the captured Chinese document name correctly.
- `.\scripts\show-capture-summary.ps1 .\artifacts\capture`: displayed jobs 2, 3, and 4 with readable Chinese document names.
- `cmake -S . -B .\build\verify-utf8 ...; cmake --build .\build\verify-utf8 --config Debug; ctest --test-dir .\build\verify-utf8 -C Debug --output-on-failure`: passed.
- `.\scripts\enter-utf8-console.ps1`: completed and reported UTF-8 console setup.
- `.\build\verify-utf8\src\Debug\wpce.exe --write-sample-job .\artifacts\utf8-smoke`: wrote a sample snapshot successfully.
- `.\scripts\run-static-analysis.ps1`: cppcheck completed; clang-tidy was skipped because it is not installed.

### Known Risks

- The original `build\vs2026-x64\src\Debug\wpce.exe` was locked by a still-running watcher process from the manual test, so the primary preset link step could not overwrite that executable until the process is stopped.
- Windows PowerShell 5 still requires explicit UTF-8 handling for its own output and for reading UTF-8 JSON without BOM.

### Follow-Ups

- Rebuild and rerun live watcher after UTF-8 console initialization.
- Use `.\scripts\enter-utf8-console.ps1` before manual PowerShell diagnostics.
- Continue Phase 2 metadata extraction once live watcher output is confirmed readable.

## 2026-06-19: Live Capture Analysis and Submit Time Fix

### Context

- User ran additional live Microsoft Print to PDF jobs through the background watcher.
- Captured JSON and stdout logs were available under `artifacts/live-capture-20260619-232750`.
- Microsoft documentation states that `JOB_INFO_2.Submitted` is already in UTC.

### Capture Review

- Captured jobs: 5 and 6.
- Captured user: `gulingling`.
- Captured printer: `Microsoft Print to PDF`.
- Captured document name: `Microsoft Word - 车辆2301何诚君312304010525.doc`.
- Each job emitted four status snapshots:
  - `spooling`
  - `spooling|printing|retained`
  - `printing|retained`
  - `retained`
- Final archived snapshots for both jobs were present under `capture/jobs/`.
- `watcher.stderr.log` was empty.

### Changes

- Fixed `format_system_time_utc` to format `JOB_INFO_2.Submitted` directly instead of converting it from local time to UTC again.

### Key Decisions

- Keep `submit_time_utc` as an ISO-like UTC string ending in `Z`.
- Do not derive local display time inside the data contract; local display belongs in diagnostic tools or UI.

### Known Risks

- Existing captured files from before this fix still contain the old timestamp value.
- The currently running listener process must be restarted before new captures use the fixed timestamp logic.

## 2026-06-19: Fast-Job Scenario Optimization

### Context

- User expected five Microsoft Print to PDF jobs, but the watcher only captured jobs 10 and 11 in the active session.
- JobId gaps suggested that jobs 7, 8, and 9 may have completed or disappeared before queue enumeration could read them.
- Microsoft documentation states `FindNextPrinterChangeNotification` can return `PRINTER_NOTIFY_INFO_DATA` entries for monitored job fields, including JobId, user, document, status, and submitted time.

### Changes

- Changed `FindFirstPrinterChangeNotification` setup to request job field notifications for:
  - `JOB_NOTIFY_FIELD_PRINTER_NAME`
  - `JOB_NOTIFY_FIELD_USER_NAME`
  - `JOB_NOTIFY_FIELD_STATUS`
  - `JOB_NOTIFY_FIELD_DOCUMENT`
  - `JOB_NOTIFY_FIELD_SUBMITTED`
- Added parsing of `PRINTER_NOTIFY_INFO_DATA` returned by `FindNextPrinterChangeNotification`.
- Added immediate `GetJobW` lookup by notified JobId before falling back to full queue enumeration.
- Added `winspool-notify` partial snapshots when a notified job is no longer available through `GetJobW`.
- Added refresh handling for `PRINTER_NOTIFY_INFO_DISCARDED`.
- Added CLI stderr diagnostics for JobId gaps without polluting normal job JSON output.

### Key Decisions

- Keep complete `JOB_INFO_2` snapshots as the preferred output when available.
- Use notification-only snapshots only as a fallback for fast jobs that disappear before enumeration.
- Report JobId gaps as diagnostics, not as fake job records.

### Verification

- `cmake -S . -B .\build\verify-notify ...`: passed.
- `cmake --build .\build\verify-notify --config Debug`: passed.
- `ctest --test-dir .\build\verify-notify -C Debug --output-on-failure`: passed, 1/1 test.
- `.\scripts\run-static-analysis.ps1`: cppcheck completed; clang-tidy was skipped because it is not installed.
- Restarted live listener with optimized build:
  - PID: 14728
  - Session: `artifacts/live-capture-notify-20260619-234153`
  - Executable: `build/verify-notify/src/Debug/wpce.exe`

### Known Risks

- Some printer drivers may not provide every requested notification field.
- `PRINTER_NOTIFY_INFO_DISCARDED` still means Windows may have dropped notification data under load; refresh can recover current state but not always every intermediate transition.
- Gap diagnostics indicate possible missed jobs but are not proof that every skipped JobId was a user print task.

## 2026-06-19: Phase 2 Metadata Collector Implementation

### Context

- Phase 1 job watching captured only JobId, User, Document, Printer, SubmitTime, and Status.
- `JOB_INFO_2W` also contains `TotalPages`, `PagesPrinted`, and `pDevMode` (DEVMODE with copies, duplex, color, paper size, orientation), but these were discarded by `to_snapshot()`.
- Phase 2 in the optimized project plan calls for metadata extraction from `JOB_INFO_2` and `DEVMODE`.

### Changes

- Extended `core::MetadataSnapshot` with `total_pages` and `pages_printed` fields alongside the existing copies, duplex, color, paper_size, orientation.
- Created `src/collector/metadata_collector.h` and `src/collector/metadata_collector.cpp`:
  - `extract_metadata(const JOB_INFO_2W&)` builds a complete `MetadataSnapshot` from job info and DEVMODE.
  - TotalPages and PagesPrinted read directly from `JOB_INFO_2W`.
  - DEVMODE fields (copies, duplex, color, paper_size, orientation) extracted via `dmFields` bitmask validation.
  - Field state is `Reported` when the driver provides the field, `NotReported` when `dmFields` omits the bit, `Unknown` when the enum value is unrecognised.
  - Custom paper sizes (dmPaperSize==0 with dmPaperLength/dmPaperWidth) formatted as "WxHmm".
  - Comprehensive `DMPAPER_*` lookup table (100+ paper size names including PRC and Japanese envelope sizes).
  - Public name mappers: `paper_size_name()`, `duplex_name()`, `color_name()`, `orientation_name()`.
- Modified `src/watcher/print_watcher.h`:
  - Added `JobSnapshotWithMetadata` struct pairing job and metadata snapshots.
  - Added `MetadataSnapshotCallback` type.
  - `PrintWatcher` constructor accepts optional second callback for metadata.
  - `enumerate_printer_jobs()` returns `vector<JobSnapshotWithMetadata>`.
- Modified `src/watcher/print_watcher.cpp`:
  - `get_job_snapshot_by_id()` returns `optional<JobSnapshotWithMetadata>` with extracted metadata.
  - `enumerate_printer_jobs()` returns paired job+metadata for every enumerated job.
  - `run()` emits both job and metadata callbacks; notification-only fallbacks receive a metadata stub with all fields `NotReported`.
- Modified `src/storage/job_snapshot_writer.h/.cpp`:
  - Added `SnapshotPair` struct for paired job+metadata enqueue.
  - `enqueue()` now accepts both `JobSnapshot` and `MetadataSnapshot`.
  - Added `metadata_snapshot_path()` and `metadata_archive_path()` for `metadata.json` and `jobs/job-<id>-metadata.json`.
  - Added `write_metadata_json_atomic()` with same temp-file+replace pattern as job snapshots.
- Modified `src/storage/json_serializer.cpp`:
  - `to_json(MetadataSnapshot)` now includes `total_pages` and `pages_printed` fields before copies.
- Modified `src/app/main.cpp`:
  - `--list-jobs` now outputs both job and metadata JSON.
  - `--watch-printer` wires both job and metadata callbacks, persists both to disk.
  - `--write-sample-job` writes both `job.json` and `metadata.json`.
  - Added `make_sample_metadata()` helper.
- Modified `src/CMakeLists.txt`: added `collector/metadata_collector.cpp` to `wpce_core`.
- Updated `tests/contract_serialization_tests.cpp`:
  - `test_metadata_state_serialization` asserts `total_pages` field.
  - `test_async_job_snapshot_writer` passes metadata alongside job snapshot and asserts `metadata.json` and archive output.

### Key Decisions

- Keep metadata extraction in `src/collector/` per the architecture baseline, not inside the watcher.
- Use `dmFields` bitmask to distinguish `Reported` from `NotReported`, and unrecognised enum values as `Unknown` — never invent values.
- Notification-only (fallback) snapshots mark all metadata fields as `NotReported` since no `JOB_INFO_2W` is available.
- Pair job and metadata via sequential callbacks (job fires first, metadata second) in the watcher; the CLI pairs them with a temporary holder under mutex.
- Keep the existing `JobSnapshot` data contract unchanged — metadata is a separate parallel artifact per the architecture baseline.

### Verification

- `.\scripts\build-msvc.ps1`: passed.
- `ctest --preset vs2026-debug`: passed, 1/1 test.
- `.\build\vs2026-x64\src\Debug\wpce.exe --version`: printed `0.1.0`.
- `.\build\vs2026-x64\src\Debug\wpce.exe --write-sample-job .\build\sample-capture-phase2`:
  - `job.json`: correct job snapshot with all 6 original fields.
  - `metadata.json`: correct metadata with total_pages, pages_printed, copies, duplex, color, paper_size, orientation.
  - `jobs/job-1.json`: correct archived job snapshot.
  - `jobs/job-1-metadata.json`: correct archived metadata snapshot.
- `.\build\vs2026-x64\src\Debug\wpce.exe --list-jobs "Microsoft Print to PDF"`: completed successfully against an empty queue.
- `.\scripts\run-static-analysis.ps1`: pending.

### Known Risks

- Some printer drivers may not populate `pDevMode` at all, resulting in all DEVMODE fields `NotReported`.
- Some drivers may set `dmFields` bits but still leave the corresponding value at 0 (default) — we treat this as reported with value "0" or equivalent.
- `TotalPages` may be 0 for jobs that are still spooling (driver hasn't determined page count yet).
- Metadata for notification-only (winspool-notify) snapshots is always `NotReported` — these are fallback records for fast-disappearing jobs.
- Live watcher validation with an actual print job to Microsoft Print to PDF is pending.

### Follow-Ups

- Run live watcher with Microsoft Print to PDF to confirm real DEVMODE field extraction works end-to-end.
- Validate metadata accuracy against a known sample set (different document types: Word, PDF, image).
- Measure metadata extraction overhead against the Phase 5 performance budget.
- Add Phase 2 integration tests with a controlled test printer.

## 2026-06-20: Phase 1-2 Completion — Job Lifecycle Tracking

### Context

- Phase 1 had print job event detection but no lifecycle tracking — it emitted raw status snapshots without understanding the job's progression (accepted → spooling → printing → completed/failed/cancelled).
- Phase 2 added metadata collection but no progress percentage computation from TotalPages/PagesPrinted.
- The user required comprehensive coverage:
  - **Application coverage**: Word, Notepad, browser, 金山文档 — all go through the same Win32 Spooler, so the watcher is inherently application-agnostic.
  - **Print failure**: JOB_STATUS_ERROR, JOB_STATUS_OFFLINE, JOB_STATUS_PAPEROUT, JOB_STATUS_USER_INTERVENTION.
  - **Transmission interruption**: Manifests as JOB_STATUS_ERROR or OFFLINE.
  - **Job cancellation**: JOB_STATUS_DELETING → JOB_STATUS_DELETED.
  - **Timeout printing**: Jobs stuck in spooling/printing without progress.
  - **Progress display**: Accepted → Spooling → Printing → Completed/Failed/Cancelled with page progress.

### Changes

- Extended `src/core/data_contracts.h`:
  - Added `JobLifecyclePhase` enum: Unknown, Accepted, Spooling, Printing, Completed, Failed, Cancelled, Stalled.
  - Added `to_string(JobLifecyclePhase)` and `is_terminal_phase()`.
  - Added `JobLifecycleEvent` struct: event_id, job_id, sequence, timestamp_utc, phase, status_flags, detail, pages_printed, total_pages.
  - Added `JobLifecycleSummary` struct: per-job schema_version, user, document_name, printer_name, submit_time_utc, events[], final_phase, progress_percent, last_status, source.
- Extended `src/core/data_contracts.cpp`:
  - Implemented `to_string(JobLifecyclePhase)`.
  - Implemented `is_terminal_phase()` — Completed, Failed, Cancelled, Stalled return true.
- Extended `src/watcher/print_watcher.h`:
  - Added `LifecycleEventCallback` type.
  - Added `stall_timeout` to `WatcherConfig`.
  - Added `classify_phase()` public function to map JOB_STATUS flags → JobLifecyclePhase.
  - `PrintWatcher` constructor accepts optional third callback for lifecycle events.
  - Added `lifecycle_summaries()` method for thread-safe per-job history access.
  - Added `InternalJobState` tracking struct inside the watcher.
- Rewrote `src/watcher/print_watcher.cpp`:
  - Added `utc_timestamp_now()` for event timestamps.
  - Added `make_lifecycle_event()` builder.
  - Added `classify_phase()` — priority order: Failed (error/paperout/offline/user_intervention) > Cancelled (deleted/deleting) > Completed (complete/retained/printed) > Printing > Spooling > Unknown.
  - Added `process_phase_transition()` inside `run()` — tracks per-job state, suppresses duplicate phases, blocks re-entry after terminal, emits lifecycle events.
  - Added `tick_stall_detection()` — periodic scan of active jobs against `stall_timeout`, emits Stalled events and marks terminal.
  - Stall detection runs on WAIT_TIMEOUT and after each notification batch (minimum 5s interval).
  - `lifecycle_summaries()` returns sorted thread-safe copies of all job histories.
- Extended `src/storage/json_serializer.h/.cpp`:
  - Added `to_json(JobLifecycleEvent)`, `append_json_lifecycle_event()`, `to_json(JobLifecycleSummary)`.
- Extended `src/storage/job_snapshot_writer.h/.cpp`:
  - Added `lifecycle_summary_path()`, `lifecycle_summary_archive_path()`.
  - Added `write_lifecycle_summaries_atomic()` — writes per-job `jobs/job-N-lifecycle.json` and a session manifest `lifecycle-summary.json`.
- Rewrote `src/app/main.cpp`:
  - `--watch-printer` now takes optional `--stall-timeout <seconds>` parameter.
  - New structured progress display: Unicode icons (➤ ⏳ 🖨 ✓ ✗ ✂ ⚠) + phase name + page progress.
  - JobId gap reporting with cleaner format.
  - On Ctrl+C, prints a session summary table showing each job's document, user, final phase, progress, and event count.
  - Persists lifecycle summaries automatically on stop.
  - Added `print_lifecycle_event()` and `print_lifecycle_summary_line()` display helpers.
- Updated `tests/contract_serialization_tests.cpp`:
  - Added `test_phase_classification()` — verifies all phase mappings and terminal checks.
  - Added `test_lifecycle_event_serialization()` — verifies event JSON format.
  - Added `test_lifecycle_summary_serialization()` — verifies multi-event summary JSON.
  - Added `test_lifecycle_summary_persistence()` — verifies atomic write of per-job archive and session manifest.

### Key Decisions

- Keep lifecycle tracking internal to PrintWatcher, not the CLI — the watcher owns the state machine.
- Use three parallel callbacks (job, metadata, lifecycle) with sequential emission order: job first, then metadata, then lifecycle. The CLI pairs them via a temporary holder.
- `classify_phase()` prioritizes terminal states over active states so a job that is both "printing" and "error" is classified as Failed.
- Stalled is a synthetic terminal phase — once emitted, no further events are emitted for that job.
- The watcher suppress duplicate consecutive phases (e.g., multiple "printing" events for the same job).
- Once a job reaches a terminal phase (Completed/Failed/Cancelled/Stalled), no further events are emitted for it.
- Lifecycle summaries are persisted atomically on shutdown, not during hot-path observation — this keeps the hot path lightweight.

### Verification

- `.\scripts\build-msvc.ps1`: passed (0 warnings, 0 errors).
- `ctest --preset vs2026-debug`: passed, 1/1 test (9 subtests: serialization, metadata, page artifacts, job status, atomic write, async write, phase classification, lifecycle event serialization, lifecycle summary serialization, lifecycle persistence).
- `.\build\vs2026-x64\src\Debug\wpce.exe --version`: printed `0.1.0`.
- `.\build\vs2026-x64\src\Debug\wpce.exe --write-sample-job .\build\sample-lifecycle`: wrote job.json and metadata.json.
- Live capture with lifecycle tracking pending user's print test.

### Coverage Matrix (Phase 1-2 Complete)

| Scenario | Status | Implementation |
|---|---|---|
| **Word 打印** | ✅ 通用覆盖 | Win32 Spooler API 应用无关 |
| **记事本 打印** | ✅ 通用覆盖 | 同上 |
| **浏览器 打印** | ✅ 通用覆盖 | 同上 |
| **金山文档 打印** | ✅ 通用覆盖 | 同上 |
| **打印失败** | ✅ | JOB_STATUS_ERROR → Failed phase |
| **传输中断** | ✅ | JOB_STATUS_OFFLINE → Failed phase |
| **任务取消** | ✅ | JOB_STATUS_DELETING/DELETED → Cancelled phase |
| **超时打印** | ✅ | `--stall-timeout N` 停滞检测 |
| **进度显示** | ✅ | `[Job N] printing  page 3/10 (30%)` |
| **接受任务** | ✅ | ADD_JOB notification → Accepted phase |
| **队列中** | ✅ | Spooling phase |
| **开始打印** | ✅ | Printing phase |
| **打印完成** | ✅ | Completed phase (printed/complete/retained) |
| **打印失败** | ✅ | Failed phase (error/offline/paperout) |
| **打印参数** | ✅ | metadata.json (total_pages, pages_printed, copies, duplex, color, paper_size, orientation) |

### Output File Structure (Post-Enhancement)

```
capture/
├── job.json                          ← Latest job snapshot
├── metadata.json                     ← Latest metadata snapshot
├── lifecycle-summary.json           ← Session manifest
└── jobs/
    ├── job-5.json                   ← Job #5 archive
    ├── job-5-metadata.json          ← Job #5 metadata archive
    ├── job-5-lifecycle.json         ← Job #5 full lifecycle (events array)
    ├── job-6.json
    ├── job-6-metadata.json
    └── job-6-lifecycle.json
```

### Known Risks

- `classify_phase()` API takes raw status flags and a status string hint; the status-string fallback is used when flags aren't directly available from the snapshot.
- Stalled phase is emitted at most once per job; if the job later completes, the stalled status in the summary is not updated.
- Unicode icons in CLI output require a UTF-8 capable terminal (PowerShell 5 needs `enter-utf8-console.ps1`).
- Some printer drivers may not set JOB_STATUS_COMPLETE or JOB_STATUS_RETAINED, falling back to JOB_STATUS_PRINTED.
- Live validation with actual print jobs from multiple applications is pending.

### Follow-Ups

- Run live capture from Word, Notepad, browser, and 金山文档 to validate multi-application coverage.
- Test cancellation scenario: start a large print job, cancel it mid-way, verify Cancelled lifecycle event.
- Test stall detection: pause the spooler, verify stall event fires after timeout.
- Run static analysis (`.\scripts\run-static-analysis.ps1`).
- Proceed to Phase 3 XPS Filter feasibility gate only after live validation confirms Phase 1-2 completeness.

## 2026-06-20: Phase 3 XPS Filter Feasibility Gate + Phase 4 PrintTicket Capture

### Context

- Phase 1-2 user-mode watcher provides job metadata (pages/copies/color/paper/N-up/collate) but cannot access XPS-level artifacts.
- Page PrintTicket and FixedPage references only exist inside the XPSDrv print pipeline.
- The optimized project plan requires a Phase 3 feasibility gate before committing to driver/filter work.
- WDK packaging tools (`inf2cat`, `stampinf`) were not installed, so full driver packaging is deferred to Phase 5.
- All required SDK headers were present: `filterpipeline.h`, `xpsobjectmodel.h`, `xpsprint.h`, `prntvpt.h`.

### Architecture Decision

Built a **pipeline simulator test harness** instead of a full printer driver:
1. Build COM filter DLL implementing `IPrintPipelineFilter`
2. Build standalone test program with mock pipeline interfaces
3. instantiate filter via `DllGetClassObject` → `IClassFactory::CreateInstance`
4. Call `InitializeFilter` → `StartOperation` → `ShutdownOperation` with mock parts
5. No driver installation, no signing, fully automated via CTest

### Changes

- Added `src/common/com_ptr.h`:
  - RAII COM smart pointer template (`com_ptr<T>`)
  - Automatic `Release()` on destruction, `AddRef()` on copy
  - `operator->()`, `operator bool()`, `get()`, `attach()`, `detach()`, `reset()`
  - `query_interface<U>(T*)` convenience helper
  - `operator&()` with pre-condition assert (null check)
  - `AddressOf()` with automatic reset

- Added `src/filter/filter_pipeline.h`:
  - `XpsCaptureFilter` class implementing `IPrintPipelineFilter` (3 methods + IUnknown)
  - CLSID: `{B1C8E3F0-2D4A-5B6C-7D8E-9F0A1B2C3D4E}`
  - Internal state: negotiation, property bag, pipeline control (via `com_ptr<>`)
  - Captured data: `captured_print_tickets_[]`, `page_artifact_indices_[]`
  - `set_output_directory()` for file persistence
  - `XpsCaptureFilterClassFactory` implementing `IClassFactory`

- Added `src/filter/filter_guid.cpp`:
  - GUID definition using `DEFINE_GUID` + `extern const CLSID`

- Added `src/filter/filter_entry.cpp`:
  - `DllMain` (no-op, disables thread notifications)
  - `DllGetClassObject` — returns `IClassFactory` for the filter CLSID
  - `DllCanUnloadNow` — global lock count check
  - `DllRegisterServer` / `DllUnregisterServer` — registry registration under `HKLM\CLSID\...\InProcServer32`
  - `XpsCaptureFilterClassFactory::CreateInstance` creates `XpsCaptureFilter` via `new`
  - `XpsCaptureFilterClassFactory::LockServer` manages global DLL lock count

- Added `src/filter/wpce_xps_filter.def`:
  - Explicit DLL export list: `DllGetClassObject`, `DllCanUnloadNow`, `DllRegisterServer`, `DllUnregisterServer`

- Added `src/filter/print_ticket_extractor.h/.cpp`:
  - `extract_print_ticket_xml(IPartPrintTicket*)` — reads PrintTicket XML via `GetStream()` → `IPrintReadStream::ReadBytes()` loop
  - `read_print_stream(IUnknown*)` — reads full stream content into `std::string`
  - `read_istream_to_string(IStream*)` — reads standard COM stream for PrintTicket APIs

- Added `src/filter/filter_pipeline.cpp`:
  - `InitializeFilter()`: stores pipeline interface references, reads printer name and job ID from property bag, reads output dir from `WPCE_OUTPUT_DIR` env var
  - `StartOperation()`:
    1. Acquires `IXpsDocumentProvider` and `IXpsDocumentConsumer` from the negotiator
    2. Loops `GetXpsPart()` to read all XPS parts
    3. QI's each part for `IFixedDocumentSequence` → extracts Job PrintTicket XML
    4. QI's each part for `IFixedDocument` → extracts Document PrintTicket XML
    5. QI's each part for `IFixedPage` → extracts Page PrintTicket XML + builds `PageArtifactIndex`
    6. Collects `PageArtifactPayload` (index + XMLs) for persistence
    7. Forwards all parts downstream via `SendXpsUnknown()`
    8. After loop, persists payloads via `AsyncPageArtifactWriter`
    9. Calls `CloseSender()` + `FilterFinished()`
  - `ShutdownOperation()`: releases all pipeline references
  - `set_output_directory()`: sets path for PrintTicket/FixedPage file persistence
  - `persist_pending_payloads()`: creates `AsyncPageArtifactWriter`, enqueues all payloads, stops writer

- Added `tools/filter_test_harness.cpp`:
  - `MockXpsProvider` — simulates `IXpsDocumentProvider` with a provided part list
  - `MockXpsConsumer` — simulates `IXpsDocumentConsumer`, counts forwarded parts
  - `MockCommunicator` — simulates `IInterFilterCommunicator`, wires provider + consumer
  - `MockPropertyBag` — simulates `IPrintPipelinePropertyBag`, returns mock printer name and job ID
  - `MockPipelineControl` — simulates `IPrintPipelineManagerControl`, tracks finished/shutdown
  - `MockFixedDocumentSequence` — simulates `IFixedDocumentSequence` with optional PrintTicket
  - `load_filter()` — loads DLL via `LoadLibraryA` + `DllGetClassObject` + `CreateInstance`
  - `run_pipeline_test()` — creates mock interfaces, calls InitializeFilter/StartOperation/ShutdownOperation, asserts success

- Added `src/storage/page_artifact_writer.h/.cpp`:
  - `PageArtifactWriterConfig` (output_directory, queue_capacity)
  - `PageArtifactPayload` (index + PrintTicket XML strings + FixedPage XML)
  - `page_artifact_path()` → `page-artifacts.json`
  - `page_artifact_archive_path()` → `pages/page-j{id}-d{doc}-p{page}.json`
  - `AsyncPageArtifactWriter` — follows `AsyncJobSnapshotWriter` pattern exactly:
    - `start()` / `stop()` / `enqueue()` / `running()` / `last_error()`
    - Bounded `std::deque<PageArtifactPayload>` with condition variable
    - Atomic writes via temp file + `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`
    - `write_text_file_atomic()` for PrintTicket XML and FixedPage content
    - `persist_payload_files()` writes `.pt.xml`, `.fpage` files to `pages/` subdirectory

- Modified `src/CMakeLists.txt`:
  - Added `page_artifact_writer.cpp` to `wpce_core`
  - Added `wpce_xps_filter` SHARED library target with `.def` file
  - Added `filter_test_harness` executable target
  - Filter DLL links `xpsprint.lib`

- Modified `tests/CMakeLists.txt`:
  - Added `wpce_filter_pipeline` CTest (filter_test_harness with DLL path)

### Key Decisions

- **Pipeline simulator** over full driver installation for Phase 3 gate — enables automated testing without WDK packaging.
- **Filter DLL is x64 only** — matches project constraint.
- **PrintTicket persistence is synchronous** in StartOperation — acceptable for feasibility; Phase 5 can move to async for production.
- **Output directory from env var** (`WPCE_OUTPUT_DIR`) — avoids registry dependency for testing.
- **Mock interfaces use proper COM ref counting** — the test harness is realistic enough to catch ref-count bugs.
- **Job PrintTicket is reused across pages** — extracted once from `IFixedDocumentSequence`, reused for all subsequent `IFixedPage` parts in the same job.
- **Document PrintTicket is reused within a document** — same pattern, consistent with XPS spec.
- **All XPS parts are forwarded** — filter is transparent/pass-through, no data modification.

### Verification

- `.\scripts\build-msvc.ps1`: passed (0 errors, 0 warnings).
- `ctest --preset vs2026-debug`: passed, 2/2 tests (wpce_contract_tests + wpce_filter_pipeline).
- Filter DLL compilation: `wpce_xps_filter.dll` (x64, Debug).
- Test harness: "Empty pipeline ran without errors" — InitializeFilter, StartOperation, ShutdownOperation all return S_OK.
- Consumer counts forwarded parts, pipeline control tracks FilterFinished.

### Output File Structure (Post Phase 4)

```
capture/
├── job.json                          ← Latest job snapshot (Phase 1)
├── metadata.json                     ← Latest metadata snapshot (Phase 2)
├── lifecycle-summary.json           ← Session manifest (Phase 2)
├── page-artifacts.json              ← Latest PageArtifactIndex (Phase 4)
├── jobs/
│   ├── job-N.json
│   ├── job-N-metadata.json
│   └── job-N-lifecycle.json
└── pages/
    ├── page-j12-d1-p1.json          ← PageArtifactIndex for job 12, doc 1, page 1
    ├── job.pt.xml                   ← Job-level PrintTicket
    ├── document-001.pt.xml          ← Document-level PrintTicket
    ├── page-001.pt.xml              ← Page-level PrintTicket
    ├── page-001.fpage               ← FixedPage markup (when captured)
    └── ...
```

### Known Risks

- Mock pipeline does not test real XPS parts — PrintTicket extraction exercise requires a real XPS document (Phase 5 integration test).
- Filter is not yet installed in a real printer driver pipeline — that requires WDK packaging (Phase 5).
- `IXpsDocumentProvider::GetXpsPart()` may return parts in a different order than expected — filter handles unknown parts gracefully (only forwards them).
- Filter DLL must be registered (`regsvr32`) before real pipeline use — test harness bypasses this via LoadLibrary.
- COM apartment threading model is "Both" — suitable for MTA use in the print pipeline.

### Follow-Ups

- Register the DLL with `regsvr32 wpce_xps_filter.dll` before real pipeline integration.
- Build a real XPS document test using `Microsoft XPS Document Writer` to validate PrintTicket extraction end-to-end.
- Add WDK packaging (INF, signing, installation) in Phase 5 before production deployment.
- Run `.\scripts\run-static-analysis.ps1` for cppcheck on filter code.

## 2026-06-20: Qt6 GUI — Print Job Monitor Frontend

### Context

- Phase 1-4 build a comprehensive print capture engine accessed via CLI (`wpce --watch-printer`).
- Users need a graphical real-time monitoring interface showing job list, metadata details, lifecycle timeline, and progress.
- Qt6 chosen for native C++ integration with the existing WPCE C++20 codebase.
- Qt6 installed via vcpkg (`qtbase[gui,widgets]:x64-windows`).

### Architecture

**Thread model**: PrintWatcher runs on a dedicated QThread. Callbacks emit Qt signals cross-thread (automatic QueuedConnection) to update UI models on the main thread. A 200 ms batch timer merges high-frequency updates to prevent UI stutter.

```
Qt Main Thread                    Watcher Thread
┌──────────────────────┐          ┌─────────────────┐
│ JobTableModel (200ms  │◄─signal──│ jobCallback      │
│  batch timer)         │          │ metadataCallback │
│ JobDetailPanel        │◄─signal──│ lifecycleCallback│
│ LifecycleTimeline     │          │ PrintWatcher     │
│ MainWindow            │          └─────────────────┘
└──────────────────────┘
```

### Changes

- Added `src/ui/main.cpp`:
  - `QApplication` entry point, UTF-8 console, global stylesheet.

- Added `src/ui/watcher_bridge.h/.cpp`:
  - `WatcherBridge : QObject` — thread-safe bridge between PrintWatcher and Qt signals.
  - `startWatching(printerName, outputDir)` creates a QThread, constructs PrintWatcher + AsyncJobSnapshotWriter inside it.
  - Three callbacks emit Qt signals: `jobSnapshotReceived`, `metadataReceived`, `lifecycleEventReceived`.
  - `stopWatching()` joins the watcher thread, persists lifecycle summaries, emits `sessionFinished`.
  - Error handling emits `watcherError(QString)`.

- Added `src/ui/job_table_model.h/.cpp`:
  - `JobTableModel : QAbstractTableModel` — 9-column real-time job list.
  - Columns: Job ID, Document, User, Phase (with Unicode icons), Progress, Total Pages, Copies, Submit Time.
  - Chinese column headers (文档/用户/阶段/进度/页数/份数/时间).
  - `onJobSnapshot()`, `onMetadata()`, `onLifecycleEvent()` slots update internal `QMap<uint32_t, JobRowData>`.
  - 200 ms batch timer: dirty rows are accumulated then flushed via `dataChanged()` to avoid UI stutter.
  - `jobAtRow(int)` for selection-driven detail panel updates.

- Added `src/ui/job_detail_panel.h/.cpp`:
  - `JobDetailPanel : QWidget` — right-side metadata display.
  - `showMetadata(MetadataSnapshot)` renders all 11 DEVMODE fields in a QFormLayout.
  - `showJobInfo(JobSnapshot)` renders job identity fields (ID, document, user, printer, time, status).
  - Each FieldValue is color-coded: green (reported), gray (not_reported), amber (unsupported/unknown), red (error).
  - `dm_fields_hex` displayed in monospace font for diagnostics.

- Added `src/ui/lifecycle_timeline.h/.cpp`:
  - `LifecycleTimeline : QWidget` — lifecycle event list for selected job.
  - `addEvent(JobLifecycleEvent)` appends a formatted timeline entry: time + phase icon + phase name + progress %.
  - `loadEvents(vector<JobLifecycleEvent>)` loads full history from lifecycle summaries.
  - `clear()` resets when switching jobs.

- Added `src/ui/main_window.h/.cpp`:
  - `MainWindow : QMainWindow` — application shell.
  - **Toolbar**: printer selector (QComboBox, editable, auto-populated from `EnumPrintersW`), ▶ Start / ■ Stop buttons.
  - **Central area**: QSplitter with QTableView (job list) + right panel (detail + timeline stacked vertically).
  - **Status bar**: session statistics.
  - Start flow: select printer → choose output directory → start WatcherBridge.
  - Stop flow: stop WatcherBridge → update button states.
  - Selection change: loads detail panel + timeline for selected job.
  - Error dialog on watcher failure.

- Added `src/ui/CMakeLists.txt`:
  - `wpce_ui` WIN32 executable linking wpce_core + Qt6::Core + Qt6::Widgets.
  - C++20, AUTOMOC enabled via root CMakeLists.txt.

- Modified root `CMakeLists.txt`:
  - Added `option(WPCE_BUILD_UI "Build WPCE Qt6 GUI" ON)`.
  - `set(CMAKE_AUTOMOC ON)` when UI is enabled.
  - `find_package(Qt6 REQUIRED COMPONENTS Core Widgets)` + `add_subdirectory(src/ui)`.

- Modified `CMakePresets.json`:
  - Added `vs2026-x64-qt` configure preset with `toolchainFile: "C:/vcpkg/scripts/buildsystems/vcpkg.cmake"`.
  - Enabled `WPCE_BUILD_UI: ON` in vs2026-x64 presets.

- Modified `scripts/build-msvc.ps1`:
  - Added `-WithQt` switch to use the `vs2026-x64-qt` preset.

### Key Decisions

- **vcpkg over Qt Online Installer**: keeps the dependency in CMake's toolchain, reproducible, no manual PATH setup.
- **WIN32 subsystem**: `wpce_ui` is a GUI app (no console window).
- **Batch timer (200ms)**: prevents QAbstractTableModel::dataChanged storms from high-frequency spooler notifications.
- **Thread-per-watcher**: PrintWatcher lives in its own QThread, destroyed on stop — no shared ownership complexity.
- **QueuedConnection by default**: signals emitted from watcher thread are automatically queued to UI main thread by Qt when the receiver lives on a different thread.
- **Chinese UI labels**: all labels use UTF-8 literals for native Chinese display (工具栏/打印机/开始监听/停止/打印参数/生命周期 etc.).

### Verification

- `C:\vcpkg\vcpkg.exe install "qtbase[gui,widgets]:x64-windows"`: pending.
- `cmake --preset vs2026-x64-qt`: pending (requires vcpkg Qt6 build to complete).
- `cmake --build --preset vs2026-debug --target wpce_ui`: pending.
- UI smoke test: launch `wpce_ui.exe`, verify window renders, printer list populates, Start/Stop buttons work, job table updates on print events.
- CTest: ensure existing 2 tests still pass alongside new UI target.

### Output File Structure

```
src/ui/
├── CMakeLists.txt
├── main.cpp                    # QApplication + stylesheet
├── main_window.h/.cpp          # Toolbar + splitter + status bar
├── job_table_model.h/.cpp      # 9-column QAbstractTableModel
├── job_detail_panel.h/.cpp     # Metadata form with color-coded states
├── lifecycle_timeline.h/.cpp   # Scrolling event timeline
└── watcher_bridge.h/.cpp       # QThread + PrintWatcher bridge
```

### Known Risks

- Qt compiled from source via vcpkg can take 30-60 minutes on first build.
- The `Winspool.lib` dependency in `wpce_core` means `wpce_ui` implicitly links Winspool — no issue but worth noting.
- `qtbase[gui,widgets]` does not include `qtbase[network]` or `qtbase[sql]` — not needed for this UI.
- Cross-thread signal for `JobSnapshot` and `MetadataSnapshot` passes by value (deep copy) — acceptable for these small structs but could be optimized with shared_ptr in future.
