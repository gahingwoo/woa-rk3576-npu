# Performance counters

rknpu.sys publishes its job statistics as Windows performance counters, so
perfmon, `typeperf`, `Get-Counter` and any PDH program (the rk-monitor tray
app in woa-rk3576, for one) can show what the NPU is doing. Windows 11 23H2,
the newest release this board runs, has no NPU page in Task Manager; these
counters are the way to watch it.

## What there is

**`RK3576 NPU`**, one instance per core, `core0` and `core1`:

| counter | type | meaning |
|---|---|---|
| `% Busy Time` | 100 ns timer | share of the interval in which a job held the core, from its first task's `OPERATION_ENABLE` to its last interrupt. A running job counts up to the moment of the query |
| `Jobs/sec` | rate | jobs completed per second (one job is one SUBMIT, possibly several tasks) |
| `Tasks/sec` | rate | tasks finished per second |
| `Avg. Job Time (us)` | average | job time on the core over the jobs that ended in the interval |
| `Avg. Submit to Complete (us)` | average | SUBMIT entering the driver to the driver completing the job in its DPC. Includes a wait for the other core on the same CBUF window |
| `Jobs Submitted`, `Jobs Completed` | totals | since the driver started |
| `Job Timeouts`, `Core Resets`, `IOMMU Faults`, `CBUF Waits` | totals | the same numbers `rknputest info` prints |
| `Job Running` | 0 or 1 | a job is on the core now |

**`RK3576 NPU Memory`**, one instance: `Buffer Bytes` and `Buffers` (every
NPU buffer that exists, all handles together), `NPU Address Space Bytes`
(mapped through the IOMMU; 0 in bypass mode) and `Page Table Pages`.

The averages and rates are computed by the consumer from two samples, so the
first sample after starting perfmon or `typeperf` shows 0.

## How it works

The driver is a kernel-mode PCW provider (`drivers/rknpu/perf.c`), built as
the WDK's kcs sample is: `drivers/rknpu/rknpu.man` is the counter manifest,
ctrpp turns it into `rknpuCounters.h` (the `RknpuRegisterNpuCore`,
`RknpuAddNpuCore`, ... helpers) and `rknpuCounters.rc` (the names, linked
into rknpu.sys). Both counter sets use a callback: nothing is computed until
a consumer asks.

