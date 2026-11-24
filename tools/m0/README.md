# M0: Linux-side measurement

Two questions, answered on a board under Linux before any Windows code
depends on them:

1. **What state does Linux leave the NPU in when it works?** Power domains,
   clocks, resets, the rail, the IOMMUs and the version registers, read
   after a real NPU job. This is the target state for the firmware's
   `Npu.c` (M1).
2. **Does the NPU compute correctly with its IOMMU not translating?** The
   Windows driver's first phases (M1 to M5) put buffers in physically
   contiguous memory below 4 GiB and leave the NPU IOMMU with paging off.
   Mainline Linux always runs the NPU with paging on, so this has never been
   measured. If it does not work, M6 (the driver owns the IOMMU) has to come
   before M3. See section 7, risk 1, of [the study](../../docs/history/2026-11-22-feasibility.md).

Nothing here has run on a board yet.

## Files

| File | What it does | Writes |
|---|---|---|
| `m0_snapshot.sh` | runs one NPU job through rocket, holds the NPU domains on, reads everything, writes a tarball | only the two runtime PM `control` files, put back on exit |
| `m0_peek.c` | reads 32-bit registers through `/dev/mem` | nothing: opened read only, mapped `PROT_READ` |
| `m0_bypass.c` | one GEMM on NPU core 0 with the IOMMU not translating, compared with the CPU | NPU core 0 job registers only; see below |
| `Makefile` | static aarch64 build of the two programs | |

