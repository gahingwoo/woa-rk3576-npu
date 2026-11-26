# Testing

What CI checks, and how a board run is done.

## In CI, on every push

- `rknputest selftest`: the IOCTL header and the tool, plain C on Linux.
- `rknputest-fake m3`, `m6` and `submit-stats`: the tool against an
  in-memory stand-in for the driver (`tools/rknputest/fakedev.c`), including
  two threads with overlapping jobs, the CBUF collision case, and a run
  under the old M3 busy rule that must fail.
- `tools/m3gen`: the GEMM register streams and the allow list, regenerated
  and compared byte for byte with what is checked in.
- The board scripts (`tools/m4`, `m6`, `m7`) must parse.
- Float math against glibc on all 2^32 inputs, and the Linux reference
  build of charsiu (aarch64).
- charsiu for Windows on Arm (llvm-mingw), `rknpu.sys` and `rknputest.exe`
  (WDK, test-signed): the `charsiu-m7-arm64`, `rknpu-m7-arm64`,
  `charsiu-windows-arm64` and `rknpu-windows-arm64` artifacts.
- The `charsiu` front door and installer under pwsh on Linux.
- Performance counters: `tools/perfcheck` checks `rknpu.man` against the
  driver's structures; the Windows job checks the names are linked into
  `rknpu.sys`.
- Wording: no en or em dashes, and the project's wording rule for public
  files.

None of that is a hardware result.

## On the board

TaishanPi 3M, Windows 11 23H2 with test signing, the NPU option on, the
driver installed as in [use](../use.md), minidumps on, a fan on the SoC. Run
each step as a SYSTEM scheduled task, so a dropped ssh session cannot stop
it; each writes a zip.

1. `rknputest info`: binding, version registers, IOMMU and cache mode. Ends
   with `RKNPU INFO: OK`.
2. `rknputest m3` and `rknputest m6`: GEMMs from 1x64x64 to 16x1024x1024,
   exact against the CPU, through the IOMMU, both cores at once.
   `m6 --collide` must stay exact; `m6 --fault` checks that an IOMMU fault
   stops the job.
3. The benchmark, about 20 minutes:

   ```
   schtasks /create /tn rknpu-m7 /sc once /st 23:59 /ru SYSTEM /f /tr "powershell -NoProfile -ExecutionPolicy Bypass -File C:\rknpu\m7-bench.ps1 -Model R:\npu\Llama-3.2-1B-Instruct-Q8_0.gguf -Out R:\npu\m7-windows"
   schtasks /run /tn rknpu-m7
   ```

   `bench.txt` and `decode.txt` are the summary. Every NPU arm must print
   the same text (`TEXT: all N short-npu arms print the same text`);
   anything else is a failure. Options: `-Quick` (short-m6, short-npu,
   short-npu2, short-m7submit and the stages arms), `-IdleDisable`,
   `-KeepCache`, `-SkipDriverTests`.

A single arm moves 3 to 6 ms a token for no reason. Read the SUBMIT phase
rows (us a SUBMIT) first and tok/s second; for a decision on tok/s, run
`-Quick` twice.

Performance counters: register them, then watch them with `typeperf` while
`rknputest m6` runs ([perfcounters](perfcounters.md#board-check)).

The Linux reference (text and tok/s under rocket, same board):
`tools/m4/linux-ref.sh` as root under Armbian edge with rocket, booted
through the firmware with Armbian's own DTB (the firmware's DTB has the NPU
disabled). `tools/m4/win-run.ps1 -Linux` on the board, or `compare.sh` on
the desk, compares a Windows run's text with it. m7-bench's Linux figure
(18.15 tok/s) is a constant in the script. `tools/m0` is the
earlier Linux measurement kit ([README](../../tools/m0/README.md)).

## Backing out

Every port change has an environment switch, and the `short-m6` arm turns
them all off at once. To go back to an earlier driver, stage that package and
repeat the reinstall in [use](../use.md).
