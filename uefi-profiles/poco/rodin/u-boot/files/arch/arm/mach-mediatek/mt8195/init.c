// SPDX-License-Identifier: GPL-2.0
/*
 * POCO X7 Pro (rodin / MT6899) U-Boot board bring-up hook:
 * 1. Disables MediaTek TOPRGU hardware watchdog (0x1c00b000) in dram_init(),
 *    board_early_init_r(), and board_late_init() to prevent the ~20s reset.
 * 2. Clamps board_get_usable_ram_top() to 0xb7000000 (clean mid-DRAM with
 *    a 16 MiB guard below mblock-31-SCP-reserved@b8000000).
 * 3. Paints a compact visual header strip at y=0..47 on Buffer 0 (0xfcc20000):
 *      M1 (y=0..19):          Green  = dram_init()
 *      S1..S6 (y=22..31):     White / Magenta / Cyan / Orange / Lime / Pink
 *      M2 (x=0..609,y=34..47):   Blue   = board_late_init() + simplefb ready
 *      M3 (x=610..1219,y=34..47): Yellow = hakios load OK | Red = failed
 *    plus high-contrast 16x32 direct-FB fallback text at y=64..191.
 * 4. Writes non-destructive ASCII breadcrumbs into pmsg-ramoops-0 (0x6ee10000).
 * 5. Scans GKI v4 /chosen linux,initrd-* byte-by-byte for HKUB and stages it
 *    to 4 KiB-aligned 0x60000000 for hakios load.
 */

#include <command.h>
#include <cpu_func.h>
#include <dm/uclass.h>
#include <env.h>
#include <fdtdec.h>
#include <init.h>
#include <stdio.h>
#include <video.h>
#include <video_font_ter16x32.h>
#include <asm/armv8/mmu.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <asm/system.h>
#include <linux/kernel.h>
#include <linux/libfdt.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <wdt.h>

DECLARE_GLOBAL_DATA_PTR;

/* MediaTek TOPRGU watchdog@1c00b000 */
#define MT6899_TOPRGU_BASE	0x1c00b000UL
#define MTK_WDT_MODE		(MT6899_TOPRGU_BASE + 0x00UL)
#define MTK_WDT_RESTART		(MT6899_TOPRGU_BASE + 0x08UL)
#define WDT_MODE_KEY		0x22000000U
#define WDT_MODE_EN		0x00000001U
#define WDT_RESTART_KEY		0x00001971U

/* Clean mid-DRAM ceiling with 16 MiB guard below mblock-31-SCP-reserved@b8000000 */
#define RODIN_USABLE_RAM_TOP	0xb7000000UL

/* Safe payload window (hakios-payload@ac000000) and aligned HKUB staging buffer */
#define RODIN_PAYLOAD_BASE	0xac000000UL
#define RODIN_HKUB_STAGE_ADDR	0x60000000UL
#define RODIN_HKUB_STAGE_MAX	(16UL << 20)

/* Preloader/LK framebuffer (mblock-17-framebuffer, Buffer 0 verified on hardware) */
#define RODIN_FB_BASE		0xfcc20000UL
#define RODIN_FB_W		1220U
#define RODIN_FB_H		2712U
#define RODIN_FB_PITCH		(1248U * 4U)

/* Android pmsg-ramoops-0 zone (0x6ec00000 + 0x10000 dump + 0x200000 console) */
#define RODIN_PMSG_BASE		0x6ee10000UL
#define RODIN_PMSG_SIZE		0x00200000UL
#define RAMOOPS_SIG		0x43474244U	/* "DBGC" */

/* HKUB v1 magic and XNUP magic */
#define HKUB_MAGIC		0x42554B48U
#define XNUP_MAGIC		0x50554E58U

struct rodin_ramoops_hdr {
	u32 sig;
	u32 start;
	u32 size;
};

struct rodin_hkub_hdr {
	u32 magic;
	u32 version;
	u32 count;
	u32 header_size;
};

