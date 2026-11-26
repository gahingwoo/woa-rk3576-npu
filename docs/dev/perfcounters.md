# Performance counters

rknpu.sys publishes its job statistics as Windows performance counters, for
perfmon, `typeperf`, `Get-Counter` and any PDH program (rk-monitor in
woa-rk3576, for one). Task Manager on 23H2 has no NPU page; these counters
are the way to watch the NPU.

## Counters

**`RK3576 NPU`**, one instance per core, `core0` and `core1`:

| counter | meaning |
|---|---|
| `% Busy Time` | share of the interval a job held the core, first task's `OPERATION_ENABLE` to last interrupt |
| `Jobs/sec`, `Tasks/sec` | jobs (one SUBMIT) and tasks completed per second |
| `Avg. Job Time (us)` | job time on the core, over the jobs that ended in the interval |
| `Avg. Submit to Complete (us)` | SUBMIT entering the driver to completion in the DPC, including any wait for the other core's CBUF window |
| `Jobs Submitted`, `Jobs Completed` | totals since the driver started |
| `Job Timeouts`, `Core Resets`, `IOMMU Faults`, `CBUF Waits` | totals, the numbers `rknputest info` prints |
| `Job Running` | 0 or 1 |

**`RK3576 NPU Memory`**: `Buffer Bytes`, `Buffers` (all handles together),
`NPU Address Space Bytes` (0 in bypass mode), `Page Table Pages`.

Rates and averages need two samples, so the first sample shows 0.

## How it works

The driver is a kernel-mode PCW provider (`drivers/rknpu/perf.c`).
`rknpu.man` is the manifest; ctrpp turns it into `rknpuCounters.h` and
`rknpuCounters.rc`. The job path adds a few 64-bit sums under the interrupt
lock it already holds (`CORE_PERF` in `rknpu.h`); the callback reads a
consistent copy through a sequence count, without a lock, only when a
consumer asks. If registration fails, or the provider was never installed,
the driver runs as before and the sets are just not visible.

## Installing

An INF cannot register a counter provider. The INF copies `rknpu.man` beside
`rknpu.sys` in the driver store, and `tools\rknpu-perfcounters.ps1`, as
administrator, runs `unlodctr` and `lodctr /m:` against the folder the
running rknpu.sys was loaded from. A new driver package is a new folder, so
run the script again after each driver install. `-Uninstall` only
unregisters. Log: `%SystemRoot%\Temp\rknpu-perfcounters.log`.

The woa-rk3576 `rk3576-tools-arm64` bundle (its `install.cmd`) registers
these counters and maliwddm's itself, and again at every start when a driver
update moved them.

## Viewing

```
Get-Counter -ListSet "RK3576 NPU" | Select-Object -ExpandProperty Counter
Get-Counter "\RK3576 NPU(*)\% Busy Time", "\RK3576 NPU(*)\Jobs/sec" -Continuous
Get-Counter "\RK3576 NPU Memory\*"
typeperf "\RK3576 NPU(*)\*" "\RK3576 NPU Memory\*" -si 1 -sc 120 -o R:\npu\perf\counters.csv -y
```

perfmon: `perfmon /sys`, the green plus, `RK3576 NPU`, all instances; Ctrl+G
twice for the report view.

## Board check

With the driver installed ([use](../use.md)):

1. `rknpu-perfcounters.ps1` ends with `RK3576 NPU: 12 counters, instances
   core0, core1` and `RK3576 NPU Memory: 4 counters`.
2. Idle: `typeperf "\RK3576 NPU(*)\*" "\RK3576 NPU Memory\*" -si 1 -sc 10`
   shows 0 busy, 0 jobs/s, 0 buffers.
3. Load, with the `typeperf ... -o m6-counters.csv` log running:
   `rknputest m6 --iters 2000`, then `m6 --iters 200 --mb 0 --collide`,
   then `m6 --iters 20 --mb 0 --fault`.

What confirms it:

- `% Busy Time` above 0 and never above 100, `Jobs/sec` above 0;
  `Avg. Job Time (us)` near m6's `hw ... us`; `Avg. Submit to Complete (us)`
  at or below m6's `LATENCY: SUBMIT to WAIT return`.
- The totals in the last CSV row equal `rknputest info`'s per-core totals.
- `--collide` grows `CBUF Waits`; `--fault` grows `IOMMU Faults` and
  `Core Resets` on its core.
- `Buffer Bytes` climbs to about 768 MiB during the scatter step and returns
  to 0.
- m6's latency lines are unchanged within noise.

## Backing out

`rknpu-perfcounters.ps1 -Uninstall` removes the sets; the driver does not
depend on them.