The job path adds a few 64-bit sums under the interrupt lock it already
holds, from the timestamps M3 and M7 already take (T0, T1, T2 and the
SUBMIT's entry time); `CORE_PERF` in `rknpu.h` has them. The callback reads
a consistent copy through a sequence count, without taking any lock, and
converts QPC ticks to 100 ns and microseconds only then.

If registration fails, the driver logs `performance counters: ... not
registered` and runs as before. If the provider was never installed with
lodctr, the driver also runs as before; the counter sets are just not
visible.

## Installing

An INF has no directive that registers a counter provider. The INF copies
`rknpu.man` into the driver store beside `rknpu.sys`, and
`tools\rknpu-perfcounters.ps1`, as administrator, runs

```
unlodctr /g:{6f33a325-aa80-4286-b76c-0b11e14f9f2b}
lodctr /m:<driver store folder>\rknpu.man <driver store folder>
```

with the folder the running rknpu.sys was loaded from. lodctr records that
rknpu.sys as the file the names come from, so after reinstalling a new
package (a new driver store folder) run the script again. `-Uninstall` only
runs the unlodctr. Its log is `%SystemRoot%\Temp\rknpu-perfcounters.log`.

## Viewing

**perfmon**: `perfmon /sys`, the green plus, then `RK3576 NPU`, all
instances, `% Busy Time` and `Jobs/sec`. The report view (Ctrl+G twice)
shows every counter as a number.

**PowerShell**:

```
Get-Counter -ListSet "RK3576 NPU" | Select-Object -ExpandProperty Counter
Get-Counter "\RK3576 NPU(*)\*" -SampleInterval 1 -MaxSamples 5
Get-Counter "\RK3576 NPU(*)\% Busy Time", "\RK3576 NPU(*)\Jobs/sec" -Continuous
Get-Counter "\RK3576 NPU Memory\*"
```

**typeperf** (CSV, good for a log file):

```
typeperf -q "RK3576 NPU"
typeperf "\RK3576 NPU(*)\% Busy Time" "\RK3576 NPU(*)\Jobs/sec" "\RK3576 NPU(*)\Avg. Submit to Complete (us)" -si 1
typeperf "\RK3576 NPU(*)\*" "\RK3576 NPU Memory\*" -si 1 -sc 120 -o R:\npu\perf\counters.csv -y
```

## Board test

You need the `rknpu-m7-arm64` artifact of the CI run on branch
`npu-perfcounters` (driver, `rknputest.exe`, `signer.cer`,
`rknpu-reinstall.ps1`, `rknpu-perfcounters.ps1`, `DriverVer.txt`). Unpack it
into `R:\npu\perf`. Delete the previous unbound rknpu packages from the
driver store first (`pnputil /enum-drivers`, then `pnputil /delete-driver
oemNN.inf` for each rknpu one that is not bound), as with every test.

**1. Stage the driver**, as administrator, after `w32tm /resync /force`:

```
certutil -addstore -f Root R:\npu\perf\signer.cer
certutil -addstore -f TrustedPublisher R:\npu\perf\signer.cer
pnputil /add-driver R:\npu\perf\rknpu\rknpu.inf
```

**2. Mark the device for reinstall, as SYSTEM, then reboot:**

```
schtasks /create /tn rknpu-reinstall /sc once /st 23:59 /ru SYSTEM /f /tr "powershell -NoProfile -ExecutionPolicy Bypass -File R:\npu\perf\rknpu-reinstall.ps1"
schtasks /run /tn rknpu-reinstall
schtasks /delete /tn rknpu-reinstall /f
shutdown /r /t 0
```

**3. Check the binding** after the reboot:

```
pnputil /enum-devices /instanceid "ACPI\RKCP3576\0" /drivers
R:\npu\perf\rknputest.exe info
```

The bound driver's version is the one in `DriverVer.txt`.

**4. Register the counters**, as administrator:

```
powershell -NoProfile -ExecutionPolicy Bypass -File R:\npu\perf\rknpu-perfcounters.ps1
```

It ends with `RK3576 NPU: 12 counters, instances core0, core1` and
`RK3576 NPU Memory: 4 counters`.

**5. Idle check**, in a second administrator window:

```
typeperf "\RK3576 NPU(*)\*" "\RK3576 NPU Memory\*" -si 1 -sc 10
```

Every `% Busy Time`, `Jobs/sec` and `Job Running` is 0; `Buffers` is 0.

**6. Load**, with the counter log running in the second window:

```
typeperf "\RK3576 NPU(*)\*" "\RK3576 NPU Memory\*" -si 1 -sc 180 -o R:\npu\perf\m6-counters.csv -y
R:\npu\perf\rknputest.exe m6 --iters 2000
R:\npu\perf\rknputest.exe m6 --iters 200 --mb 0 --collide
R:\npu\perf\rknputest.exe m6 --iters 20 --mb 0 --fault
```

Instead of rknputest, a charsiu run (docs/M7.md, step 3) works as well; it
drives both cores at once.

**What confirms it:**

| question | where | answer |
|---|---|---|
| the counters exist | step 4 | both sets, 12 and 4 counters, instances `core0` and `core1` |
| idle is idle | step 5 | 0 busy, 0 jobs/s |
| busy and rates move | `m6-counters.csv` during `m6 --iters 2000` | `% Busy Time` above 0 on the cores m6 is using, never above 100; `Jobs/sec` above 0; `Avg. Job Time (us)` near the `hw ... us` figures m6 prints; `Avg. Submit to Complete (us)` at or below m6's `LATENCY: SUBMIT to WAIT return` line (that one also has the WAIT's return to the program) |
| totals agree with the driver | m6's `core N totals` lines, and `rknputest info` after the runs | `Jobs Submitted`, `Jobs Completed`, `Job Timeouts`, `Core Resets`, `IOMMU Faults` and `CBUF Waits` in the last CSV row equal the driver's per-core totals |
| the collide case | the `--collide` run | `CBUF Waits` grows |
| the fault case | the `--fault` run | `IOMMU Faults` and `Core Resets` grow on the core that ran it |
| memory | during the m6 scatter step | `Buffer Bytes` climbs to about 768 MiB and drops back to 0 after the run |
| no cost | the m6 latency lines | the same as the M7 board run, within noise |

Send `m6-counters.csv`, the rknputest output and
`%SystemRoot%\Temp\rknpu-perfcounters.log`.

## Backing out

`rknpu-perfcounters.ps1 -Uninstall` removes the counter sets; the driver
does not depend on them. To go back to the M7 driver, stage that package and
repeat steps 1 to 3.
