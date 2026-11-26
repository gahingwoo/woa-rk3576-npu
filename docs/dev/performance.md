# Performance

Decode speed against Linux, where the time goes, and the switches. TaishanPi
3M, Windows 11 23H2, NPU at 594 MHz, Llama-3.2-1B-Instruct Q8_0 run as
charsiu w4a16, `tools/m7/m7-bench.ps1`. One run per arm per bench.

## Results

| Run | tok/s | ms a token | of Linux |
|---|---|---|---|
| Linux, charsiu on rocket, CPU pinned at maximum | 18.15 | 55.1 | |
| Windows, M7 defaults, 2026-11-26, fan on | **16.12** | 62.0 | 89% |
| Windows, M7 defaults, 2026-11-25, fan off (thermally throttled) | 12.91 | 77.5 | 71% |
| Windows, M7 defaults, 2026-11-24, CPU 2208 / 2016 MHz (rknpu 8.54.38.606) | 15.05 | | 83% |
| Windows, M7 defaults, 2026-11-24, CPU 1608 MHz (rknpu 5.13.22.1) | 14.23 | | 78% |
| Windows, M6 driver and port, 2026-11-24 | 12.12 | 82.5 | 67% |

The 2026-11-25 and 2026-11-26 runs used the M7 driver (`m7-defaults`,
23790ab), before the M8 SUBMIT changes. Keep a fan on the SoC: without it the
same bench lost 3.2 tok/s.

## Each switch, 2026-11-26

Each arm against the defaults of the same run (`short-npu2`, 62.0 ms a
token). Positive is slower. All arms printed the same text.

| arm | what it changes | ms a token |
|---|---|---|
| `short-noucache` | `CHARSIU_RKNPU_UCACHE=0` | +0.1 |
| `short-nofuse` | `CHARSIU_RKNPU_UCACHE=0 CHARSIU_RKNPU_FUSE=0` | +1.4 |
| `short-nopin` | `CHARSIU_WIN_SYSFS=0`, decode thread not pinned | +15.5 |
| `short-pthread` | `CHARSIU_WIN_SYNC=pthread` | -0.8 |
| `short-spin` | `CHARSIU_WIN_SPIN_US=50` | +4.6 |
| `short-noqos` | `CHARSIU_NPU_IDLE=1`, no CPU hold | +9.8 |
| `short-async` | `CHARSIU_RKNPU_ASYNC=1` | +14.1 |
| `short-idle` | processor idle disabled for the arm | +7.0 |
| `short-npu1` | `CHARSIU_NPU_ONEDEV=1`, one core | +41.2 |
| `short-m6` | every M7 change off, no weight cache | 218.9 ms (4.57 tok/s) |

A single arm moves 3 to 6 ms a token for no reason. `short-noucache` is an
A/A run on this board: the user-mode cache maintenance probe traps
(`SCTLR_EL1.UCI` is 0), so both arms do the maintenance in the driver.

The 2026-11-24 runs (1608 MHz / 2208 MHz CPU) that set the current defaults,
against the defaults of that time:

| arm | run 1 | run 3 | decision |
|---|---|---|---|
| `CHARSIU_WIN_SPIN_US=0` | -6.3 | -3.5 | spin 0 became the default (no slower, spends no CPU) |
| `CHARSIU_RKNPU_UCACHE=1` | -5.9 | -3.2 | A/A (probe trapped); default 1 anyway, user mode where a kernel allows it |
| `CHARSIU_RKNPU_ASYNC=1` | +9.4 | +14.4 | stays off |
| one core | +23.5 | +35.0 | both cores stay |
| `CHARSIU_RKNPU_FUSE=0` | -3.7 | +4.6 | noise; stays on |
| no CPU hold | -6.1 | +5.7 | noise; stays on |
| processor idle disabled | -4.2 | -1.8 | a power-scheme change, not taken |
| decode thread not pinned | +5.3 | | pinning stays |

## Staging and the weight cache

`charsiu_run --cache` keeps the quantised weights beside the model.
2026-11-26: 97.9 s staging without it, 102.8 s writing it, 20.6 s the first
read, 12.8 to 14.2 s after. 2026-11-24 (run 1): 77.2 s without, 20.5 s first
read, 4 to 5 s after.

## Where the time goes

The NPU is busy about 34 ms a token per core on both Windows and Linux. The
gap is the time it sits idle between jobs. The M6 attribution, ranked:

1. CPU work slower, about 12 ms: decode thread not pinned to the A72s, a slow
   pool barrier, CPU at 1608 against Linux's 2208 MHz.
2. SUBMIT IOCTL, twice a call, serial, about 9 ms.
3. PREP_BO and FINI_BO as one IOCTL each, 380 a token, about 6 ms.
4. Interrupt to waiter, 1 to 2 ms.

What M7 changed, each behind a switch ([settings](architecture.md#settings)):
PREP and FINI fused into WAIT and SUBMIT (4 IOCTLs a decode call instead of
10), a lean WAIT and a wake boost, the decode thread pinned from Windows'
core data, SRW locks for charsiu's pool, the CPU hold, user-mode cache
maintenance where allowed, and the weight cache. Overlapping a call's two
SUBMITs exists and is off: it was slower in every run, and it moves a SUBMIT
failure where charsiu's decode path does not check it.

SUBMIT on the M7 driver: about 110 us, about 100 of it inside `NpuSubmit`.
The interrupt reaches the DPC in 3 to 4 us; the DPC wakes the waiter in
about 9 us.

## SUBMIT phases (M8, CI only)

The driver splits each SUBMIT into the `RKNPU_SP_*` phases in
`rknpu_ioctl.h`. `rknputest submit-stats [--reset]` and `rknputest info`
print the per-core sums; `CHARSIU_RKNPU_STATS=1` makes charsiu print its
own; `m7-bench.ps1` puts them in `decode.txt`. M8 checks streams in a cached
shadow instead of the write-combined arena (M7 read every entry from DRAM
twice) and reads only the six registers the power and clock check depends
on. `CHARSIU_RKNPU_M7SUBMIT=1` (arm `short-m7submit`) asks an M8 driver for
M7's way, as the before and after. Not on a board yet.

## Firmware settings that move the numbers

edk2-rk3576: "CPU Maximum Frequency" (1608 MHz by default), "CPU Idle
States" (off by default). The MADT gives every core EfficiencyClass 0, so
Windows' scheduler does not tell the clusters apart; the port's pinning
works around it for charsiu.