struct rodin_hkub_entry {
	char name[16];
	u32 offset;
	u32 size;
	u32 crc32;
	u32 reserved;
};

static void rodin_wdt_disable(u32 *before, u32 *after)
{
	u32 mode;

	writel(WDT_RESTART_KEY, MTK_WDT_RESTART);
	mode = readl(MTK_WDT_MODE);
	if (before)
		*before = mode;
	writel((mode & ~WDT_MODE_EN) | WDT_MODE_KEY, MTK_WDT_MODE);
	if (after)
		*after = readl(MTK_WDT_MODE);
}

static void rodin_pmsg_reset(void)
{
	volatile struct rodin_ramoops_hdr *hdr =
		(volatile struct rodin_ramoops_hdr *)RODIN_PMSG_BASE;

	hdr->sig = RAMOOPS_SIG;
	hdr->start = 0;
	hdr->size = 0;
	__asm__ volatile("dsb sy" : : : "memory");
}

static void rodin_pmsg_puts(const char *s)
{
	volatile struct rodin_ramoops_hdr *hdr =
		(volatile struct rodin_ramoops_hdr *)RODIN_PMSG_BASE;
	volatile char *data = (volatile char *)(RODIN_PMSG_BASE + sizeof(*hdr));
	u32 cap = RODIN_PMSG_SIZE - sizeof(*hdr);
	u32 at = hdr->start;
	u32 sz = hdr->size;

	if (hdr->sig != RAMOOPS_SIG || at >= cap || sz > cap) {
		rodin_pmsg_reset();
		at = 0;
		sz = 0;
	}
	while (*s) {
		data[at++] = *s++;
		if (at >= cap)
			at = 0;
		if (sz < cap)
			sz++;
	}
	hdr->start = at;
	hdr->size = sz;
	__asm__ volatile("dsb sy" : : : "memory");
}

static void rodin_fb_rect(u32 x0, u32 y0, u32 w, u32 h, u32 argb, bool ruler)
{
	u32 y, x;
	u32 x_end = min(x0 + w, RODIN_FB_W);
	u32 y_end = min(y0 + h, RODIN_FB_H);
	ulong start = RODIN_FB_BASE + (ulong)y0 * RODIN_FB_PITCH;
	ulong end = RODIN_FB_BASE + (ulong)y_end * RODIN_FB_PITCH;

	for (y = y0; y < y_end; y++) {
		volatile u32 *row = (volatile u32 *)(RODIN_FB_BASE + (ulong)y * RODIN_FB_PITCH);

		for (x = x0; x < x_end; x++)
			row[x] = (ruler && x < x0 + 16U) ? 0xFFFFFFFFU : argb;
	}
	__asm__ volatile("dsb sy" : : : "memory");
	if (dcache_status())
		flush_dcache_range(ALIGN_DOWN(start, 64), ALIGN(end, 64));
}

static void rodin_fb_puts_line(u32 line_idx, u32 fg, u32 bg, const char *s)
{
	u32 x0 = 16U;
	u32 y0 = 64U + line_idx * 32U;
	u32 max_cols = (RODIN_FB_W - x0) / 16U;
	u32 col = 0;
	ulong start, end;

	if (y0 + 32U > RODIN_FB_H)
		return;

	rodin_fb_rect(x0, y0, RODIN_FB_W - x0, 32U, bg, false);
	while (s && *s && col < max_cols) {
		u8 ch = (u8)*s++;
		const unsigned char *glyph = &video_fontdata_16x32[(u32)ch * 64U];
		u32 r, c;

		for (r = 0; r < 32U; r++) {
			u16 bits = ((u16)glyph[r * 2U] << 8) | (u16)glyph[r * 2U + 1U];
			volatile u32 *row =
				(volatile u32 *)(RODIN_FB_BASE + (ulong)(y0 + r) * RODIN_FB_PITCH);
			u32 px = x0 + col * 16U;

			for (c = 0; c < 16U; c++)
				row[px + c] = (bits & (0x8000U >> c)) ? fg : bg;
		}
		col++;
	}
	start = RODIN_FB_BASE + (ulong)y0 * RODIN_FB_PITCH;
	end = start + 32UL * RODIN_FB_PITCH;
	__asm__ volatile("dsb sy" : : : "memory");
	if (dcache_status())
		flush_dcache_range(ALIGN_DOWN(start, 64), ALIGN(end, 64));
}

