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
| State | Test #17 confirmed zero ghost touches on `rodin` (`8` cores, `5007 MB`, UFS RO, `3×` MT6363 buttons); Phase 5C-7 lowers interior THP thresholds to `300..450` (with dedicated `>= 550` `row 39` edge guard, quiet-only baseline drift, and idle `max_peak` telemetry) so real finger taps fire cleanly without `row 39` ghosts |

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
7. **Live Test #5 (Full Darwin/XNU `12377.121.6` Kernel Boot) & Live Test #6 (Full RAM-Only Userspace PID 1):**
   - Test #5 booted `hakios-rodin-boot.img` + `hakios-rodin-init_boot.img` through U-Boot -> `loader.efi` -> XNU `12377.121.6` (`pmap`, `VM`, `IONVRAM`, `minicrypto`, `MAC Framework`, `GenericARMPE`, `GenericARMCPU` `cpu 0`, `lo0`, `MiniIOMobileFramebuffer` `1125x2436` on `1220x2712` panel with `4992` B/row stride and crisp `0xFF` white-on-black text), reaching `GenericARMPE: all started: 1 processor running` and pausing cleanly at `Failed to mount root device` (no rootfs staged in Phase 3).
   - **Self-Contained `md0` APFS Rootfs (`tools/qemu-root.sh`):** Phase 4 built a `4,194,304` B (`4 MiB`, `54,379` B gzip-9) APFS container image (`tools/apfs/mkapfs.py`) holding `/dev`, `/etc/motd`, `/mnt`, `/tmp`, and static freestanding arm64 `/sbin/launchd` (`tools/qemu-init/*.c`, zero Apple/IPSW files) packed into `XNUP` (`21,969,016` B uncompressed, `0xAC000000..0xAD4F3878`, `155.05 MiB` below `0xB7000000`) with `rd=md0`. Gzipped `payload.gz` (`6,530,872` B) + `loader.efi` (`87,040` B) in `HKUB` kept `hakios-rodin-init_boot.img` (`6,631,424` B) inside `init_boot_b` (`8 MiB`), with zero UFS storage writes.
   - **Live Test #6 Verified (`slot b`, `rd=md0`):** Booted cleanly to APFS `md0` and executed `/sbin/launchd` (PID 1) in ~3 seconds: `board: rodin (POCO X7 Pro)`, `processors: 1`, `memory: 447 MB` (`memory: free 392, wired 54 MB` heartbeat ticking every ~20s with zero panics or watchdog resets), `the kernel ran exec, devfs and the console for it: all is well.` -> `# ` shell prompt.
8. **Phase 5A (Multi-GB Clean Rank-1 DRAM & 8-Core Cortex-A725 PSCI SMP):**
   - **5 GiB Clean Rank-1 DRAM (`0x1c0000000..0x300000000`):** Exposes Bank 2 (`0x1c0000000`, size `0x140000000` = `5120 MiB`) in U-Boot (`armv8-mem-map.c`, `mt6899-poco-rodin.dts`, `CONFIG_NR_DRAM_BANKS=3`) while keeping U-Boot `ram_top` clamped at `0xb7000000` and `PAYLOAD_BASE` at `0xAC000000`. Sits completely clear of all EMI-MPU `no-map` carve-outs (above `dramc-rk0@1bfff0000` and `984 MiB` below `mblock-23-mkp@33d800000` / `mblock-21-gz@33dc00000`). `loader.efi` reserves the top 1 MiB (`PANIC_LOG_RESERVE` at `0x2fff00000`) and hands XNU `physBase = 0x1c0000000`, `memSize = 0x13ff00000` (`5119 MB`).
   - **8-Core PSCI SMP (`cpus=8`, `small_cores_after=0`):** Enables all 8 Cortex-A725 cores (`0x000..0x700` MPIDR Aff1 stride `core-shift = 8`, `psci = "smc"`, `GICR` at `0x0c440000` size `0x200000`). Every secondary core runs `gic_v3_irq_cpu_init()` (clearing `ICC_CTLR_EL1.EOImode`) and uses VA-only cache ops (`dc set/way` count = 0).
   - **Live Test #7 Verified (`slot b`, `rd=md0`):** Booted Darwin/XNU `12377.121.6` in ~3 seconds to `/sbin/launchd` (PID 1) with `board: rodin (POCO X7 Pro)`, `processors: 8` (`GenericARMPE: all started: 8 processors running`), and `memory: 5007 MB` (`free ~5004 MB` steady heartbeat, APFS scaled to 4 threads + 16 KiB pages, zero panics/resets).
