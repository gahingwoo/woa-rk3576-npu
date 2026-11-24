# Performance

Decode speed against Linux, where the time goes, and the switches. From the
board runs of 2026-11-24 on the TaishanPi 3M (NPU at 594 MHz, Llama-3.2-1B
Instruct Q8_0 run as charsiu w4a16). Single runs per arm; differences of 3
to 6 ms a token are within run-to-run noise.

| | tok/s |
|---|---|
| Windows, M6 driver and port | 12.12 (11.29 in the M7 bench's M6 arm) |
| Windows, M7 defaults | 14.23 |
| Windows, best M7 arms | 15.5 to 16.6 |
| Linux, charsiu on rocket, CPU pinned at maximum | 18.15 |

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
  locks with a short spin for its pool (`CHARSIU_WIN_SYNC`,
  `CHARSIU_WIN_SPIN_US`), and its CPU hold: no power throttling and a 1 ms
  timer (`CHARSIU_RKNPU_QOS`).
- Overlapping a call's two SUBMITs (`CHARSIU_RKNPU_ASYNC`) and cache
  maintenance in the process (`CHARSIU_RKNPU_UCACHE`) exist, and are off by
  default: the first moves a SUBMIT failure where charsiu's decode path does
  not check it, the second depends on what the kernel lets user mode do.
- The weight cache (`charsiu_run --cache`) cuts staging from about 77 s to
  about 5 s.

Firmware settings that affect the numbers (edk2-rk3576): "CPU Maximum
Frequency" (1608 MHz by default), "CPU Idle States" (off by default), and
the MADT gives every core EfficiencyClass 0, so Windows' scheduler does not
tell the clusters apart.

The full attribution, as written before the M7 run, is in
[history](../history/2026-11-24-m7.md).