void rodin_stage_slice(u32 idx, u32 argb, const char *tag)
{
	u32 x0 = idx * 200U;

	if (x0 < RODIN_FB_W) {
		u32 w = min(200U, RODIN_FB_W - x0);

		rodin_fb_rect(x0, 22U, w, 10U, argb, false);
	}
	if (tag) {
		char line[96];

		snprintf(line, sizeof(line), "[rodin-uboot] %s\n", tag);
		rodin_pmsg_puts(line);
	}
}

int dram_init(void)
{
	u32 wdt_before = 0, wdt_after = 0;
	char msg[128];

	rodin_wdt_disable(&wdt_before, &wdt_after);

	/* Milestone 1: compact pre-relocation Green header bar (y=0..19) */
	rodin_fb_rect(0U, 0U, RODIN_FB_W, 20U, 0xFF00FF00U, true);

	rodin_pmsg_reset();
	snprintf(msg, sizeof(msg),
		 "[rodin-uboot] stage1: dram_init wdt=0x%08x->0x%08x\n",
		 wdt_before, wdt_after);
	rodin_pmsg_puts(msg);

	return fdtdec_setup_mem_size_base();
}

phys_addr_t board_get_usable_ram_top(phys_size_t total_size)
{
	rodin_stage_slice(0U, 0xFFFFFFFFU, "S1:usable_ram_top");
	/* Keep relocation, MMU page tables, 32 MiB malloc heap, and stack <= 0xb7000000 */
	return min(gd->ram_top, (phys_addr_t)RODIN_USABLE_RAM_TOP);
}

int dram_init_banksize(void)
{
	int ret = fdtdec_setup_memory_banksize();

	rodin_stage_slice(1U, 0xFFFF00FFU, "S2:dram_banksize");
	return ret;
}

int board_early_init_r(void)
{
	rodin_wdt_disable(NULL, NULL);
	rodin_stage_slice(4U, 0xFFAAFF00U, "S5:board_early_init_r");
	return 0;
}

static bool rodin_window_ok(u64 addr, u64 size, u64 start, u64 end)
{
	return addr >= start && addr < end && size <= (end - addr);
}

static bool rodin_addr_readable(u64 addr, u64 size)
{
	if (!addr || !size)
		return false;

	return rodin_window_ok(addr, size, 0x40000000ULL, 0x48000000ULL) ||
	       rodin_window_ok(addr, size, 0x48a00000ULL, 0x6cc00000ULL) ||
	       rodin_window_ok(addr, size, 0xa0000000ULL, 0xb8000000ULL);
}

static u64 rodin_find_hkub(u64 start, u64 end, u32 *out_size)
{
	u64 p, limit;

	if (end <= start || !rodin_addr_readable(start, end - start))
		return 0;

	limit = min(end, start + (128ULL << 20));
	for (p = start; p + sizeof(struct rodin_hkub_hdr) <= limit; p++) {
		const u8 *b = (const u8 *)(uintptr_t)p;
		struct rodin_hkub_hdr h;

		if (b[0] != 'H' || b[1] != 'K' || b[2] != 'U' || b[3] != 'B')
			continue;

		memcpy(&h, b, sizeof(h));
		if (h.magic == HKUB_MAGIC && h.version == 1 &&
		    h.count >= 1 && h.count <= 16 &&
		    h.header_size == sizeof(h) + h.count * sizeof(struct rodin_hkub_entry) &&
		    p + h.header_size <= end) {
			struct rodin_hkub_entry entries[16];
			u32 total = h.header_size;
			u32 i;

			memcpy(entries, b + sizeof(h), h.count * sizeof(entries[0]));
			for (i = 0; i < h.count; i++) {
				u64 end_off = (u64)entries[i].offset + entries[i].size;

				if (end_off > (u64)(end - p)) {
					total = 0;
					break;
				}
				if ((u32)end_off > total)
					total = (u32)end_off;
			}
			if (total && p + total <= end) {
				*out_size = total;
				return p;
			}
		}
	}
	return 0;
}

