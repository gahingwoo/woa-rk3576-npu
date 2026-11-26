# History

How the driver and the port got here, a few lines per milestone. The full
milestone pages (plans, board logs, design notes) were removed from the tree
on 2026-11-26; read them with
`git show 3686706:docs/history/<file>`, for example
`git show 3686706:docs/history/2026-11-24-m7.md`.

| File in 3686706 | Milestone |
|---|---|
| `2026-11-22-feasibility.md` | the feasibility study |
| `2026-11-22-m2.md` | M2, driver skeleton |
| `2026-11-22-m3.md` | M3, buffers, submission, interrupts |
| `2026-11-22-m4.md` | M4, the charsiu port |
| `2026-11-24-m6.md` | M6, the driver owns the IOMMU |
| `2026-11-24-m7.md` | M7, decode against Linux |

## Feasibility, 2026-11-22 (desk only)

Shape A chosen: a KMDF driver with rocket's job model, and charsiu ported by
replacing its one OS file, `src/device.c`. Shape B, an MCDM driver for
DirectML or Windows ML, was ruled out on Windows 11 23H2: a compute-only
device must compile D3D12 compute shaders and this NPU has no shader core,
and Windows ML's NPU providers need 24H2. The first step was a Linux
measurement (M0).

## M0, Linux measurement, passed 2026-11-22

Under Armbian edge with rocket on the TaishanPi 3M: a register snapshot after
a real NPU job (the target state for the firmware's `Npu.c`), and one GEMM
with the NPU IOMMU not translating, 3 of 3 `M0 BYPASS PASS`, 0 of 64 wrong,
canary unchanged. So the NPU works with paging off, and M1 to M5 could use
contiguous buffers below 4 GiB. Kit: [tools/m0](../tools/m0/README.md).

## M1 and M2, firmware and driver skeleton, passed 2026-11-22

Firmware: BUCK2 found at 750 mV and set to 800 mV, NPU clock 594 MHz, both cores
report `version=0x46495245 version_num=0x00010002`, IOMMU status 0x18 (left
in reset). Windows started `ACPI\RKCP3576`; `rknputest info` ended `RKNPU
INFO: OK` with GSIV 279 and 280 connected.

## M3, buffers, submission, interrupts, passed 2026-11-22

IOMMU bypass, buffers contiguous below 4 GiB and write-combined. Every
SUBMIT stream is copied and checked (allow list, no PC-block writes, the five
address registers inside the caller's buffers), because in bypass the NPU can
write anywhere. `rknputest m3`: int8 GEMMs from 1x64x64 to 16x1024x1024,
exact against the CPU on both cores.

## M4, charsiu on Windows, run 2026-11-24

charsiu built with llvm-mingw, unchanged except `src/device.c`. `port/libm`
makes the float math bit-identical to arm64 glibc. On the board the CPU arms
matched the desk's text exactly (short, long and logits). The NPU arm was 3
to 5 times slower than the CPU and did not repeat itself (submit counts 6484
against 6613): 4 MiB contiguous allocations below 4 GiB failed from 80 to
370 MiB held, each failure left a different tensor on the CPU, and the
device was exclusive, so the second core never opened. 0 timeouts and 0 DMA
errors over about 26k submits.

## M6, the driver owns the IOMMU, passed 2026-11-24

The driver builds the NPU page tables itself (scattered pages, anywhere
below 2^40), buffers are CPU-cached with explicit maintenance, the device is
no longer exclusive and WAIT holds no lock, so both cores overlap. Decode
12.12 tok/s.

The second board run found both cores wrong when run at once. Cause: the two
cores share one CBUF, and rknputest's streams all used window 0 (charsiu
gives device d window d, so its numbers were never affected). Fix: per-core
windows in the tests, and SUBMIT waits when two jobs would use the same
window (`cbuf_waits`). After the fix `rknputest m6` passed 3 of 3, and
`m6 --collide` stayed exact.

## M7, decode against Linux, merged 2026-11-24

M6 was at 82.5 ms a token against Linux's 55.1 ms (18.15 tok/s, rocket, CPU
pinned at maximum). The NPU is busy about 34 ms a core a token on both
systems; the gap was NPU idle time, ranked: CPU work slower (decode thread
not pinned, slow pool barrier, CPU at 1608 against 2208 MHz, about 12 ms),
SUBMIT IOCTLs (about 9 ms), PREP and FINI as one IOCTL each (380 a token,
about 6 ms), interrupt to waiter (1 to 2 ms). M7 fused PREP and FINI into
SUBMIT and WAIT (4 IOCTLs a call instead of 10), pinned the decode thread,
replaced the pool locks, added the CPU hold and used the weight cache.

Board runs of the M7 defaults:

| Date | Conditions | tok/s | of Linux |
|---|---|---|---|
| 2026-11-24 | CPU 1608 MHz | 14.23 | 78% |
| 2026-11-24 | CPU 2208 / 2016 MHz | 15.05 | 83% |
| 2026-11-25 | fan off, thermally throttled | 12.91 | 71% |
| 2026-11-26 | fan on | 16.12 | 89% |

The user-mode cache maintenance probe traps on the TaishanPi 3M
(SCTLR_EL1.UCI is 0), so its arm is an A/A run and shows the bench's noise:
3 to 6 ms a token per arm.

## M8, installer, `charsiu` commands, SUBMIT timing, merged 2026-11-25

The `charsiu` front door, `charsiu serve` over Winsock, the installer and
the package, `UserAccess`, and the per-phase SUBMIT timing with a cached
shadow for the stream check. Merged into main from `charsiu-windows` and
`submit-timing`; checked in CI only. The 2026-11-25 and 2026-11-26 runs
above used the M7 driver, without the M8 SUBMIT changes.
