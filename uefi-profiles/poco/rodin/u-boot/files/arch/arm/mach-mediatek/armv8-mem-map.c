// SPDX-License-Identifier: GPL-2.0-only
/*
 * Custom ARMv8 memory map for POCO X7 Pro (rodin / MT6899) EL1 chainboot:
 * - Strictly 2 MiB-aligned with ZERO overlap onto EMI-MPU / TZ / SSPM / SCP /
 *   ADSP / modem / GPU / GenieZone no-map carve-outs from rodin-fdt.dts.
 * - Keeps Android ramoops/pstore (0x6ec00000) and LK framebuffer (0xfcc20000)
 *   as MT_NORMAL_NC so pixels and breadcrumbs are DMA/scanout coherent.
 * - Uses PTE_BLOCK_INNER_SHARE for normal/NC RAM to match TCR_SHARED_INNER.
 */

#include <asm/armv8/mmu.h>

static struct mm_region mediatek_mem_map[] = {
	{
		/* Peripherals (GICv3, TOPRGU, UART, dispsys) */
		.virt = 0x00000000UL,
		.phys = 0x00000000UL,
		.size = 0x20000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* Low kernel load window below aee_debug_kinfo@48080000 (includes LK FDT at 0x47c80000) */
		.virt = 0x40000000UL,
		.phys = 0x40000000UL,
		.size = 0x08000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) | PTE_BLOCK_INNER_SHARE,
	}, {
		/* Clean low DRAM 0x48a00000..0x6cc00000 (above BL31@48800000, includes SYS_LOAD_ADDR & initrd) */
		.virt = 0x48a00000UL,
		.phys = 0x48a00000UL,
		.size = 0x24200000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) | PTE_BLOCK_INNER_SHARE,
	}, {
		/* Android ramoops / pstore (0x6ec00000..0x6f010000, 2 MiB aligned) */
		.virt = 0x6ec00000UL,
		.phys = 0x6ec00000UL,
		.size = 0x00600000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL_NC) | PTE_BLOCK_INNER_SHARE,
	}, {
		/* Clean 384 MiB mid-DRAM 0xa0000000..0xb8000000 (above SSPM@9fd70000, below SCP@b8000000) */
		.virt = 0xa0000000UL,
		.phys = 0xa0000000UL,
		.size = 0x18000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) | PTE_BLOCK_INNER_SHARE,
	}, {
		/* LK framebuffer (mblock-17-framebuffer 0xfcc20000..0xffff0000, 2 MiB aligned) */
		.virt = 0xfcc00000UL,
		.phys = 0xfcc00000UL,
		.size = 0x03400000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL_NC) | PTE_BLOCK_INNER_SHARE,
	}, {
		/* List terminator */
	}
};

struct mm_region *mem_map = mediatek_mem_map;
