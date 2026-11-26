# POCO X7 Pro (`rodin`)

| | |
| --- | --- |
| Device | POCO X7 Pro (`24129RT7CC`, codename `rodin`) |
| SoC | MediaTek Dimensity 8400-Ultra (`MT6899`), 8× Cortex-A725 (4 + 3 + 1 All-Big-Core); `soc.dtsi` |
| GPU | Arm Mali-G720 MC7 (`arm,mali-valhall`, `0x13000000`, GPUEB `0x13c00000`) |
| Memory | 12 GiB LPDDR5X (`0x40000000`–`0x33fffffff`) |
| Target Kernel | HakiOS `oosexgang/hakios-xnu-kernel` branch `common` (`xnu-12377.121.6` / Darwin 27.2) |
| UEFI Profile | `uefi-profiles/poco/rodin/` (`v2026.07`, EL1 chainboot under GenieZone) |
| Maintainer | `YuriRinkashime` |
| State | Test #4 booted `XNULOADER v3` at EL1 (`rodin`); Phase 3 minimal XNU kernel built & staged into `XNUP` |

Nothing in this repository or its bring-up configuration writes to the phone's internal UFS storage. There is no `storage { }` node in `board.dts` and zero `ufs_write=1` boot arguments. Active slot `a` (`boot_a`, `init_boot_a`, `lk_a`) and `userdata` are never modified by anything here.

## Boot Chain, Hypervisor & Test #1 Root-Cause Fixes

1. **Unlocked Bootloader with Spoofed Lock State (`lk_a`/`lk_b`):** `lk_a` and `lk_b` run a custom HyperOS 4 Fenrir (`R0rt1z2/fenrir` `bl2_ext` EL3 patch) where `seccfg` is unlocked while `fastboot getvar unlocked` reports `no`. Do not treat `unlocked: no` as a locked device, and **never flash or patch `lk_a` or `lk_b`**.
2. **No `fastboot boot` or `kexec`:** MediaTek LK on `rodin` returns `FAILED (remote: 'unknown command')` for `fastboot boot`, and the stock/EVONIX Android kernel omits `CONFIG_KEXEC` and `CONFIG_KEXEC_FILE`.
3. **GenieZone Owns EL2 (EL1 Entry):** `ro.boot.hypervisor.version = GenieZone` owns EL2, so LK enters at **EL1** (`CONFIG_SKIP_LOWLEVEL_INIT=y`, `CONFIG_POSITION_INDEPENDENT=y`). Per `xnu-kernel-mtk-helio`, MediaTek ATF/GenieZone can leave `ICC_CTLR_EL1.EOImode` set on entry; clearing `EOImode` on all cores will be required before XNU's virtual timer (`PPI 27`) runs on hardware.
4. **Test #1 Hardware Findings & Fixes (Applied in `uefi-profiles/poco/rodin/`):**
   - **Centered Red/White Line Fixed (`ram_top` Collision):** Upstream `mt8195/init.c` clamped `ram_top` to `SZ_4G` (`0x100000000`), placing U-Boot's relocated binary, MMU tables, 32 MiB zeroed heap, and stack directly inside `mblock-17-framebuffer` (`0xfcc20000..0xffff0000`). Clamping `board_get_usable_ram_top()` to **`0xb8000000`** keeps U-Boot inside unreserved DRAM (`0x9fff0000..0xb7ffffff`).
   - **~20s Watchdog Reset Fixed (`toprgu@1c00b000`):** `dram_init()` now kicks `WDT_RESTART` (`0x1971` at `0x1c00b008`) and clears `WDT_MODE_EN` (bit 0 at `0x1c00b000` with unlock key `0x22000000`).
   - **Framebuffer Cache/Stride & Unmapped TrustZone Regions:** `armv8-mem-map.c` maps the framebuffer (`0xfcc00000`) and `ramoops` (`0x6ec00000`) as `MT_NORMAL_NC`, leaves `tee-reserved` (`0xee000000..0xfcc00000`) and `gz`/`mkp` (`>= 0x33d800000`) unmapped, and uses 32-pixel-aligned stride `1248 * 4 = 4992` bytes/line.
   - **GKI v4 `HKUB` Scan & Diagnostics:** `board_late_init()` scans `/chosen/linux,initrd-*` for `"HKUB"`, paints visual milestone bars + 16px white left ruler, and logs ASCII breadcrumbs to `pmsg-ramoops-0` (`0x6ee10000`).
