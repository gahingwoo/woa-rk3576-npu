# M0: Linux-side measurement

Two questions, answered under Linux before the Windows driver depended on
them:

1. **What state does Linux leave the NPU in when it works?** Power domains,
   clocks, resets, the rail, the IOMMUs and the version registers, read
   after a real NPU job: the target state for the firmware's `Npu.c`.
2. **Does the NPU compute correctly with its IOMMU not translating?**

Result, TaishanPi 3M under Armbian edge with rocket, 2026-11-22: snapshot
taken, and `m0_bypass` 3 of 3 `M0 BYPASS PASS` (0 of 64 wrong, canary
unchanged). See [history](../../docs/history.md).

## Files

| File | What it does | Writes |
|---|---|---|
| `m0_snapshot.sh` | runs one NPU job through rocket, holds the NPU domains on, reads everything, writes a tarball | only the two runtime PM `control` files, put back on exit |
| `m0_peek.c` | reads 32-bit registers through `/dev/mem` | nothing (read only, `PROT_READ`) |
| `m0_bypass.c` | one GEMM on NPU core 0 with the IOMMU not translating, compared with the CPU | NPU core 0 job registers only |
| `Makefile` | static aarch64 build of the two programs | |

Every register address is cited in the source: `pm-domains.c`,
`clk-rk3576.c`, `clk.h`, `rst-rk3576.c`, `rocket_registers.h`,
`rockchip-iommu.c`, and the NPU nodes of
[linux-rk3576-npu](https://github.com/gahingwoo/linux-rk3576-npu)
`rfc-send-v17/npu`, patch 11 (not in mainline yet).

## Safety

`m0_bypass` writes, through `/dev/mem`, only the registers rocket writes on
every submit on core 0 (PC `BASE_ADDRESS`, `REGISTER_AMOUNTS`,
`INTERRUPT_MASK` = 0, `INTERRUPT_CLEAR`, `TASK_CON`, `TASK_DMA_BASE_ADDR`,
`OPERATION_ENABLE`, and the CNA and CORE `S_POINTER`). rocket's next job
overwrites them all. It never writes the PMU, CRU, a GRF, QoS, the PMIC,
core 1 or `GLOBAL_OPERATION_ENABLE`. The IOMMU is written only with
`--allow-mmu-writes` (stall, paging off, then restored and read back).

The stream comes from charsiu's emitter with the physical addresses of pages
the process pinned, and a canary over the rest of the 2 MiB page checks that
nothing else changed. It refuses to start unless core 0's domains are on, its
runtime PM is pinned, nothing holds `/dev/accel/accel*` and core 0 is idle (a
register read with the domain off is an SError). It prints `REBOOT` if a job
did not finish, the IOMMU did not return to its old state, or the canary
changed.

## Steps

On an aarch64 desk:

```sh
cd tools/m0
make CHARSIU=~/Desktop/charsiu
scp -O m0_peek m0_bypass m0_snapshot.sh root@BOARD:/root/m0/
```

On the board, Armbian with the NPU working and `charsiu_matmul` and
`charsiu_probe` installed, nothing else using the NPU:

1. Add `iomem=relaxed iommu.passthrough=1 mem=2G` to `extraargs=` in
   `/boot/armbianEnv.txt` and reboot. `iomem=relaxed` lets `/dev/mem` map
   rocket's registers; `iommu.passthrough=1` leaves the NPU IOMMU in
   identity between rocket jobs, so `m0_bypass` never writes it; `mem=2G`
   keeps every page below 4 GiB. Check `/proc/cmdline`.
2. Huge pages: `echo 16 > /proc/sys/vm/nr_hugepages`, and
   `grep HugePages_Free /proc/meminfo` shows 16.
3. Snapshot: `./m0_snapshot.sh 2>&1 | tee snapshot.log`. Expect
   `running: charsiu_matmul 1 256 64`, then `done, send
   ./m0-<host>-<time>.tar.gz`. `NOTE-iomem.txt` means step 1 did not take.
4. Bypass:

   ```sh
   echo on > /sys/bus/platform/devices/27700000.npu/power/control
   ./m0_bypass --dry-run 2>&1 | tee bypass-dry.txt
   ./m0_bypass 2>&1 | tee bypass-1.txt
   ./m0_bypass 2>&1 | tee bypass-2.txt
   ./m0_bypass 2>&1 | tee bypass-3.txt
   echo auto > /sys/bus/platform/devices/27700000.npu/power/control
   ```

   A pass ends with `result: 0 of 64 wrong`, `canary: 0 bytes outside the
   output changed`, `M0 BYPASS PASS`. Exit status: 0 pass, 3 wrong numbers,
   4 reboot needed, 1 refused (a precondition, not a result).
5. Post-check: `charsiu_matmul 1 256 64` and `charsiu_probe` still pass; if
   not, reboot first.
6. Remove the three boot options and reboot.

Without `iommu.passthrough=1`, run `m0_bypass --allow-mmu-writes` instead.

Keep: the `m0-*.tar.gz`, `snapshot.log`, `bypass-*.txt`, `post.txt`,
`/proc/cmdline` and the board name. The firmware's serial log should then
show the same `CLKSEL_CON86`, gate, reset and `REPAIR0` bits, and the rail at
800 mV.
