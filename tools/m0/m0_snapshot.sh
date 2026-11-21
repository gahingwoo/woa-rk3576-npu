#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Jiaxing Hu <gahing@gahingwoo.com>
#
# M0 part 1: the NPU's working state under Linux, read only.
#
# Run as root on an RK3576 board under Linux with the rocket driver bound
# and charsiu installed. It
#   1. runs one NPU job through rocket (charsiu_matmul 1 256 64, an m = 1
#      int8 matmul checked against the CPU, or the command given with --run)
#      and stops if it fails,
#   2. pins runtime PM on for both NPU cores, so their domains and clocks
#      stay up, and stops unless genpd and the clock framework both say so,
#   3. reads the power-domain, clock, reset, rail, IOMMU and version state,
#   4. puts runtime PM back and writes everything into m0-<host>-<time>.tar.gz.
#
# Nothing here writes a register or a PMIC. The only writes are to the two
# runtime PM "control" files, which is what pinning means, and they are put
# back on exit. /dev/mem is opened read only by m0_peek.
#
# Usage: m0_snapshot.sh [--run "COMMAND"] [--peek PATH] [--out DIR]
#
# Every address below is cited. Sources:
#   [pmd]  drivers/pmdomain/rockchip/pm-domains.c, rk3576_pmu and
#          rk3576_pm_domains[] (mainline), with the 15 us NPUTOP settle and
#          PD_NPU need_regulator from the NPU series (rfc-send-v17/pd).
#   [clk]  drivers/clk/rockchip/clk-rk3576.c, "npu" block, and clk.h
#          (RK3576_CLKSEL_CON(x) = 0x300 + 4x, RK3576_CLKGATE_CON(x) =
#          0x800 + 4x, RK3576_SOFTRST_CON(x) = 0xa00 + 4x).
#   [rst]  drivers/clk/rockchip/rst-rk3576.c, RK3576_CRU_RESET_OFFSET.
#   [dts]  rk3576.dtsi: cru@27200000, power-management@27380000, and the NPU
#          nodes the series adds (rfc-send-v17/npu v17-0011): rknn_core_0
#          27700000 (pc +0, cna +0x1000, core +0x3000), rknn_mmu_0
#          27702000/27702100, rknn_core_1 27708000, rknn_mmu_1
#          2770a000/2770a100.
#   [rkt]  drivers/accel/rocket/rocket_registers.h (REG_PC_*).
#   [mmu]  drivers/iommu/rockchip-iommu.c (RK_MMU_*).
#   [pll]  the GPLL/MODE registers as edk2-rk3576 Vdec.c reads them.

set -u

RUN=""
PEEK=""
OUTBASE="."
while [ $# -gt 0 ]; do
	case "$1" in
	--run)  RUN="$2"; shift 2 ;;
	--peek) PEEK="$2"; shift 2 ;;
	--out)  OUTBASE="$2"; shift 2 ;;
	-h|--help) sed -n '2,30p' "$0"; exit 0 ;;
	*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
done