5. **`PAYLOAD_BASE` Collision (`tee-reserved`):** `0xee200000..0xfc4fffff` collides with HakiOS's default `0xf0000000` `PAYLOAD_BASE`. Update `PAYLOAD_BASE` in `cmd/hakios.c` and rebuild `loader.efi` with `LOADER_CFLAGS="-DPAYLOAD_BASE=0xAC000000ULL"` before booting an `XNUP` payload on `rodin`.
6. **Live Test #4 (`XNULOADER v3` at EL1) & Phase 3 XNU Kernel Readiness:**
   - Test #4 booted `hakios-rodin-boot.img` + `hakios-rodin-init_boot.img` end-to-end into `loader.efi` (`XNULOADER v3`) at `CurrentEL=1` (`MIDR=410fd871`, `CNTFRQ=13000000`, display `1220x2712` at `0xfcc20000`, `platform rodin`, zero watchdog resets), stopping cleanly waiting for `XNUP` at `0xAC000000`.
   - **`soc.dtsi` UART Omission:** `serial@11001000` is MediaTek `mt6577-uart` (16550-style, clock-gated by LK `mtk_printk_ctrl.disable_uart=1`), not `arm,pl011`. Omitting `uart` from `soc.dtsi` prevents `loader.efi` from passing a bogus `arm,pl011` node to `pe_serial.c`.
   - **`board.dts` Console Bootargs:** Uses `bootargs = "-v", "gpu=0", "debug=0x144";` (removes `serial=3` and enables verbose framebuffer console).
   - **XNU Early Console / GICv3 / GenieZone EL1 Readiness (`xnu-12377.121.6`):**
     - `osfmk/console/video_console.c`: writes opaque `0xFF` alpha (`0xFFRRGGBB`) for 32bpp `vc_colors`, `vc_clear_screen()`, `vc_paint_char_32()`, and `vc_reverse_cursor()` so text is visible on MT6899 OVL.
     - `osfmk/arm64/gic_v3_irq.c`: clears `ICC_CTLR_EL1.EOImode` (bit 1) in `gic_v3_irq_cpu_init()` so `ICC_EOIR1_EL1` drops priority and deactivates PPI 27 under MediaTek ATF/GenieZone.
     - `osfmk/arm64/caches_asm.s` & `osfmk/arm/cpuid.c`: guards `CleanPoC_Dcache`, `CleanPoU_Dcache`, `FlushPoC_Dcache`, `Flush_Dcache`, `FlushPoU_Dcache` and forces `c_bulksize_op = 0` under `BCM2837` so XNU performs VA-only cache maintenance (zero `dc isw`/`dc csw`/`dc cisw` set/way instructions that trap under GenieZone EL2).
   - **`init_boot_b` 8 MiB Fit via `payload.gz`:** Minimal `qemu bringup` XNU (`mach.development.bcm2837`) + `XNUP` (`~16.95 MiB`) is gzipped (`payload.gz`, `~6.18 MiB`) inside `HKUB` so `hakios-rodin-init_boot.img` (`~6.27 MiB`) fits inside `init_boot_b` (`8 MiB`) and `cmd/hakios.c` decompresses `payload.gz` to `0xAC000000`.

## Bring-Up Status

