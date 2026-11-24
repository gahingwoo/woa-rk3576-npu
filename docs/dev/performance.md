# Performance

Decode speed against Linux, where the time goes, and the switches. From the
board runs of 2026-11-24 on the TaishanPi 3M (NPU at 594 MHz, Llama-3.2-1B
Instruct Q8_0 run as charsiu w4a16). One run per arm per bench; differences
of 3 to 6 ms a token are within run-to-run noise, so a default only moved
where two benches agreed.

| | tok/s |
|---|---|
| Windows, M6 driver and port | 12.12 (11.29 in the M7 bench's M6 arm) |
| Windows, M7 defaults, run 1 (CPU 1608 MHz, rknpu 5.13.22.1) | 14.23 |
| Windows, M7 defaults, run 3 (CPU 2208 / 2016 MHz, rknpu 8.54.38.606) | 15.05 |
| Linux, charsiu on rocket, CPU pinned at maximum | 18.15 |

Each M7 bench arm against the defaults of the same run (`short-npu2`), ms a
token, run 1 / run 3; negative is faster than the defaults. Every arm printed
the same text in both runs.

| arm | what it changed | run 1 | run 3 | decision |
|---|---|---|---|---|
| `short-nospin` | `CHARSIU_WIN_SPIN_US=0` | -6.3 | -3.5 | **spin 0 is the default now** |
| `short-ucache` | `CHARSIU_RKNPU_UCACHE=1` | -5.9 | -3.2 | **A/A, not a measurement**: the probe trapped on this board (`user-mode DC CVAC / DC CIVAC trap (SCTLR_EL1.UCI is 0)` in its log), so the arm ran the defaults. Made the default anyway: identical here, user-mode where a kernel allows it |
| `short-async` | `CHARSIU_RKNPU_ASYNC=1` | +9.4 | +14.4 | stays off |
| `short-npu1` | one core | +23.5 | +35.0 | both cores stay |
| `short-nofuse` | `CHARSIU_RKNPU_FUSE=0` | -3.7 | +4.6 | noise; stays on |
| `short-noqos` | no CPU hold | -6.1 | +5.7 | noise; stays on |
| `short-idle` | processor idle disabled | -4.2 | -1.8 | small; a power-scheme change, not taken |
| `short-nopin` | decode thread not pinned | +5.3 | | pinning stays |

**Read the table with the `short-ucache` row in mind.** That arm ran the same
code as the defaults and came out 5.9 and 3.2 ms faster, so a single arm
moves by that much for no reason; `short-nospin` (-6.3 / -3.5) is no larger
than that. Spin 0 is the default because it is no slower and spends no CPU,
not because the gain is proven. A decision on a few ms needs several runs
of each arm, interleaved.

Where SUBMIT's time goes, from the M7 timing line (run 1, defaults): about
110 us a SUBMIT, of which about 100 us is inside the driver's own handler
(`drv_us`), so the IOCTL path itself is about 10 us. Without the fused
cleans (`short-nofuse`) the driver part is about 91 us. The stream copy and
check measured 0.83 us a stream on the host, so most of the driver's time is
somewhere else in `NpuSubmit`: the next thing to instrument. The interrupt
reaches the DPC in 3 to 4 us and the DPC wakes the waiter in about 9 us.

### Inside SUBMIT (M8)

The driver now splits each SUBMIT into the phases in `rknpu_ioctl.h`
(`RKNPU_SP_*`): the entry and JobLock wait, the power and clock check, the
idle check, the arena and BoLock, the IOMMU check, the fused cleans, the
stream copy, the stream check (rules and CBUF key), the shared-CBUF wait,
the bookkeeping, and the start registers. Each SUBMIT returns its own split
when the caller passes an `rknpu_submit_m8_out`; charsiu sums them and
prints `charsiu rknpu dev N submit phases: ...` with `CHARSIU_RKNPU_STATS=1`,
and `m7-bench.ps1` puts them in `decode.txt`. `rknputest submit-stats
[--reset]` (and `rknputest info`) prints the driver's per-core sums.

What M8 changed in SUBMIT, from reading the code, before any board number:

- **The stream check read the write-combined arena.** M7 copied each stream
  into the copy arena (Normal non-cacheable on ARM64) and then checked it
  there, and computed the CBUF key from it in a second pass: every entry was
  read from DRAM twice, about 150 entries a task and 2 to 3 tasks a SUBMIT.
  M8 copies into a cached shadow only the driver can see, checks the shadow,
  and writes it to the arena in one go. What runs is still exactly what was
  checked.
- **The power and clock check read the PLL too.** `NpuSafeToRead` reads the
  PMU, five CRU registers and the three GPLL registers for the clock
  report; SUBMIT now reads only the six its answer depends on.
- Not changed, and why: SUBMIT allocates nothing (the arena and shadow are
  made once per core); the IOMMU is never zapped per SUBMIT (only buffer
  map and unmap touch the tables), but its check is six register reads,
  which the `mmu` phase will price before anything trusts a cached state;
  the stream copy is a few KiB from a cached mapping.

`CHARSIU_RKNPU_M7SUBMIT=1` asks an M8 driver for M7's way of both, so the
`short-m7submit` bench arm against the defaults is the before and after on
one driver, and its phase rows say which phase moved.

Run 1 also measured the weight cache: staging 77.2 s without it, 20.5 s the
first time it is read and about 4 to 5 s after that.

The bench arms since this change: `short-spin` (`CHARSIU_WIN_SPIN_US=50`)
and `short-noucache` (`CHARSIU_RKNPU_UCACHE=0`, the driver's fused PREP and
FINI) are the controls for the two new defaults, and `short-nofuse` turns
both user-mode maintenance and fusing off.

The NPU hardware is not where Windows loses: it is busy about 34 ms a token
per core on both systems. The gap is the time the NPU sits idle between
jobs, which is CPU work and IOCTL cost on Windows. In the M6 attribution,
ranked:

1. CPU work running slower: the decode thread was not pinned to the A72s,
   the thread-pool barrier was slow, and the firmware's default CPU clock is
   1608 MHz against Linux's 2208 MHz (about 12 ms a token).
2. The SUBMIT IOCTL, twice a call, serial on the decode thread (about 9 ms).
3. PREP_BO and FINI_BO as one IOCTL each, 380 a token (about 6 ms).
4. The interrupt-to-waiter path (1 to 2 ms).

What M7 changed, each behind a switch (defaults in
[architecture](architecture.md#settings)):

- FINI deferred into the next SUBMIT and PREP done by WAIT: 4 IOCTLs a
  decode call instead of 10 (`CHARSIU_RKNPU_FUSE`).
- A lean WAIT after a clean finish, and a priority boost when the DPC wakes
  the waiter (`M7WakeBoost`).
- charsiu's CPU pinning answered from Windows (`CHARSIU_WIN_SYSFS`), SRW
  locks for its pool, with no spin before a waiter sleeps by default
  (`CHARSIU_WIN_SYNC`; `CHARSIU_WIN_SPIN_US` turns a spin on), and its CPU
  hold: no power throttling and a 1 ms timer (`CHARSIU_RKNPU_QOS`).
- Cache maintenance in the process (`CHARSIU_RKNPU_UCACHE`, default 1): DC
  CVAC and DC CIVAC on the buffer's own mapping, no IOCTL, when a probe at
  the first open shows Windows lets user mode run them (SCTLR_EL1.UCI); it
  did on the TaishanPi 3M. Where it does not, or with 0, the driver does it,
  fused into SUBMIT and WAIT as above.
- Overlapping a call's two SUBMITs (`CHARSIU_RKNPU_ASYNC`) exists and is off:
  it moves a SUBMIT failure where charsiu's decode path does not check it,
  and it measured slower in both runs.
- The weight cache (`charsiu_run --cache`) cuts staging from about 77 s to
  about 5 s.

Firmware settings that affect the numbers (edk2-rk3576): "CPU Maximum
Frequency" (1608 MHz by default), "CPU Idle States" (off by default), and
the MADT gives every core EfficiencyClass 0, so Windows' scheduler does not
tell the clusters apart.

The full attribution, as written before the M7 run, is in
[history](../history/2026-11-24-m7.md).