9. **Phase 5B, 5B-2 & 5B-3 (Strictly Read-Only MT6899 UFSHCI `0x112b0000` Bring-Up & JEDEC Init):**
   - **Hardware Safety Audit (`rodin-fdt.dts`, `/proc/iomem`, `dmesg`):** `ufshci@112b0000` (`size 0x2a00`) is a JEDEC UFSHCI controller with zero `iommus` property (bypasses MediaTek SMMU; `/soc` `dma-ranges` covers `64 GiB`), non-coherent DMA (requires `VM_WIMG_WCOMB` `lowdma` scratch or VA-only `dc civac`), always-on PMIC rails (`mt6363_vemc`, `mt6363_vufs12`, `dvfsrc-vcore`, no `va09-supply`), and 3 UFS LUNs (`sda`/`sdb`/`sdc`).
   - **Live Test #8 Verified (`slot b`, `rd=md0`):** Booted cleanly to 8 cores / `5007 MB` / `/sbin/launchd` (`all is well.`) with zero bus faults. `ufshc_standalone_probe(0x112b0000)` verified `hc_whitelist_selftest()` (35 cases) and read out `CAP=55c7051f` (32 slots, `64bit=yes`, `MCQ=yes`), `VER=00000400` (UFSHCI 4.0), `HCPMID=00000426` (MediaTek), `MCQCFG=00001f00` (legacy single-UTRL mode active), and `HCE=0` (cleanly disabled by LK).
   - **Live Test #9 Verified (`slot b`, `rd=md0`, Phase 5B-2):** Booted cleanly to 8 cores / `5007 MB` / `/sbin/launchd` (`all is well.`) with zero crashes. Confirmed `MTK_REFCLK=00000003 MTK_IP_VER=10450100`, `HCE=1 ready`, `mtk pre-link: lpm=0 lcc=0 spc=0 (val=00000250)`, `MTK device reset SMC r0=0`, `DME_LINKSTARTUP ok on try 3 (ARG2=00000000 HCS=0000000f)`, `link up! HCE=00000001 HCS=0000000f IS=00000400 MCQCFG=00001f00`, `DMA scratch pa=0x48a00000 (lowdma WCOMB)`, `NOP OUT -> NOP IN ok (try 1)`, `LUN 0/2 TUR st=02 sk=02` (post-reset `NOT READY` awaiting `fDeviceInit`), `LUN 1 TUR st=02 sk=05`, and `controller disabled cleanly (HCE=00000000 HCS=00000000)`.
   - **Phase 5B-3 JEDEC `fDeviceInit` & Full Sense/GPT Scan (`ufs_ro_probe=0x112b0000`):** Asserts/deasserts `UFS_MTK_SIP_DEVICE_RESET` up front before `HCE=1` so `DME_LINKSTARTUP` succeeds on try 1; runs `hc_ro_query_selftest()` (10 cases) allowing **only** `SET_FLAG`/`READ_FLAG` on `fDeviceInit (0x01)` and `READ_DESC(DEVICE=0x00)` while `ufshc_permitted()` continues blocking all general Query/NOP/Task Management UPIUs and all SCSI write/format/unmap/start-stop/sync-cache/mode-select operations; polls `fDeviceInit` clear (`<= 500 ms`), logs Device Descriptor (`bInitPowerMode`, spec, mfg, LUN counts), captures full SCSI sense (`sk`/`asc`/`ascq`) with bounded `TUR` retries (`sk=06`, `sk=02 asc=04 ascq=01`), reads `INQUIRY`, `READ CAPACITY(10/16)`, and LBA 0/1 + GPT partition entries (`boot_a`, `boot_b`, `init_boot_a`, `init_boot_b`, `super`, `userdata`, etc.), and restores `HCE=0` cleanly before `/sbin/launchd`.
   - **Live Test #10 & Phase 5B-4 (`WCOMB` / Device-GRE Alignment Fault Fix & Read-Only GPT Enumeration):** Test #10 verified `MTK pre-HCE device reset SMC r0=0`, `HCE=1 ready`, `DME_LINKSTARTUP ok on try 1 (ARG2=00000000 HCS=0000000f)`, `NOP OUT -> NOP IN ok (try 1)`, `fDeviceInit ok in 0 polls (0 ms)`, `dev desc: len=0x59 spec=0x0400 mfg=0x01ce luns=0x03 wlu=0x04 boot=0x01 initPM=0x01`, and `LUN 0: SAMSUNG KLUEG4RHHD-B0G1 1200, 1024 blocks x 4096 B (4 MB)`, then hit `Unaligned kernel data abort` (`ESR 0x96000021`) inside `hc_ro_probe_gpt()` -> `hc_ro_exec()` because `bzero(buf, 4096)` executed `dc zva` on `VM_WIMG_WCOMB` (`Device-GRE`) scratch memory. Phase 5B-4 replaces all direct `WCOMB` `bzero`/`memcpy`/`memcmp`/CRC/auto-vectorized accesses in `hc_ro_exec()`, `hc_ro_nop_out()`, `hc_ro_query()`, `hc_ro_probe_gpt()`, and `ufshc_standalone_probe()` with strictly byte-by-byte volatile helpers (`hc_dev_copy_from()`, `hc_dev_copy_to()`, `hc_dev_zero()`), stages DMA reads into normal cached kernel RAM before parsing MBR/GPT/CRC-32, logs `LUN N: no GPT (raw/boot LUN)` cleanly for boot LUNs 0/1, and summarizes LUN 2 (`sdc`) GPT capacity, CRCs, free trailing LBAs, and key partitions without flooding `/sbin/launchd`.