| Subsystem | Feature / Boot Argument | State | Notes |
| --- | --- | --- | --- |
| `loader.efi` | `arch/arm64/boot/build.sh` (`rodin` in `platforms.h`) | Verified (Test #4) | `XNULOADER v3` reached at `CurrentEL=1` (`MIDR=410fd871`, `CNTFRQ=13 MHz`, `platform rodin`); rebuilt with `.uart = 0` |
| U-Boot (`poco/rodin`) | `uefi-profiles/poco/rodin/` (`v2026.07`) | Verified (Test #4) | Zero WDT resets, clean `0xb7000000` `ram_top`, VA-only CMO, `payload.gz` decompression to `0xAC000000` |
| Loader / Early Console | `bootargs`: `-v gpu=0 debug=0x144` | Built & staged | Minimal `qemu bringup` XNU built (`17,774,432` B), 0 set/way ops, `0xFF` alpha + `EOImode=0` |
| Display / Framebuffer | `loader.fb-base = 0xfcc20000`, `backlight.type = "soft"` | Not tested | `atag,videolfb`: `0xfcc20000`, `0x33d0000` (`51.81 MiB`), `60.00 Hz`, `o10_42_02_0a_dsc_vdo_lcm_drv` |
| CPU / SMP | `platform.cores = 8`, `psci = "smc"` | Not tested | Initial `boot.cpus = 1`; All-Big-Core `0x000..0x700` MPIDR layout |
| Touch | Disabled (`xiaomi_touch@0` / Goodix SPI) | Not tested | Documented only (`IRQ=9`, `RST=134`, `AVDD=73`, `1220×2712`); kept disabled |
| Buttons | Disabled (`mt6363keys` PMIC keys) | Not tested | Documented only (`KEY_POWER=0x74`, `KEY_VOLUMEDOWN=0x72`, `KEY_VOLUMEUP=0x73`) |
| GPU (Mali-G720 MC7) | `gpu=0` (software rendering) | Not tested | `mali@13000000` + `gpueb@13c00000` documented; CSF/GPUEB bring-up unstarted |
| Storage (UFS) | Read-only reference (`0x112b0000`) | Not tested | No `storage { }` section; UFS writes permanently disabled |
| Audio / Wi-Fi / BT / Modem / Camera / Thermal | — | Not tested | Excluded from initial scaffold |

## Extracted Hardware & Memory Map

All values below were extracted read-only from the running device's live `/sys/firmware/fdt`, `/proc/iomem`, `/proc/cmdline`, `/proc/cpuinfo`, `dmesg`, and bootloader dumps (never committed to this repository).

| Component | Extracted Value / Address | Source |
| --- | --- | --- |
| System RAM | Base `0x40000000`, Size `0x300000000` (12 GiB; `0x40000000`–`0x33fffffff` with `mblock` carve-outs) | FDT `/memory@40000000`, `/proc/iomem` |
| Framebuffer (`atag,videolfb`) | Base `0xfcc20000`, Size `0x033d0000` (`51.81 MiB`), `60.00 Hz`, `islcmfound = 1`, `lcmname = o10_42_02_0a_dsc_vdo_lcm_drv` | FDT `/chosen/atag,videolfb*`, `mblock-17-framebuffer` |
| Display Panel & Controller | `dispsys-config@14000000`, `ovlsys-config@14400000`/`14600000`, `dsi0@1420a000`, `1220×2712` (`4992` B/line 32-px aligned stride) | FDT `dsi0@1420a000`, `xiaomi_touch@0`, `/proc/cmdline` |
| Watchdog (`toprgu`) | `watchdog@1c00b000` (`mediatek,mt6899-wdt`, `WDT_MODE=+0x00`, `WDT_RESTART=+0x08`, `KEY=0x22000000`) | FDT `watchdog@1c00b000` |
| GICv3 | `GICD = 0x0c400000` (size `0x40000`), `GICR = 0x0c440000` (size `0x200000`) | FDT `/interrupt-controller@c400000`, `/proc/iomem` |
| ARM Generic Timer | `arm,armv8-timer`, `clock-frequency = 0x00c65d40` (13 MHz), PPIs `13, 14, 11, 10` (active-low) | FDT `/timer` |
| PSCI | `arm,psci-1.0`, `method = "smc"` | FDT `/psci` |
| AP UARTs | `0x11001000` (`uart0`), `0x11002000`, `0x11003000`, `0x11004000` (`mediatek,mt6577-uart`); cmdline sets `mtk_printk_ctrl.disable_uart=1` | FDT `serial@1100*000`, `/proc/iomem`, `/proc/cmdline` |
| Ramoops / Pstore | `ramoops.mem_address=0x6ec00000`, `mem_size=0x410000` (`console=0x6ec10000`, `pmsg=0x6ee10000`) | `/proc/cmdline`, `/proc/iomem` |
| CPU PAC Flags | Both `paca` and `pacg` present in `Features` | `/proc/cpuinfo` |
| Hypervisor / KVM | `/dev/kvm` absent; `ro.boot.hypervisor.version = GenieZone`, `kvm-arm.mode=protected` | `ls -la /dev/kvm`, `/proc/cmdline` |
| GPU (Mali-G720 MC7) | `mali@13000000` (`0x13000000`, size `0x480000`), GPUEB `0x13c00000`, `me_gpu_reserved` (`0x33da00000`), `me_GPUmputab_PMA` (`0xfc600000`) | FDT `mali@13000000`, `reserved-memory` |
| Touch IC (Disabled) | `xiaomi_touch@0` (`xiaomi,touch-spi`, Goodix `goodix_ts`): `IRQ = GPIO 9`, `RST = GPIO 134`, `AVDD = GPIO 73` | FDT `xiaomi_touch@0`, `dmesg` |
| Physical Keys (Disabled) | `mt6363keys` (`mediatek,mt6363-keys`): Power `0x74`, Vol-Down `0x72`, Vol-Up `0x73` | FDT `mt6363keys` |

## What Is Excluded (Never Committed)

- Stock Android DTB / DTS dumps and partition backups (`*.dtb`, `*.dtbo`, `*.img`, `boot-header.bin`, `rodin-fdt.*`)
- Built binary images and payloads (`out/`, `build/`, `*.payload`, `*.hkub`, `*.efi`)
- Xiaomi / MediaTek proprietary firmware blobs (`firmware/`, `mali_csffw*`, `adsp`, `scp`, `vcp`, `modem`)
- Apple IPSW files, Darwin userland binaries, APFS images, and device identities (`*.ipsw`, `*.apfs`, `*.im4p`, `identity.bin`)
