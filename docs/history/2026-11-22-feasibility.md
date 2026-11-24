# RK3576 NPU on Windows on Arm: feasibility study

Written 2026-11-22, desk only. No board was touched, nothing was built, and
nothing in this document has run on hardware. Every figure from the Linux side
is quoted from the NPU project's own logs and notes; every Windows-side claim
is marked as read from Microsoft documentation, read from our own repos, or
guessed.

Inputs read: [linux-rk3576-npu](https://github.com/gahingwoo/linux-rk3576-npu) (README, `rfc-send-v17/`), the mainline
`rocket` driver (`drivers/accel/rocket`, linux-next),
[charsiu](https://github.com/gahingwoo/charsiu) (runtime), the vendor `rknpu` kernel driver (GPL-2.0, carried in
[kiln](https://github.com/gahingwoo/kiln)), public reference trees, the firmware in
[edk2-rk3576](https://github.com/gahingwoo/edk2-rk3576) (`main` and branch
`vdpu383-acpi`), [woa-rk3576](https://github.com/gahingwoo/woa-rk3576) (`drivers/video/vdpu383`), and
[wddm-mali-g52](https://github.com/gahingwoo/wddm-mali-g52) `docs/WDDM2.md` (branch `wddm2`).

## 1. Verdict

**Feasible, and shape A goes first.** A KMDF driver that reproduces the rocket
uAPI's semantics, plus charsiu ported with one file replaced, should get
Llama-3.2-1B running on the NPU under Windows 11 23H2. No Microsoft AI stack is
needed or usable.

Why it is tractable:

1. **The kernel side of rocket is small.** rocket is a register-stream
   submitter. It owns no operator set. Per task, the hardware sees about 15
   register writes (`rocket_job_hw_submit()`, rocket_job.c:134-183). It also
   has an IRQ handler that chains the next task, and a reset. The RK3576 delta
   in v17 is 44 lines (`v17-0009`). Everything clever lives in userspace.
2. **charsiu already has a one-file OS boundary.** `src/device.c` (481 lines)
   is the only code that talks to the kernel: four ioctls plus GEM_CLOSE,
   through the API in `include/charsiu.h:591-650`. The other ~37,000 lines are
   C11 with NEON intrinsics and no GCC extensions (no `__attribute__`,
   `__builtin_*`, `typeof` or statement expressions in `src/`).
3. **There is a precedent.** `woa-rk3576/drivers/video/vdpu383` is the pattern
   already: ACPI device, admin-only `\\.\` interface, contiguous buffers below
   4 GiB, IOMMU in bypass, a test tool, and a firmware setup option that powers
   the block. The NPU driver is the same shape with a different job format.

What is *not* feasible on this board:

- **Shape B (MCDM so that DirectML / Windows ML / ONNX Runtime-DirectML use
  the NPU) is not realistic on 23H2.** Section 6 gives the reasons. In short:
  - A compute-only device exposes D3D12 *Core 1.0*, and that means compiling
    Shader Model 6.x DXIL compute shaders for the device. This NPU has no
    shader core.
  - Windows ML's NPU execution providers need 24H2 (build 26100).
  - The Task Manager NPU columns are documented only for 24H2 and later.
- Copilot+ / Windows Studio Effects: need 24H2 and 40+ TOPS. Out of scope
  forever on RK3576, which is roughly 6 TOPS int8.

**User value, honestly:** the user gets an OpenAI-compatible local LLM server
on a Windows RK3576 board, at whatever fraction of the Linux 17.85-18.60 tok/s
survives the Windows submit path. The Linux side is already the better place
to run it. The Windows port proves the open stack is OS-independent, and it
gives the woa-rk3576 driver set its first accelerator that apps can use.

One more point bears on value. **Windows 11 23H2 left servicing for every edition
on 2026-11-10** (Home/Pro on 2025-11-11; I recall these dates and have not
re-checked them). The only Windows that runs on this SoC is now out of support.
It is still the deployable target, but this weighs against large investments
like shape B.

## 2. Architecture sketch (shape A)

```
  charsiu (unchanged)          llama.c, npudev.c, regcmd.c, job.c, npuquant.c ...
        |   charsiu.h device API (charsiu_open / bo_alloc / bo_prep / bo_fini / submit)
  device_win.c   (new, replaces device.c)  + plat_win.c (mmap, threads, affinity, clock)
        |   DeviceIoControl on \\.\RKNPU    (+ a shared read-only status page)
  rknpu.sys (KMDF, new, GPL-2.0-only)
     - ACPI\RKCP3576-NPU device: MMIO 0x27700000/64 KiB, GSIV 279 (core 0), 280 (core 1)
     - BO table per file object: contiguous <4 GiB (M1) or IOMMU-mapped (M6)
     - per-core software queue; ISR chains tasks; DPC completes jobs
     - cache maintenance by range (armasm64 DC CVAC/CIVAC stub)
     - job timeout -> core soft reset through an ACPI _DSM
        |
  firmware (edk2-rk3576, setup option, default off)
     - RK806 BUCK2 vdd_npu_s0 = 800 mV
     - PD_NPU -> PD_NPUTOP (+15 us settle) -> PD_NPU0 / PD_NPU1
     - CRU: clk_rknn_dsu0 = GPLL/2 = 594 MHz, ungate the 6 per-core clocks
     - SSDT "RKNPU": device, _STA guarded on PD + gate bits, _CCA 0, _DSM reset
```

### 2.1 The interface (rknpu_ioctl.h)

Keep **rocket's semantics one for one**, so that `device_win.c` is a mechanical
translation and charsiu's ordering rules (PREP before CPU access, FINI before
submit) mean the same thing on both OSes:

| IOCTL | rocket equivalent | notes |
|---|---|---|
| `GET_INFO` | `GET_PARAM` (v17 10/12) | hw version (RK3576 = 2), core count, PC version register, IOMMU mode, interface version |
| `CREATE_BO` | `CREATE_BO` + `mmap` | returns handle, device address (u32), user VA mapped in caller context (`EvtIoInCallerContext`, as vdpu383 does), cache mode flag |
| `FREE_BO` | `GEM_CLOSE` | also freed on file cleanup |
| `PREP_BO` / `FINI_BO` | same | range-capable (offset, length); does the D-cache maintenance; PREP waits for the BO's last writer, with a timeout; `timeout 0` = poll |
| `SUBMIT` | `SUBMIT` | same `jobs[] -> tasks[] {regcmd u32, regcmd_count}` + in/out BO handle lists; returns a 64-bit job sequence number |
| `WAIT` | (implicit in PREP) | wait for a sequence number; plus a mapped read-only page holding each core's completed seqno, so the user side can spin before sleeping, like charsiu's past-deadline PREP poll |
| `RESET` / `READ_REGS` | debug only | admin, same as vdpu383 |

The struct layouts should be fixed-size and identical on both sides. Copy
`include/uapi/drm/rocket_accel.h` (MIT) field for field. Do not invent fields
(charsiu's own `rocket_uapi.h` header says the same).

### 2.2 Submission and interrupts

- Port `rocket_job_hw_submit()` verbatim in meaning: BASE_ADDRESS=1, CNA and
  CORE S_POINTER with the `0x10000000 * core` bit, BASE_ADDRESS=regcmd,
  REGISTER_AMOUNTS, INTERRUPT_MASK/CLEAR for DPU_0|DPU_1, the RK3576 16-bit
  TASK_CON layout (`LAST_LAYER_CLEAR|COUNT_CLEAR|PP_EN|TASK_NUMBER(1)`),
  TASK_DMA_BASE_ADDR=0, OPERATION_ENABLE=1.
- ISR (DIRQL): read PC interrupt status, write OPERATION_ENABLE=0 and
  INTERRUPT_CLEAR=0x1ffff under the core's interrupt spinlock (rocket's comment
  about that ordering applies unchanged). If the job has tasks left, program
  the next one **in the ISR**: the writes are cheap, and charsiu's measured
  floor is about 130 us per call. Otherwise queue the DPC.
- DPC: bump the core's completed seqno in the shared page and set the job's
  event. The next queued job starts from the DPC.
- **The core and its IOMMU share one SPI** (247 for core 0, 248 for core 1;
  GSIV +32 = 279/280). One device owns both, and the ISR checks the MMU
  interrupt status of both MMU instances too (only meaningful once M6 enables
  paging).
- Core selection: rocket's drm_sched hands a job to any free core. Do the
  same, and add an optional core-mask hint (the vendor driver has `core_mask`).
  charsiu's int4 decode runs on both cores.
- Never touch `GLOBAL_OPERATION_ENABLE` (0xf008) from core 0's window. The
  project found it hits NPU1 in single-core mode.

### 2.3 Memory model

Two phases.

**M1-M5: IOMMU bypass, physically contiguous, below 4 GiB** (the vdpu383 model).

- The rocket uAPI's addresses are 32 bits (`drm_rocket_task.regcmd` is u32),
  and so are the addresses inside a register stream. With paging off, the NPU
  emits physical addresses, so every BO must lie in 0x40000000-0xFFFFFFFF.
  DRAM starts at 0x40000000, so that is 3 GiB at most and less on a 4 GB board.
- Per-BO `MmAllocateContiguousNodeMemory(..., high = 0xFFFFFFFF)`. charsiu
  already allocates per slice (`npudev.c:3263` weights/coef/regcmd per staged
  slice), so the request sizes are megabytes, not the whole model.
- **Cache attributes**, the one design choice here that affects throughput:
  - weights, coefficients, register streams: written once by the CPU, read by
    the NPU. Allocate them write-combined (Normal non-cacheable), as vdpu383
    does. No maintenance is needed.
  - inputs and outputs: read back by the CPU every call. A72 reads from
    non-cacheable memory are far slower than cached reads, so allocate these
    cached and do the maintenance in PREP/FINI by range.
    - Mechanism: an armasm64 stub doing `DC CVAC` / `DC CIVAC` + `DSB SY` over
      the kernel VA. The rkdwmmc `ArmSmc.asm` stub is the precedent for "the
      WDK has no intrinsic, so write 10 lines of armasm64".
    - The HAL path (DMA adapter, `FlushAdapterBuffersEx`) is the documented
      alternative. A 32-bit adapter may silently insert bounce buffers, so
      measure before choosing it.
  - Never map the same page cached in one place and WC in another
    (mismatched attributes are architecturally unpredictable on ARM64).
- The "burn the first allocation" in charsiu (`device.c:159-174`, IOVA 0)
  becomes moot (physical 0 is not DRAM). Keep it anyway: harmless, and keeps
  the code identical.

**M6: the driver owns the NPU IOMMU** (Rockchip IOMMU v2, `rockchip,rk3568-iommu`
compatible, two instances per core at +0x2000/+0x2100).

- Driver-built two-level tables: DTE page, PTE pages, 4 KiB granules, 32-bit
  IOVA, 40-bit PA (the vendor config sets `dma_mask = DMA_BIT_MASK(40)` for
  RK3576).
- BOs become `MmAllocatePagesForMdlEx` (any pages, also above 4 GiB on 8 GB
  boards). This removes the contiguous-allocation risk and matches what Linux
  does, so charsiu's address assumptions hold exactly.
- Windows does not manage this IOMMU. It is not an SMMU and has no IORT
  entry, so there is no conflict. Program all four instances (2 cores x 2)
  with the same DTE.
- Mind the known Linux issue: after repeated resets the IOMMU does not recover
  (`MMU_DTE_ADDR is not functioning`, v16/v17 covers). Rewrite DTE_ADDR and
  re-enable paging after every core reset, and read back DTE_ADDR before
  trusting it.
- Isolation: start with one address space shared by all handles (admin only
  anyway). Per-process spaces need a DTE switch and a zap on context change.
  Defer that.

### 2.4 Power, clocks, resets

Windows has no clock or power-domain framework, so **the firmware must leave
the NPU powered, clocked and out of reset**, exactly as it does for the GPU
(`RK3576Dxe/Gpu.c`) and the decoder (`Vdec.c`). Everything below is from
mainline and the project's v17 series.

| Item | Value | Source |
|---|---|---|
| Rail | RK806 BUCK2 = vdd_npu_s0. **Set 800 mV.** The current CM5-IO table (`RockchipPlatformLib.c:71-105`) never sets BUCK2, so it is whatever SPL/PMIC defaults leave (the project measured 750 mV from U-Boot). At 750 mV, 786 MHz produced 11-25 wrong words a pass; 800 mV produced none (charsiu `src/overlap.h`, memory `project-charsiu-gemma4-and-m8`) | project, measured |
| Domains | PD_NPU (PWR_CON 0x0 bit 0, needs the rail first), then PD_NPUTOP (0x4 bit 6, status bit 22, idle 0x18, **15 us settle, no QoS access during it**), then PD_NPU0 (0x4 bit 7, bit 23, idle bit 1) and PD_NPU1 (0x4 bit 8, bit 24, idle bit 2) | `pm-domains.c:1334-1351`, v17 pd 2/3 and 3/3 |
| Compute clock | `clk_rknn_dsu0`: CLKSEL_CON(86) mux [8:7] (gpll/cpll/aupll/spll), div [6:2]; gate CLKGATE_CON(31) bit 5. GPLL 1188 / 2 = **594 MHz**. Use the CRU. The SCMI/PVTPLL route is a measured dead end (kernel patch 0019) | `clk-rk3576.c:919-956` |
| Other gates | aclk_rknn0 GATE(28) bit 9, aclk_rknn1 GATE(29) bit 0, hclk_rknn_root GATE(31) bit 4 (mux CLKSEL(86)[1:0]), pclk_nputop_root GATE(31) bit 8 (CLKSEL(87)[1:0]), aclk_rknn_cbuf GATE(32) bit 0, hclk_rknn_cbuf GATE(32) bit 12 | same |
| Resets | SRST_A_RKNN0 / SRST_A_RKNN1 (and the BIU resets); take the bit positions from `rst-rk3576.c` | mind `project_rk3576_reset_id_encoding_trap`: mainline IDs are array indices, not register bits |
| Power management | none in M1-M7: the NPU stays on from firmware. The Linux cold-rail cost is "a few percent on the first work after idle, nothing in steady state" (r380), and the idle power saving is unmeasurable on these boards. Optional later: `_PS0/_PS3` in ASL | |

**Core reset from the driver:** the CRU is shared by every block, and it is not in the
NPU's `_CRS`. Expose `_DSM` functions in the NPU's SSDT that write the soft-reset
register through a `SystemMemory` OperationRegion (assert, `Stall`, deassert),
and optionally one that picks 594 / 786 MHz through CLKSEL_CON(86). That keeps
every CRU write in firmware-described code. The SiP SMC route used by the SD
drivers is the alternative, but it needs BL31 work.

## 3. Reuse map

### 3.1 charsiu ([charsiu](https://github.com/gahingwoo/charsiu), GPL-2.0 / GPL-2.0-or-later)

| Path | Lines | Port |
|---|---|---|
| `src/regcmd.c` | 1791 | **as is**: the register-stream emitter |
| `src/job.c`, `src/npuquant.c`, `src/npufp16.c` | 2132, 1962, 1596 | **as is**: only `fopen` and `clock_gettime` (shim) |
| `src/npudev.c` | 8041 | **as is**: talks only through `charsiu_bo_*` / `charsiu_submit*` |
| `src/npupool.c` | 643 | small: `clock_gettime`; cache dir `XDG_CACHE_HOME`/`HOME` -> `%LOCALAPPDATA%` |
| `src/tokenizer.c`, `src/clip.c`, `src/vision.c`, `src/image.c` | 1369, 841, 1660, 115 | **as is** |
| `src/gguf.c`, `src/whisper.c` | 2013, 1806 | small: `mmap`/`open` -> `CreateFileMapping`/`MapViewOfFile` |
| `src/llama.c` | 9752 | **rework, contained**: pthread pool (lines ~319-870, 2496-2506), `sched_setaffinity`/`pthread_setaffinity_np`, `/sys/.../cpufreq` reads (512-672). Replace with `SetThreadAffinityMask`, and find the A72 cluster through `GetLogicalProcessorInformationEx` EfficiencyClass (MADT: 1 = A72). Or build with llvm-mingw and keep winpthreads, rewriting only affinity and cpufreq |
| `src/device.c` | 481 | **replace** with `device_win.c`. Keep the crash-breadcrumb idea via `SetUnhandledExceptionFilter` |
| `include/rocket_uapi.h` | 82 | replace with `rknpu_ioctl.h` (shared with the driver) |
| `tools/npu_gemm_test.c`, `tools/charsiu_probe.c`, `tools/charsiu_check.c` | | **first Windows test vehicles**: CPU-exact oracles that already exist |
| `tools/charsiu_run.c`, `tools/charsiu_serve.c` | 1595, 666 | run: `/proc/self/status` -> `GetProcessMemoryInfo`; serve: BSD sockets -> Winsock (small) |
| `tools/*.py` (rkllm_*, gptq, ...) | | desk-side, unchanged |

Toolchain: **clang**, not MSVC cl. Either llvm-mingw (`aarch64-w64-mingw32`,
can cross-build from the aarch64 Linux desk) or clang-cl inside the WDK CI.
Clang's `arm_neon.h` is the one charsiu is written against. MSVC's ARM64 NEON
header differs in places, and MSVC has no VLAs. The driver itself stays MSVC/WDK
like the rest of woa-rk3576.

### 3.2 Linux kernel side ([linux-rk3576-npu](https://github.com/gahingwoo/linux-rk3576-npu), mainline rocket)

| Path | Port |
|---|---|
| `drivers/accel/rocket/rocket_registers.h` (GPL-2.0-only OR MIT) | **as is** (only the PC/CNA/CORE offsets and fields used) |
| `rocket_job.c` `rocket_job_hw_submit()`, `rocket_job_handle_irq()`, `rocket_job_next_locked()`, the reset ordering comments | **re-implement in KMDF**, same sequence. These comments are the record of the bugs already paid for; carry them over |
| `rfc-send-v17/npu/v17-0009` (RK3576 support: 16-bit TASK_CON, 6 clocks, 1 reset) | semantics into the driver (TASK_CON) and the firmware (clocks/reset) |
| `rfc-send-v17/pd/v17-000[1-3]` (domain supply, 15 us NPUTOP settle) | **into firmware C** (EDK2 `Npu.c`) |
| `rfc-send-v17/npu/v17-0011` (rk3576.dtsi NPU nodes) | **into ASL** (addresses, SPIs, clock list) |
| `kernel/0029-*` (pd_npu owns vdd_npu_s0) | n/a: firmware sets the rail once |
| `rocket_gem.c` (drm_gem_shmem, dma_sync), `rocket_drv.c` (DRM accel, file privs), `rocket_core.c` (clk_bulk, reset_control, pm_runtime autosuspend, iommu_group), drm_sched + dma_resv fences, genpd, devfreq | **Linux-specific, not ported**: each is replaced by firmware state, the BO table, a software queue and KEVENTs |
| `drivers/iommu/rockchip-iommu.c` (GPL-2.0) | M6 only: page-table format and command sequence, re-implemented |
| `mesa/`, `mesa-patches/` (Teflon TFLite delegate) | **not ported**: Gallium/DRM winsys. Vision via Teflon is also out of the agreed scope (project memory: "LLM, not YOLO") |

### 3.3 Not reusable

- The vendor userspace (librknnrt, librkllmrt in Rockchip `rknn-llm`,
  used by kiln): closed Linux `.so` files. They cannot run on Windows and must
  not be copied from.
- the vendor `rknpu` kernel driver (carried in [kiln](https://github.com/gahingwoo/kiln)) (vendor kernel driver, GPL-2.0): readable as
  a second opinion on the non-IOMMU path (`rknpu_gem.c:200, 409, 713`) and on
  the RK3576 config (`rknpu_drv.c`: `pc_task_number_bits 16`, `dma_mask 40`,
  `nbuf_phyaddr 0x3fe80000`). Not a code source.
- `ggml-rocket`, `rocket-userspace` (third party, GPL,
  RK3588-validated): references for shape C (llama.cpp backend), not for A.

## 4. Firmware / ACPI changes

All in edk2-rk3576, following the `vdpu383-acpi` branch one for one (commit
`77cdcdd` is the template: 16 files, 539 lines):

1. `Silicon/Rockchip/RK3576/Drivers/RK3576Dxe/Npu.c` (+ `.h`): the rail, the
   domain sequence with the 15 us NPUTOP settle, the clocks, the reset release.
   Read the PLLs back, as `Vdec.c` does. Log the PC version register and both
   MMU status registers. Tolerate "already on": with the NPU disabled in Linux
   DT, U-Boot was seen leaving NPUTOP/NPU0/NPU1 on, and our chain may differ.
2. Rail: the per-board `RockchipPlatformLib.c` gets
   `RK8XX_VOLTAGE_INIT (MASTER_BUCK2, 800000)`, written only when the option is
   on (CM5-IO, TaishanPi 3M, ROCK 4D). Check each board's schematic for BUCK2
   first; the CM5-IO comment says BUCK2 = vdd_npu_s0.
3. `Silicon/Rockchip/RK3576/AcpiTables/Npu.asl`: an SSDT, OEM table ID `RKNPU`,
   `_HID` in the RKCP namespace (proposal: `RKCP3576` or `RKCPA576`; check
   against existing IDs), `_CCA 0`.
   - `_CRS`: `Memory32Fixed 0x27700000, 0x10000` (both cores: pc/cna/mmu/core
     at +0x0000..0x3FFF and +0x8000..0xBFFF), interrupts 279 and 280, level,
     active high. Optional: the 1 MiB NBUF SRAM at 0x3FE80000, unused by rocket
     today.
   - `_STA`: on only when PMU_REPAIR0 bits 22-24 are set and the NPU gates are
     open. Same guard as Vdec.asl, because a register access to a powered-down
     domain is an SError on this SoC.
   - `_DSM`: core soft reset (n = 0/1), optionally the compute-clock select.
4. `RK3576AcpiPlatformDxe.c`: skip the `RKNPU` SSDT unless `PcdAcpiNpu` is set
   (copy of the `PcdAcpiVdec` code at lines 435-463).
5. HII option "NPU (RKNN, experimental)", default **off**: with it off,
   nothing in the boot changes for Linux or Windows.
6. Linux impact with the option on: genpd finds the domains on and takes over,
   the same situation as GPU and VDEC. Check once that rocket's probe and the
   v17 pd 1/3 "switch off unused subdomains" path behave with a pre-powered
   NPU.

## 5. Milestones (person-weeks, one person, current CI and board rig)

| # | Deliverable | Est. | Exit criterion |
|---|---|---|---|
| M0 | Linux-side capture + bypass answer (section 8) | 0.5 | register snapshot of a working inference; one raw NPU job with paging off produces the CPU-exact result |
| M1 | firmware `Npu.c` + SSDT + option | 1-1.5 | serial log shows rail, domains, clocks, version register; `acpiview` lists `RKNPU`; Linux still passes `charsiu_probe` with the option on |
| M2 | KMDF skeleton: MMIO, both interrupts, GET_INFO, READ_REGS, test tool `rknputest info` | 1 | device starts with no problem code; version register matches firmware |
| M3 | BO alloc (WC + cached), PREP/FINI maintenance, SUBMIT, ISR chaining, WAIT, timeout + `_DSM` reset, file cleanup | 2-3 | `npu_gemm_test` ported as `rknputest gemm`: m = 1 control exact against the CPU, then m = 2..32 |
| M4 | charsiu port: `device_win.c`, `plat_win.c`, clang build in CI | 2-3 | `charsiu_probe` 1024/1024 exact; Llama-3.2-1B int4, **same text as Linux** on the same prompt (hash the text, not the timings) |
| M5 | performance: shared seqno page, spin-then-sleep, A72 pinning, range-only maintenance | 2-4 | tok/s within a stated margin of Linux at 594 MHz, with repeats, on one boot; never compared across sessions without measuring the drift |
| M6 | driver-owned NPU IOMMU | 3-5 | the M4 text again, with BOs from scattered pages, some above 4 GiB on an 8 GB board |
| M7 | user-facing: `charsiu serve` on Winsock, install script, README | 1-2 | OpenAI-compatible endpoint answers from a second machine |
| | **Total to a usable demo (M0-M5, M7)** | **10-15** | |
| | **With the IOMMU (M6)** | **13-20** | |
| C1 | llama.cpp backend over the same device lib (ggml-rocket as reference) | 4-8 | after M6 |
| C2 | ONNX Runtime custom EP (MatMul/Gemm/MatMulNBits on NPU, rest CPU EP) | 6-12 | after M6; only worth it if a concrete ORT consumer exists |
| B | MCDM KMD, no D3D12 UMD (Task Manager entry, VidSch, TDR) | 10-20 | **not recommended** (section 6) |

## 6. Shape B (MCDM) in detail: what Microsoft requires

Read from Microsoft Learn ("Microsoft Compute Driver Model Overview",
"The Direct3D 12 Core 1.0 Feature Level", Windows ML "supported execution
providers") on 2026-11-22:

- MCDM arrived with WDDM 2.6 (Windows 10 1903). The KMD is a WDDM 2.x
  render-only miniport (dxgkrnl, DxgkDdi*), not KMDF.
  - It must have an MMU (GpuMmu or IoMmu) and may not require physical mode.
  - Exception since 2004: "prototype MCDM devices" without an MMU, usable by
    one process at a time.
  - So a bypass-mode NPU could be at best a prototype MCDM device. M6's
    driver-owned IOMMU could in principle be described to VidMm as a GpuMmu
    (32-bit VA, 2 levels). That is the same class of work, with the same
    open questions, as wddm-mali-g52's option (b), 16-26 pw.
- The user-mode side of a compute-only device is **D3D12 at
  `D3D_FEATURE_LEVEL_1_0_CORE`**:
  - "A Core device supports Shader Model 5.0+ ... the driver need only
    support 6.x", and "Compute shaders only".
  - The UMD has to compile DXIL compute shaders to the device's ISA. The NPU
    has no ISA in that sense: it is a fixed-function CNA/CORE/DPU pipeline
    driven by register streams. A DXIL-to-NPU compiler is not a thing that
    can be built.
  - Metacommands (`CreateMetaCommand` / `ExecuteMetaCommand`) are in the Core
    API list, and that is how a fixed-function engine could expose a GEMM.
  - DirectML, however, falls back to HLSL compute shaders for every operator
    without a metacommand. A Core device that cannot run shaders breaks
    DirectML on the first such operator.
  - DirectML's NPU path (developer preview 2024, Intel only) is an IHV
    partnership interface. I found no public spec for it.
  - DirectML itself has been in sustained engineering since Windows ML
    (2025).
- Windows ML: "Hardware-optimized execution providers for NPUs ... require
  Windows 11 24H2 (build 26100) or later". Closed on 23H2.
- Task Manager NPU columns: documented for 24H2/25H2/26H1 builds in 2026; I
  found no evidence that they reached 23H2. On 23H2 an MCDM adapter would
  probably show as a GPU entry, if at all (unverified).
- What B would buy on 23H2: a Task Manager entry, dxgkrnl scheduling and TDR,
  and nothing any application uses. Our own runtime would still need a private
  path (D3DKMT escapes) to submit.
- If the user ever moves to an ARMv8.2 SoC (RK3588) and 24H2: Windows ML
  exposes ONNX Runtime EPs, and an IHV can ship its own EP **without** MCDM.
  That would be shape C, packaged for Windows ML. B still does not follow.

## 7. Risks and unknowns

Ordered by how much they could cost.

1. **IOMMU bypass is unmeasured for the NPU.** Mainline always runs it with
   paging on.
   - For: the vendor driver carries a non-IOMMU path (`rknpu_gem.c`), and
     vdpu383 assumes bypass too (also unmeasured).
   - Against: nobody has run this NPU with paging off on RK3576.
   - M0 answers it before any Windows work.
2. **Contiguous memory below 4 GiB under Windows.**
   - Llama-3.2-1B at int4 stages roughly 0.7-0.9 GB of weights in
     megabyte-sized contiguous chunks. charsiu also maps the gguf, so the
     memory peak is close to double that.
   - On a 4 GB board, Windows plus the desktop already sit in the same low
     ~2.75 GiB. Allocation can fail late, after fragmentation.
   - Mitigations: allocate at model load, fail loudly, and keep M6 on the plan.
     M6 is the real fix, not an optimisation.
3. **Cache maintenance correctness.** A missed maintenance step does not fail.
   It returns stale data (charsiu's `device.c` header says so). The oracles
   (`charsiu_probe` 1024/1024, `npu_gemm_test`) must run on Windows before any
   model does.
4. **Submit latency.** charsiu's measured per-call floor on Linux is about
   130 us x 150 calls = 38% of a token (memory `project-charsiu-call-floor`).
   - Every extra microsecond of IOCTL + ISR/DPC + wake shows up directly in
     tok/s.
   - Windows timer resolution must not be in the path: no `Sleep()`-based
     polling.
   - Unknown until M5 measures it.
5. **Rail and clock.** If firmware leaves vdd_npu_s0 at 750 mV, results can
   be silently wrong at 786 MHz. Stay at 594 MHz until the rail is confirmed
   800 mV from the serial log and an RK806 read-back.
6. **Power-domain sequencing.** Touching QoS within 15 us of NPUTOP power-on
   is an async SError (v17 pd 3/3, 536 clean cold power-ons with the delay).
   The firmware must copy the order exactly.
7. **Reset recovery.** On Linux, the IOMMU did not recover after repeated
   resets (2 ms job timeouts). In M1-M5 a reset only touches the core, because
   paging is off. In M6 it re-arms the IOMMU and checks DTE_ADDR.
8. **Security.** Bypass + user-supplied register streams = arbitrary DMA
   anywhere in the low 4 GiB, kernel memory included. The device interface is
   admin-only (`D:P(A;;GA;;;SY)(A;;GA;;;BA)`, as vdpu383), test-signed, and
   never presented as a general driver. M6 narrows this to "anything the
   driver mapped".
9. **Scheduling.** Decode is bimodal with threads on A72 versus A53 (memory
   `feedback-the-scheduling-lottery`). Windows' heterogeneous scheduler must
   be overridden by explicit affinity, or every Windows number is a lottery.
10. **Measurement on Windows.** Windows Update, Defender and the indexer make
    background noise (and the board updates itself; memory
    `project_cm5io_board_self_updated`). The repeat-and-spread discipline from
    the Linux work applies, and Linux-versus-Windows numbers are cross-session
    by construction.
11. **Out-of-support OS.** 23H2 is past end of servicing (section 1). Test
    signing is unaffected.

**Licensing** (no blocker found; not legal advice):

- woa-rk3576 new code is GPL-2.0-only. charsiu is GPL-2.0 and
  GPL-2.0-or-later, both compatible with a GPL-2.0-only combined work.
- `rocket_registers.h` is GPL-2.0-only OR MIT, the rocket uAPI header is MIT,
  and rocket_job.c and rockchip-iommu.c are GPL-2.0-only. Re-implementing
  their logic in a GPL-2.0-only driver is consistent.
- The firmware parts are BSD-2-Clause-Patent like the rest of edk2-rk3576.
  They contain register facts and sequences only, as `Gpu.c` and `Vdec.c`
  already do.
- The NPU repo's `LICENSE` is GPL-2.0. The vendor userspace is not used. The
  register knowledge in charsiu (`src/regcmd.c`) is the project's own,
  established on hardware.
- Model weights keep their own licences (e.g. the Llama 3.2 Community
  License) and are not redistributed in the driver package.

## 8. First concrete step

**M0, a Linux-side measurement, before any Windows code.** It follows the
standing rule: when Linux drives the peripheral fine, capture its working
state first.

1. On a Linux boot that has just run `charsiu_probe` successfully, snapshot
   read-only:
   - PMU PWR_CON/STATUS/REPAIR bits for PD_NPU/NPUTOP/NPU0/NPU1;
   - CRU CLKSEL_CON(86)/(87), CLKGATE_CON(28/29/31/32) and the RKNN soft-reset
     registers;
   - the RK806 BUCK2 voltage;
   - both cores' PC version register;
   - all four MMU status registers.

   **Pin `power/control=on` first, and do not read anything if the domain-on
   wait timed out.** That rule comes from the 2026-11-01 panic (memory
   `feedback-a-wait-that-times-out-must-not-fall-through`). This snapshot is
   the target state for `Npu.c`.
2. In the same session, with rocket unbound from core 0, run one known GEMM
   with **paging disabled**:
   - buffers from a contiguous allocation below 4 GiB (a CMA-backed udmabuf);
   - the register stream from `charsiu`'s emitter, physical addresses patched
     in, submitted with the `rocket_job_hw_submit()` sequence through
     `/dev/mem` (the `wddm-mali-g52/tools/m1/m1_raw.c` approach);
   - compare against the CPU-exact result.

   This settles risk 1. If it fails, M6 (driver-owned IOMMU) moves ahead of
   M3 and the estimate grows by 3-5 pw.

Desk work that can run in parallel, and needs no board: write `Npu.c`,
`Npu.asl`, and `rknpu_ioctl.h`. The header should be shared by the driver,
`device_win.c` and a Linux build of the test tool (vdpu383's CI already builds
its tool on Linux with gcc). All of this is the user's call to flash and run.