die() { echo "m0_snapshot: $*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run as root"
[ -n "$PEEK" ] || PEEK="$(dirname "$0")/m0_peek"
[ -x "$PEEK" ] || PEEK="$(command -v m0_peek || true)"
[ -n "$PEEK" ] && [ -x "$PEEK" ] || die "m0_peek not found (build it, or pass --peek)"

DBG=/sys/kernel/debug
[ -d "$DBG/clk" ] || mount -t debugfs none "$DBG" 2>/dev/null
[ -r "$DBG/pm_genpd/pm_genpd_summary" ] || die "no $DBG/pm_genpd/pm_genpd_summary (debugfs, CONFIG_DEBUG_FS)"
[ -r "$DBG/clk/clk_summary" ] || die "no $DBG/clk/clk_summary"

CORE0=/sys/bus/platform/devices/27700000.npu
CORE1=/sys/bus/platform/devices/27708000.npu
[ -d "$CORE0" ] || die "no $CORE0: is the NPU enabled in the device tree?"
[ "$(basename "$(readlink -f "$CORE0/driver" 2>/dev/null)")" = rocket ] || die "27700000.npu is not bound to rocket"

STAMP="$(date -u +%Y%m%d-%H%M%SZ)"
OUT="$OUTBASE/m0-$(hostname)-$STAMP"
mkdir -p "$OUT" || die "cannot create $OUT"
echo "m0_snapshot: writing $OUT"

# --- 1. one job through rocket --------------------------------------------
if [ -z "$RUN" ]; then
	# Not npu_gemm_test: it sweeps m > 1 and exits 1 when those are not
	# exact, which says nothing about whether the NPU ran.
	for d in "" /opt/charsiu/; do
		if [ -x "${d}charsiu_matmul" ] || command -v "${d}charsiu_matmul" >/dev/null 2>&1; then
			RUN="${d}charsiu_matmul 1 256 64"
			break
		fi
	done
	[ -n "$RUN" ] || die "charsiu_matmul not found on PATH or in /opt/charsiu; pass --run"
fi
echo "m0_snapshot: running: $RUN"
sh -c "$RUN" > "$OUT/run.txt" 2>&1
RUNRC=$?
echo "exit $RUNRC" >> "$OUT/run.txt"
[ "$RUNRC" = 0 ] || die "'$RUN' failed (exit $RUNRC), see $OUT/run.txt; not reading anything"

# --- 2. hold the domains on ------------------------------------------------
# rocket runtime-suspends a core 50 ms after its last job, and the domain
# goes down with it. Holding the charsiu fd open does NOT keep it up. Only
# pinning runtime PM does.
OLD0="$(cat "$CORE0/power/control")"
OLD1=""
[ -d "$CORE1" ] && OLD1="$(cat "$CORE1/power/control")"
restore() {
	echo "$OLD0" > "$CORE0/power/control" 2>/dev/null
	[ -n "$OLD1" ] && echo "$OLD1" > "$CORE1/power/control" 2>/dev/null
}
trap restore EXIT INT TERM
echo on > "$CORE0/power/control" || die "cannot pin $CORE0"
[ -n "$OLD1" ] && { echo on > "$CORE1/power/control" || die "cannot pin $CORE1"; }

domain_on() {
	awk -v d="$1" '$1 == d { print $2; exit }' "$DBG/pm_genpd/pm_genpd_summary" | grep -q '^on'
}
WANT="npu nputop npu0"
[ -n "$OLD1" ] && WANT="$WANT npu1"
ok=0
for i in 1 2 3 4 5 6 7 8 9 10; do
	all=1
	for d in $WANT; do domain_on "$d" || all=0; done
	[ "$(cat "$CORE0/power/runtime_status")" = active ] || all=0
	[ $all = 1 ] && { ok=1; break; }
	sleep 0.5
done
cp "$DBG/pm_genpd/pm_genpd_summary" "$OUT/pm_genpd_summary.txt"
# A wait that times out must END the script. Reading a block whose domain is
# off is an SError (this panicked a CM5-IO on 2026-11-01).
[ $ok = 1 ] || die "domains not all on after 5 s ($WANT), see $OUT/pm_genpd_summary.txt; nothing read"

# Clocks the NPU and its IOMMUs need before their registers answer [dts].
NPUCLKS="clk_rknn_dsu0 aclk_rknn0 hclk_rknn_root pclk_nputop_root aclk_rknn_cbuf hclk_rknn_cbuf"
[ -n "$OLD1" ] && NPUCLKS="$NPUCLKS aclk_rknn1"
for c in $NPUCLKS; do
	n="$(cat "$DBG/clk/$c/clk_enable_count" 2>/dev/null || echo missing)"
	[ "$n" != missing ] && [ "$n" -gt 0 ] 2>/dev/null || die "clock $c enable_count=$n; nothing read from the NPU"
done

# --- 3. read ---------------------------------------------------------------
{
	uname -a
	cat /proc/cmdline
	date -u
	[ -r /proc/device-tree/model ] && tr -d '\0' < /proc/device-tree/model && echo
	command -v charsiu >/dev/null && charsiu --version 2>&1 | head -1
} > "$OUT/system.txt" 2>&1
[ -r /proc/config.gz ] && cp /proc/config.gz "$OUT/config.gz"
dmesg 2>/dev/null | grep -i -E 'rocket|rknn|npu|iommu|27700000|27708000|vdd_npu' > "$OUT/dmesg-npu.txt"

# genpd per-domain counters (active_time rises across a run)
for d in npu nputop npu0 npu1; do
	[ -d "$DBG/pm_genpd/$d" ] || continue
	for f in current_state active_time total_idle_time sub_domains devices; do
		[ -r "$DBG/pm_genpd/$d/$f" ] && printf '%s/%s: %s\n' "$d" "$f" "$(tr '\n' ' ' < "$DBG/pm_genpd/$d/$f")"
	done
done > "$OUT/genpd-npu.txt"

# clock framework view
cp "$DBG/clk/clk_summary" "$OUT/clk_summary.txt"
for c in gpll cpll $NPUCLKS aclk_rknn1 hclk_npu_cm0_root; do
	[ -d "$DBG/clk/$c" ] || continue
	printf '%-20s rate=%s enable=%s prepare=%s parent=%s\n' "$c" \
		"$(cat "$DBG/clk/$c/clk_rate")" "$(cat "$DBG/clk/$c/clk_enable_count")" \
		"$(cat "$DBG/clk/$c/clk_prepare_count")" "$(cat "$DBG/clk/$c/clk_parent" 2>/dev/null)"
done > "$OUT/clk-npu.txt"

# the rail: regulator sysfs and debugfs. num_users is the live enable count,
# not a consumer count; open_count is what says the domain took the rail.
for r in /sys/class/regulator/regulator.*; do
	[ "$(cat "$r/name" 2>/dev/null)" = vdd_npu_s0 ] || continue
	for f in name microvolts state status num_users min_microvolts max_microvolts; do
		[ -r "$r/$f" ] && printf '%s: %s\n' "$f" "$(cat "$r/$f")"
	done
done > "$OUT/regulator-vdd_npu_s0.txt"
if [ -d "$DBG/regulator/vdd_npu_s0" ]; then
	for f in "$DBG"/regulator/vdd_npu_s0/*; do
		[ -f "$f" ] && printf '%s: %s\n' "$(basename "$f")" "$(tr '\n' ' ' < "$f")"
	done >> "$OUT/regulator-vdd_npu_s0.txt"
fi
[ -r "$DBG/regulator/regulator_summary" ] && cp "$DBG/regulator/regulator_summary" "$OUT/regulator_summary.txt"

# IOMMU domain type of the NPU group: DMA / DMA-FQ / identity. It decides
# whether the bypass test needs to touch the IOMMU at all.
for c in "$CORE0" "$CORE1"; do
	[ -e "$c/iommu_group" ] || continue
	g="$(readlink -f "$c/iommu_group")"
	printf '%s: group %s type %s\n' "$(basename "$c")" "$(basename "$g")" "$(cat "$g/type" 2>/dev/null)"
done > "$OUT/iommu-groups.txt"

# PMU through its syscon regmap, which the PMU driver itself uses [pmd].
for d in "$DBG"/regmap/*27380000*; do
	[ -r "$d/registers" ] && cp "$d/registers" "$OUT/regmap-pmu.txt" && break
done

PMU=0x27380000
CRU=0x27200000
hex() { printf '0x%08x' "$(( $1 ))"; }

# PMU [pmd]: rk3576_pmu offsets; NPU bits: PWR_CON0 bit 0 (npu),
# PWR_CON1 bits 6/7/8 (nputop/npu0/npu1), STATUS bit 0 (npu, 1 = off),
# REPAIR bits 22/23/24 (1 = on), REQ/ACK/IDLE 0x18 (nputop), bit 1 (npu0),
# bit 2 (npu1), CLK_UNGATE 0x18/0x1a/0x1c.
"$PEEK" \
	PMU_REQ0=$(hex $PMU+0x110) PMU_ACK0=$(hex $PMU+0x120) PMU_IDLE0=$(hex $PMU+0x128) \
	PMU_CLK_UNGATE=$(hex $PMU+0x140) PMU_PWR_CON0=$(hex $PMU+0x210) PMU_PWR_CON1=$(hex $PMU+0x214) \
	PMU_STATUS0=$(hex $PMU+0x230) PMU_CHAIN_STATUS=$(hex $PMU+0x248) PMU_MEM_STATUS=$(hex $PMU+0x250) \
	PMU_REPAIR0=$(hex $PMU+0x570) \
	> "$OUT/pmu.txt" 2>&1

# CRU [clk][rst][pll]: CLKSEL_CON86 ([1:0] hclk_rknn_root mux, [6:2]
# clk_rknn_dsu0 div, [8:7] its mux gpll/cpll/aupll/spll), CLKSEL_CON87
# ([1:0] pclk_nputop_root mux, [4:3] hclk_npu_cm0_root mux), CLKGATE 28 bit 9
# aclk_rknn0, 29 bit 0 aclk_rknn1, 31 bits 4/5/8 hclk_rknn_root /
# clk_rknn_dsu0 / pclk_nputop_root, 32 bits 0/5/12 aclk_rknn_cbuf /
# hclk_npu_cm0_root / hclk_rknn_cbuf; SOFTRST 28 bits 9/11/12 A_RKNN0 /
# A_RKNN0_BIU / L_RKNN0_BIU, 29 bits 0/2/3 A_RKNN1 / BIUs, 31 bits 1/9
# L_NPUSUBSYS_BIU / P_NPUTOP_BIU, 32 bits 0/11/12/13 A_RKNN_CBUF /
# A_RKNNTOP_BIU / H_RKNN_CBUF / H_RKNNTOP_BIU. GPLL_CON0..2 at 0x1c0,
# MODE_CON00 at 0x280 (GPLL mode [3:2]).
"$PEEK" \
	CRU_CLKSEL86=$(hex $CRU+0x300+86*4) CRU_CLKSEL87=$(hex $CRU+0x300+87*4) \
	CRU_GATE28=$(hex $CRU+0x800+28*4) CRU_GATE29=$(hex $CRU+0x800+29*4) \
	CRU_GATE31=$(hex $CRU+0x800+31*4) CRU_GATE32=$(hex $CRU+0x800+32*4) \
	CRU_SOFTRST28=$(hex $CRU+0xa00+28*4) CRU_SOFTRST29=$(hex $CRU+0xa00+29*4) \
	CRU_SOFTRST31=$(hex $CRU+0xa00+31*4) CRU_SOFTRST32=$(hex $CRU+0xa00+32*4) \
	CRU_GPLL_CON0=$(hex $CRU+0x1c0) CRU_GPLL_CON1=$(hex $CRU+0x1c4) CRU_GPLL_CON2=$(hex $CRU+0x1c8) \
	CRU_MODE_CON00=$(hex $CRU+0x280) \
	> "$OUT/cru.txt" 2>&1

# NPU cores [rkt] and their IOMMUs [mmu]. rocket owns these ranges, so
# CONFIG_IO_STRICT_DEVMEM refuses the mmap without iomem=relaxed; m0_peek
# then prints mmap-failed and nothing is read.
npu_regs() {
	b=$1; p=$2
	echo "${p}_PC_VERSION=$(hex $b+0x0)" "${p}_PC_VERSION_NUM=$(hex $b+0x4)" \
	     "${p}_PC_OPERATION_ENABLE=$(hex $b+0x8)" "${p}_PC_INTERRUPT_MASK=$(hex $b+0x20)" \
	     "${p}_PC_INTERRUPT_RAW_STATUS=$(hex $b+0x2c)" "${p}_PC_TASK_CON=$(hex $b+0x30)" \
	     "${p}_PC_TASK_STATUS=$(hex $b+0x3c)"
	for m in 0x2000 0x2100; do
		echo "${p}_MMU${m}_DTE_ADDR=$(hex $b+$m+0x00)" "${p}_MMU${m}_STATUS=$(hex $b+$m+0x04)" \
		     "${p}_MMU${m}_PAGE_FAULT_ADDR=$(hex $b+$m+0x0c)" "${p}_MMU${m}_INT_RAWSTAT=$(hex $b+$m+0x14)" \
		     "${p}_MMU${m}_INT_MASK=$(hex $b+$m+0x1c)" "${p}_MMU${m}_AUTO_GATING=$(hex $b+$m+0x24)"
	done
}
# shellcheck disable=SC2046
"$PEEK" $(npu_regs 0x27700000 CORE0) > "$OUT/npu-core0.txt" 2>&1
if [ -n "$OLD1" ] && domain_on npu1; then
	# shellcheck disable=SC2046
	"$PEEK" $(npu_regs 0x27708000 CORE1) > "$OUT/npu-core1.txt" 2>&1
fi
grep -q mmap-failed "$OUT/npu-core0.txt" && \
	echo "m0_snapshot: NPU registers refused by /dev/mem; reboot with iomem=relaxed for this session" | tee "$OUT/NOTE-iomem.txt"

# genpd again: the domains must still be on after the reads
cp "$DBG/pm_genpd/pm_genpd_summary" "$OUT/pm_genpd_summary-after.txt"

# --- 4. put runtime PM back and pack ---------------------------------------
restore
trap - EXIT INT TERM
TAR="$OUT.tar.gz"
tar -czf "$TAR" -C "$(dirname "$OUT")" "$(basename "$OUT")" || die "tar failed"
echo "m0_snapshot: done, send $TAR"
