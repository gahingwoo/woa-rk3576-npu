# Testing

What CI checks, and how the board runs are done. Current on `main` on
2026-11-24. The milestone board tests as they were run are in
[history](../history/README.md).

## In CI, on every push

- `rknputest selftest`: the IOCTL header and the tool, plain C on Linux.
- `rknputest-fake m3` and `m6`: the tool against an in-memory stand-in for
  the driver (`tools/rknputest/fakedev.c`), including two threads with
  overlapping jobs, the CBUF collision case, and a run under the old M3 busy
  rule that must fail.
- `tools/m3gen`: the GEMM register streams and the register allow list,
  regenerated and compared byte for byte with what is checked in.
- The board scripts (`tools/m4`, `m6`, `m7`) must parse.
- Float math against glibc on all 2^32 inputs, and the Linux reference
  build of charsiu (aarch64).
- charsiu for Windows on Arm (llvm-mingw), and `rknpu.sys` with
  `rknputest.exe` (WDK, test-signed). These are the `charsiu-m7-arm64` and
  `rknpu-m7-arm64` artifacts.
- Wording: no en or em dashes, and the project's wording rule for public
  files.

None of that is a hardware result.

## On the board

Board: the TaishanPi 3M under Windows 11 23H2 with test signing, firmware
with the NPU option on, the driver installed as in [use](../use.md),
minidumps on. Each step runs as a SYSTEM scheduled task, so a dropped ssh
session cannot stop it, and writes a zip to collect afterwards.

1. `rknputest info`: binding, version registers, IOMMU and cache mode. Must
   end with `RKNPU INFO: OK`.
2. `rknputest m3` and `rknputest m6`: GEMMs from 1x64x64 to 16x1024x1024 on
   both cores, exact against the CPU, through the IOMMU, both cores at once.
   `m6 --fault` checks that an IOMMU fault stops the job.
3. The benchmark, about 20 minutes:

   ```
   schtasks /create /tn rknpu-m7 /sc once /st 23:59 /ru SYSTEM /f /tr "powershell -NoProfile -ExecutionPolicy Bypass -File C:\rknpu\m7-bench.ps1 -Model R:\npu\Llama-3.2-1B-Instruct-Q8_0.gguf -Out R:\npu\m7-windows"
   schtasks /run /tn rknpu-m7
   ```

   `bench.txt` and `decode.txt` in the zip are the summary. Every NPU arm
   must print the same text (`TEXT: all N short-npu arms print the same
   text`); anything else is a failure. The arms and what each switch costs
   are in [performance](performance.md). `-IdleDisable` and `-Quick` are
   the optional controls.

The Linux reference (text and tokens a second under rocket, the same
board): `tools/m4/linux-ref.sh` as root under Armbian edge with rocket,
booted through the firmware with Armbian's own DTB (the firmware's DTB has
the NPU disabled). Its output is what `-Linux` compares against.
`tools/m0` is the earlier Linux measurement kit ([its README](../../tools/m0/README.md)).

## Backing out

Every port change has an environment switch, and the `short-m6` arm runs
them all off at once. To go back to an earlier driver, stage that package
and repeat the reinstall in [use](../use.md).
