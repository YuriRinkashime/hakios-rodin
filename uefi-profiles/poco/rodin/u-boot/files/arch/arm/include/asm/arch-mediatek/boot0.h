/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Minimal ARM64 Linux Image header (64 bytes, magic "ARMd" at +0x38)
 * for chainloading U-Boot from MediaTek LK on MT6899 (poco/rodin).
 */

	b	reset				/* branch to startup code */
	.long	0				/* reserved */
	.quad	0x80000				/* text_offset (512 KiB) */
	.quad	0x01000000			/* image_size (16 MiB) */
	.quad	0xa				/* flags: LE kernel, 4K pages, anywhere */
	.quad	0				/* reserved */
	.quad	0				/* reserved */
	.quad	0				/* reserved */
	.ascii	"ARM\x64"			/* Magic number ("ARMd") */
	.long	0				/* reserved */
