# WPCE Print Simulation Testing

This project can run repeatable print monitoring tests without physical
printer hardware by using two local test printers:

- `WPCE Sim Printer`: `Generic / Text Only` on `nul:` for normal, matrix,
  concurrency, and constrained-PC scenarios.
- `WPCE Fault Printer`: `Generic / Text Only` on an unreachable TCP port plus
  queue pause/resume for stalled, cancel, and network-timeout-style scenarios.

## Setup

Run from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\setup-virtual-printers.ps1
```

The setup is idempotent. It installs the built-in `Generic / Text Only` driver
if needed and only creates/updates printers named with the `WPCE` test prefix.

## Run Scenarios

Run the full suite:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run-print-simulation.ps1 -Scenario all -JobCount 12 -Concurrency 4 -DurationSeconds 9 -StallTimeoutSeconds 3
```

Run a single scenario:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run-print-simulation.ps1 -Scenario pc-stress -JobCount 12 -DurationSeconds 12 -StallTimeoutSeconds 3 -SkipSetup
```

Supported scenarios:

- `smoke`
- `matrix`
- `concurrency`
- `pc-stress`
- `stalled`
- `cancel`
- `network-timeout`
- `all`

## Outputs

Each scenario writes to:

```text
artifacts/print-simulation/<timestamp>-<scenario>/
```

Important files:

- `scenario-report.json`: scenario-level result summary.
- `watcher.stdout.log`: raw watcher event stream.
- `watcher.stderr.log`: drops or writer errors.
- `perf.csv`: periodic watcher performance samples.
- `lifecycle-summary.json`: session lifecycle manifest.
- `jobs/job-*-lifecycle.json`: per-job lifecycle details.

The latest aggregate summary is:

```text
artifacts/print-simulation/latest-summary.json
```

## Last Local Validation

On 2026-06-20, local validation created both virtual printers and ran:

```powershell
ctest --test-dir build\vs2026-x64 -C Debug --output-on-failure
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run-print-simulation.ps1 -Scenario pc-stress -JobCount 12 -DurationSeconds 12 -StallTimeoutSeconds 3 -SkipSetup
```

Results:

- CTest: 16/16 passed.
- `pc-stress`: 12 submitted, 12 snapshot jobs, 12 lifecycle jobs, all completed.
- Earlier full-suite run validated `smoke`, `matrix`, `concurrency`,
  `stalled`, `cancel`, and `network-timeout` style scenarios.

## Notes

- `network-timeout` is simulated by holding the fault printer queue paused. The
  plain `Generic / Text Only` TCP-port printer may still complete too quickly to
  represent a real network printer timeout by itself.
- Some virtual drivers do not report all DEVMODE fields. Missing fields are
  expected to appear as `not_reported` or `unknown`.