Every register address is cited in the source next to where it is used:
`pm-domains.c` (`rk3576_pmu`, `rk3576_pm_domains[]`), `clk-rk3576.c` and
`clk.h`, `rst-rk3576.c`, `rocket_registers.h`, `rockchip-iommu.c`, and the
NPU nodes the RK3576 NPU series adds to `rk3576.dtsi`
([linux-rk3576-npu](https://github.com/gahingwoo/linux-rk3576-npu)
`rfc-send-v17/npu`, patch 11). The NPU nodes are not in mainline yet.

## What m0_bypass writes, and why it is safe

All through `/dev/mem`, all on NPU core 0:

- PC `0x27700000`: `BASE_ADDRESS`, `REGISTER_AMOUNTS`, `INTERRUPT_MASK`
  (0, so no interrupt is raised), `INTERRUPT_CLEAR`, `TASK_CON`,
  `TASK_DMA_BASE_ADDR`, `OPERATION_ENABLE`.
- CNA `S_POINTER` (`0x27701004`) and CORE `S_POINTER` (`0x27703004`).

These are the registers rocket writes on every submit
(`rocket_job_hw_submit()`) and on every completion, with the same values
apart from the interrupt mask. rocket's next job overwrites all of them, so
there is nothing to restore. With the interrupt masked, rocket's interrupt
handler never sees this job.

Only with `--allow-mmu-writes`, and only if paging is on at the start: the
`COMMAND` register of the two core 0 IOMMU instances (`0x27702008`,
`0x27702108`) to stall, switch paging off and unstall, and afterwards
`DTE_ADDR` and `INT_MASK` written back with the values read before, a zap,
paging back on, and both read back. Use the boot option below instead and
the IOMMU is never written.

It never writes the PMU, the CRU, a GRF, a QoS register, the PMIC, core 1,
or `GLOBAL_OPERATION_ENABLE`. No clock rate, voltage or reset changes.

The NPU only writes where its register stream says. The stream is built by
charsiu's own emitter with the physical addresses of pages this process has
pinned. After the job the tool checks that nothing outside the expected
output changed (a canary pattern over the rest of the 2 MiB page). Before it
touches anything it refuses unless core 0's domains are on, its runtime PM is
pinned, no process holds `/dev/accel/accel*` open, and core 0 is idle. A
register read with the domain off is an SError on this SoC, and that check
is what prevents it.

It prints `REBOOT` if a job did not finish, the IOMMU did not take its old
state back, or the canary changed.

Running it safely needs no kernel module. It does need three boot options
for one session, all removed afterwards.

## Steps

On the desk (aarch64, so the build is native):

```sh
cd tools/m0
make CHARSIU=~/Desktop/charsiu
scp -O m0_peek m0_bypass m0_snapshot.sh root@BOARD:/root/m0/
```

On the board, under Armbian with the NPU working and charsiu's probe tools
installed (`charsiu_matmul` and `charsiu_probe`, on `PATH` or in
`/opt/charsiu`). One thing on the NPU at a time: nothing
else may be using it.

1. **Boot options for this session.** Add to `extraargs=` in
   `/boot/armbianEnv.txt` (or to the kernel command line of whatever boots
   the board), then reboot:

   ```
   iomem=relaxed iommu.passthrough=1 mem=2G
   ```

   - `iomem=relaxed`: rocket owns the NPU's registers, and without this
     `/dev/mem` refuses to map them.
   - `iommu.passthrough=1`: between rocket jobs the NPU IOMMU then sits in
     identity, paging off, so `m0_bypass` never writes it. rocket still
     attaches its own paging domain for each of its jobs.
   - `mem=2G`: Linux uses only the first 2 GiB of RAM (0x40000000 to
     0xC0000000), so every page this test gets is below 4 GiB.

   Check: `cat /proc/cmdline` shows all three.

2. **Huge pages** (the buffers need one physically contiguous 2 MiB page):

   ```sh
   echo 16 > /proc/sys/vm/nr_hugepages
   grep HugePages_Free /proc/meminfo     # expect 16
   ```

3. **Snapshot:**

   ```sh
   cd /root/m0
   ./m0_snapshot.sh 2>&1 | tee snapshot.log
   ```

   Expected: `running: charsiu_matmul 1 256 64` (one m = 1 int8 matmul
   through rocket, checked against the CPU; not `npu_gemm_test`, which exits
   1 whenever m > 1 is not exact), then `done, send
   ./m0-<host>-<time>.tar.gz`. It stops before reading anything if the job
   fails, if the domains are not all on within 5 s, or if an NPU clock is
   not enabled. If `NOTE-iomem.txt` appears, step 1 did not take.

4. **Bypass:**

   ```sh
   echo on > /sys/bus/platform/devices/27700000.npu/power/control
   ./m0_bypass --dry-run 2>&1 | tee bypass-dry.txt
   ./m0_bypass 2>&1 | tee bypass-1.txt
   ./m0_bypass 2>&1 | tee bypass-2.txt
   ./m0_bypass 2>&1 | tee bypass-3.txt
   echo auto > /sys/bus/platform/devices/27700000.npu/power/control
   ```

   The dry run reads the state, builds the job and prints every register
   write it would make, without making any. Expected from a real run:

   ```
     IOMMU 0x27702000: STATUS 0x... DTE_ADDR 0x... INT_MASK 0x..., paging off
     IOMMU 0x27702100: ... paging off
     huge page at PA 0x...
     PA: weights 0x... input 0x... output 0x... coef 0x... regcmd 0x...
     regcmd: 143 entries
     submit:
       write 0x27700010 = 0x00000001
       ...
     done after ... us: RAW_STATUS 0x00000100 ...
     result: 0 of 64 wrong, worst delta 0
     canary: 0 bytes outside the output changed
   M0 BYPASS PASS
   ```

   Exit status: 0 pass, 3 wrong numbers, 4 reboot needed, 1 refused (a
   precondition, not a result).

5. **Post-check** (rocket still works after the raw jobs):

   ```sh
   charsiu_matmul 1 256 64 2>&1 | tee post.txt
   charsiu_probe 2>&1 | tee -a post.txt
   ```

   If either fails, reboot before anything else and say so.

6. Remove the three boot options and reboot.

If `iommu.passthrough=1` cannot be used, leave it out and run `m0_bypass
--allow-mmu-writes` instead. The output then shows paging `ON` at the start
and the IOMMU writes around the job.

## What to send back

- the `m0-*.tar.gz` from step 3 and `snapshot.log`
- `bypass-dry.txt`, `bypass-1.txt` to `bypass-3.txt`
- `post.txt`
- `/proc/cmdline` and the board name

## How the result decides the plan

Three runs in one boot. One pass is one sample; say how many.

| Result | Reading | Plan |
|---|---|---|
| 3 of 3 `PASS`, canary 0, post-check passes | the NPU reads and writes physical memory correctly with paging off | M1 to M5 as planned; M6 stays after M5 |
| `done`, numbers wrong, canary 0, the same wrong numbers every run | the NPU reaches memory but not the right bytes: cache maintenance, address width or layout | look at the dry-run addresses and the snapshot first; if it stands, M6 moves before M3 |
| wrong numbers that differ between runs | not settled | more runs before any decision |
| `NOT DONE`, or a DMA read or write error | the NPU cannot work through an IOMMU that is not translating | M6 moves before M3, estimate +3 to 5 person-weeks |
| canary changed | the NPU wrote outside its buffers | stop, reboot; M6 before M3 |
| refused | a precondition failed; nothing was measured | fix the precondition and rerun |

The snapshot is the reference for M1 either way: the firmware's serial log
must show the same `CLKSEL_CON86`, gate, reset and `REPAIR0` bits, and a
rail at 800 mV.