## Bring-Up Status

| Subsystem | Feature / Boot Argument | State | Notes |
| --- | --- | --- | --- |
| `loader.efi` | `arch/arm64/boot/build.sh` (`rodin` in `platforms.h`) | Verified (Test #4–#6) | `XNULOADER v3` at `CurrentEL=1` (`MIDR=410fd871`, `CNTFRQ=13 MHz`, `platform rodin`, `.uart = 0`) |
| U-Boot (`poco/rodin`) | `uefi-profiles/poco/rodin/` (`v2026.07`) | Verified (Test #4–#6); 5 GiB bank staged | Zero WDT resets, clean `0xb7000000` `ram_top`, VA-only CMO, 3-bank DRAM (`5120 MiB` Rank-1 bank) |
| Loader / Early Console | `bootargs`: `cpus=8 -v gpu=0 debug=0x144 rd=md0 small_cores_after=0 ufs_ro_probe=0x112b0000 buttons=1 touchscreen=1` | Verified (Test #5–#12, #13 regression fixed) | GICv3 `EOImode=0` + PPI 27 + VA-only cache ops + `0xFF` alpha verified on device |
| Display / Framebuffer | `loader.fb-base = 0xfcc20000`, `backlight.type = "soft"` | Verified (Test #5–#13) | `MiniIOMobileFramebuffer` `1125x2436` on `1220x2712` (`4992` B/row) rendering kernel console cleanly |
| RAM-only Userspace | `rd=md0` (`tools/qemu-root.sh` APFS + `/sbin/launchd`) | Verified (Test #6–#13) | Self-contained static arm64 `/sbin/launchd` (PID 1) via `/dev/console` (`# `); zero UFS writes |
| CPU / SMP | `platform.cores = 8`, `boot.cpus = 8`, `psci = "smc"` | Verified (Test #7–#12; `cpus=8` restored in 5C-3) | 8× Cortex-A725 All-Big-Core `0x000..0x700` + `5007 MB` Rank-1 DRAM verified on device |
| Touch | Polled `xiaomi_touch@0` (`goodix.c` MT6899 backend, `touchscreen=1`) | Zero ghosts verified (Test #17); Phase 5C-7 staged | `spi1@11011800` (`0x100`), `gpio@10005000` (`IRQ=9`, `AVDD=73`, `RST=134`), `mt6368_vtp` `1.8 V` (`SID 5`), `GT9916K` `05.80.07.1a` THP (`18x40` `diff` @ `0x10d5c`, interior `noise*2+80` `300..450`, `row 39 >= 550` guard, 2-frame confirm, quiet-only drift) |
| Buttons | Polled read-only `mt6363keys` (`buttons=1`) | Verified (Test #12) | `spmi@1cc04000` (`pmif` `SWINF_2` `0x0880..0x08a8`), MT6363 `SID 4` `SWCID 0x000b=0x63`, `TOPSTATUS 0x001e` bits `1/3/4` -> `/dev/hakios-touch` + console (`VOL_UP`, `VOL_DOWN`, `POWER` down/up verified) |
| GPU (Mali-G720 MC7) | `gpu=0` (software rendering) | Not tested | `mali@13000000` + `gpueb@13c00000` documented; CSF/GPUEB bring-up unstarted |
| Storage (UFS) | Read-only probe (`0x112b0000`, `ufs_ro_probe=0x112b0000`) | Verified (Test #11, zero writes) | `DME_LINKSTARTUP` try 1, `NOP IN`, `fDeviceInit` (2 ms), `SAMSUNG KLUFG4NHHD-F0G1 1200`: LUN 0/1 raw 8 MB, LUN 2 GPT 93/93 (`hdr_crc=0bf42308`, `tbl_crc=8870a302`), `HCE=0` restored |
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
