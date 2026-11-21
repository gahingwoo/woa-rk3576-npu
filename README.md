# woa-rk3576-npu

[简体中文](README.zh-CN.md)

The Rockchip RK3576 NPU on Windows on Arm: a KMDF driver and a port of the
[charsiu](https://github.com/gahingwoo/charsiu) LLM runtime, so that models
that run on the NPU under Linux run on it under Windows too.

Status: nothing here has run on hardware yet. The feasibility study is
[docs/FEASIBILITY.md](docs/FEASIBILITY.md). The M0 measurement kit, to run
under Linux on a board, is in [tools/m0](tools/m0/README.md). The M2 driver
skeleton ([drivers/rknpu](drivers/rknpu)), its test tool
([tools/rknputest](tools/rknputest)) and the IOCTL header
([include/rknpu_ioctl.h](include/rknpu_ioctl.h)) build in CI; the board test
is [docs/M2.md](docs/M2.md).

## The approach

- **Driver.** A KMDF driver with the same job model as mainline Linux's
  `rocket` driver and the RK3576 support in
  [linux-rk3576-npu](https://github.com/gahingwoo/linux-rk3576-npu):
  buffer allocation, cache maintenance, job submission and completion
  interrupts. It follows the pattern of the VDPU383 test driver in
  [woa-rk3576](https://github.com/gahingwoo/woa-rk3576): DMA buffers below
  4 GiB, the NPU IOMMU in bypass at first, an administrator-only device
  interface and a test tool.
- **Firmware.** [edk2-rk3576](https://github.com/gahingwoo/edk2-rk3576)
  powers the NPU, sets its rail and clock, and describes it in ACPI, behind a
  setup option that is off by default.
- **Runtime.** charsiu talks to the kernel through one file (`src/device.c`).
  The port replaces that file and the few POSIX-only pieces, and builds with
  clang.
- **Not planned:** an MCDM (compute-only WDDM) driver for DirectML or Windows
  ML. On Windows 11 23H2, the newest release the RK3576 can run (it is
  ARMv8.0), MCDM needs a D3D12 compute driver that compiles shaders, and this
  NPU is fixed-function. See section 6 of the study.

## Milestones

| # | What | Exit criterion | Status |
|---|---|---|---|
| M0 | Linux-side measurement | one NPU job with IOMMU paging off gives the CPU-exact result | kit written ([tools/m0](tools/m0/README.md)), not run |
| M1 | Firmware power, clocks, ACPI | the device appears in ACPI with its version register readable | written (edk2-rk3576 branch `npu-acpi`), builds, not run |
| M2 | Driver skeleton and test tool | the device starts with no problem code | written, CI builds and test-signs it, not run ([docs/M2.md](docs/M2.md)) |
| M3 | Buffers, submission, interrupts | GEMM results exact against the CPU | not started |
| M4 | charsiu port | Llama-3.2-1B gives the same text as on Linux | not started |
| M5 | Performance | tokens per second within a stated margin of Linux | not started |
| M6 | Driver-managed NPU IOMMU | M4 again with scattered and high memory | not started |
| M7 | Install script, `charsiu serve` | answers from a second machine | not started |

Estimates and risks are in the study.

## Boards

The LCKFB TaishanPi 3M and the ArmSoM CM5-IO, the boards
[woa-rk3576](https://github.com/gahingwoo/woa-rk3576) supports, under
Windows 11 23H2 with edk2-rk3576 firmware.

## License

GPL-2.0-only ([LICENSE](LICENSE)), the same as charsiu and the Linux driver
this follows.
