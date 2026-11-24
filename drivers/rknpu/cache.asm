//
// cache.asm - data cache maintenance by virtual address, for cached NPU buffers (M6)
//
// The NPU is not coherent (_CCA 0). With cached buffers the CPU's caches
// hold lines the NPU cannot see, so before the NPU reads a buffer the CPU
// wrote, the range is cleaned to the point of coherency (DC CVAC), and
// before the CPU reads what the NPU wrote it is cleaned and invalidated
// (DC CIVAC). That is what Linux's arm64 dma_sync_*() do for rocket's
// PREP_BO and FINI_BO. The WDK has no intrinsic for DC, hence these two
// leaf functions (no stack, no unwind data needed). Maintenance by VA to
// PoC is broadcast to every CPU in the inner shareable domain, so it does
// not matter which CPU runs it or which mapping of the page is used.
//
//   VOID RknpuDcClean(PVOID Va, SIZE_T Length)            dc cvac,  then dsb sy
//   VOID RknpuDcCleanInvalidate(PVOID Va, SIZE_T Length)  dc civac, then dsb sy
//
// The line size is CTR_EL0.DminLine (64 bytes on Cortex-A72 and A53).
//
// Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
// SPDX-License-Identifier: GPL-2.0-only
//

	EXPORT RknpuDcClean
	EXPORT RknpuDcCleanInvalidate
	AREA   s_RknpuCache, CODE, READONLY

RknpuDcClean
	cbz   x1, RknpuDcClean_done
	mrs   x3, CTR_EL0
	ubfx  x3, x3, #16, #4
	mov   x2, #4
	lsl   x2, x2, x3
	add   x1, x0, x1
	sub   x3, x2, #1
	bic   x0, x0, x3
RknpuDcClean_loop
	dc    cvac, x0
	add   x0, x0, x2
	cmp   x0, x1
	blo   RknpuDcClean_loop
RknpuDcClean_done
	dsb   sy
	ret

RknpuDcCleanInvalidate
	cbz   x1, RknpuDcCleanInvalidate_done
	mrs   x3, CTR_EL0
	ubfx  x3, x3, #16, #4
	mov   x2, #4
	lsl   x2, x2, x3
	add   x1, x0, x1
	sub   x3, x2, #1
	bic   x0, x0, x3
RknpuDcCleanInvalidate_loop
	dc    civac, x0
	add   x0, x0, x2
	cmp   x0, x1
	blo   RknpuDcCleanInvalidate_loop
RknpuDcCleanInvalidate_done
	dsb   sy
	ret

	END