int board_late_init(void)
{
	u32 wdt_before = 0, wdt_after = 0;
	phys_addr_t fdt_pa = get_prev_bl_fdt_addr();
	u64 initrd_start = 0, initrd_end = 0, hkub_addr = 0, staged_addr = 0;
	u32 initrd_word0 = 0, hkub_size = 0;
	volatile u32 *pay_hdr = (volatile u32 *)RODIN_PAYLOAD_BASE;
	char msg[320];
	char line0[96], line1[96], line2[96];

	rodin_wdt_disable(&wdt_before, &wdt_after);

	/* Ensure no stale garbage at PAYLOAD_BASE masquerades as XNUP */
	if (pay_hdr[0] != XNUP_MAGIC)
		pay_hdr[0] = 0;

	/*
	 * video_post_probe() clears the framebuffer when console probes, so
	 * repaint compact header strip M1 + S1..S6 + M2 (Blue x=0..609, y=34..47).
	 */
	rodin_fb_rect(0U, 0U, RODIN_FB_W, 20U, 0xFF00FF00U, true);
	rodin_stage_slice(0U, 0xFFFFFFFFU, NULL);
	rodin_stage_slice(1U, 0xFFFF00FFU, NULL);
	rodin_stage_slice(2U, 0xFF00FFFFU, NULL);
	rodin_stage_slice(3U, 0xFFFF8000U, NULL);
	rodin_stage_slice(4U, 0xFFAAFF00U, NULL);
	rodin_stage_slice(5U, 0xFFFF66CCU, "S6:board_late_init");
	rodin_fb_rect(0U, 34U, 610U, 14U, 0xFF0066FFU, true);

	if (rodin_addr_readable((u64)fdt_pa, 4096ULL)) {
		const void *fdt = (const void *)(uintptr_t)fdt_pa;

		if (fdt_check_header(fdt) == 0) {
			int chosen = fdt_path_offset(fdt, "/chosen");

			if (chosen >= 0) {
				initrd_start = fdtdec_get_uint64(fdt, chosen, "linux,initrd-start", 0);
				if (!initrd_start)
					initrd_start = fdtdec_get_uint(fdt, chosen, "linux,initrd-start", 0);
				initrd_end = fdtdec_get_uint64(fdt, chosen, "linux,initrd-end", 0);
				if (!initrd_end)
					initrd_end = fdtdec_get_uint(fdt, chosen, "linux,initrd-end", 0);
			}
		}
	}

	if (rodin_addr_readable(initrd_start, 4ULL))
		memcpy(&initrd_word0, (const void *)(uintptr_t)initrd_start, sizeof(initrd_word0));

	hkub_addr = rodin_find_hkub(initrd_start, initrd_end, &hkub_size);
	if (hkub_addr && hkub_size <= RODIN_HKUB_STAGE_MAX) {
		staged_addr = RODIN_HKUB_STAGE_ADDR;
		memcpy((void *)(uintptr_t)staged_addr, (const void *)(uintptr_t)hkub_addr, hkub_size);
		if (dcache_status())
			flush_dcache_range(staged_addr, ALIGN(staged_addr + hkub_size, 64));
		env_set_hex("hakios_hkub_addr", (ulong)staged_addr);
		env_set_hex("hakios_hkub_size", (ulong)hkub_size);
	} else if (hkub_addr) {
		staged_addr = hkub_addr;
		env_set_hex("hakios_hkub_addr", (ulong)hkub_addr);
		env_set_hex("hakios_hkub_size", (ulong)hkub_size);
	} else {
		env_set("hakios_hkub_addr", NULL);
		env_set("hakios_hkub_size", NULL);
	}

	snprintf(line0, sizeof(line0),
		 "reloc=0x%lx top=0x%llx wdt=0x%08x->0x%08x",
		 gd->relocaddr, (unsigned long long)gd->ram_top, wdt_before, wdt_after);
	snprintf(line1, sizeof(line1),
		 "fdt=0x%llx rd=0x%llx..0x%llx w0=0x%08x",
		 (unsigned long long)fdt_pa, (unsigned long long)initrd_start,
		 (unsigned long long)initrd_end, initrd_word0);
	if (hkub_addr)
		snprintf(line2, sizeof(line2),
			 "hkub=0x%llx -> stage=0x%llx sz=0x%x (%u B)",
			 (unsigned long long)hkub_addr, (unsigned long long)staged_addr,
			 hkub_size, hkub_size);
	else
		snprintf(line2, sizeof(line2),
			 "hkub=NOT FOUND in initrd 0x%llx..0x%llx",
			 (unsigned long long)initrd_start, (unsigned long long)initrd_end);

	/* Paint guaranteed-visible direct-FB 16x32 diagnostic lines at y=64..159 */
	rodin_fb_puts_line(0U, 0xFFFFFFFFU, 0xFF000000U, line0);
	rodin_fb_puts_line(1U, 0xFF00FFFFU, 0xFF000000U, line1);
	rodin_fb_puts_line(2U, hkub_addr ? 0xFF00FF00U : 0xFFFF4444U, 0xFF000000U, line2);

	snprintf(msg, sizeof(msg),
		 "[rodin-uboot] stage2: %s\n"
		 "[rodin-uboot] %s\n"
		 "[rodin-uboot] %s\n",
		 line0, line1, line2);
	rodin_pmsg_puts(msg);

	/* Start vidconsole below direct-FB lines (7 * 32 = y >= 224) */
	printf("\n\n\n\n\n\n\n%s", msg);
	video_sync_all();

	return 0;
}

static int do_rodin_mark(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	if (argc != 2)
		return CMD_RET_USAGE;

	if (!strcmp(argv[1], "ok")) {
		rodin_fb_rect(610U, 34U, 610U, 14U, 0xFFFFFF00U, false);	/* Yellow */
		rodin_fb_puts_line(3U, 0xFFFFFF00U, 0xFF000000U,
				   "stage3: hakios load OK -> running hakios boot (loader.efi)");
		rodin_pmsg_puts("[rodin-uboot] stage3: hakios load OK -> launching loader.efi\n");
		video_sync_all();
		return CMD_RET_SUCCESS;
	}
	if (!strcmp(argv[1], "fail")) {
		rodin_fb_rect(610U, 34U, 610U, 14U, 0xFFFF0000U, false);	/* Red */
		rodin_fb_puts_line(3U, 0xFFFF4444U, 0xFF000000U,
				   "stage3: hakios load/boot FAILED or returned (see vidconsole below)");
		rodin_pmsg_puts("[rodin-uboot] stage3: hakios load FAILED or boot returned\n");
		video_sync_all();
		return CMD_RET_SUCCESS;
	}
	return CMD_RET_USAGE;
}

U_BOOT_CMD(rodin_mark, 2, 0, do_rodin_mark,
	   "paint visual milestone bar and log to pmsg-ramoops-0",
	   "ok | fail"
);

void reset_cpu(void)
{
	struct udevice *wdt;

	if (IS_ENABLED(CONFIG_PSCI_RESET)) {
		psci_system_reset();
	} else {
		uclass_first_device(UCLASS_WDT, &wdt);
		if (wdt)
			wdt_expire_now(wdt, 0);
	}
}

int print_cpuinfo(void)
{
	printf("CPU:   MediaTek MT6899 (poco/rodin)\n");
	return 0;
}
