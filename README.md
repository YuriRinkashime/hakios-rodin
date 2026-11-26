# POCO X7 Pro (`rodin`)

| | |
| --- | --- |
| Device | POCO X7 Pro (`24129RT7CC`, codename `rodin`) |
| SoC | MediaTek Dimensity 8400-Ultra (`MT6899`), 8× Cortex-A725 (4 + 3 + 1 All-Big-Core); `soc.dtsi` |
| GPU | Arm Mali-G720 MC7 (`arm,mali-valhall`, `0x13000000`, GPUEB `0x13c00000`) |
| Memory | 12 GiB LPDDR5X (`0x40000000`–`0x33fffffff`) |
| Target Kernel | HakiOS `oosexgang/hakios-xnu-kernel` branch `common` (`xnu-12377.121.6` / Darwin 27.2) |
| Maintainer | `YuriRinkashime` |
| State | Initial board/SoC scaffold from live hardware dumps; **not yet booted on device** |

Nothing in this repository or its bring-up configuration writes to the phone's internal UFS storage. Testing is strictly RAM-only (`fastboot boot`), with no `storage { }` node in `board.dts` and zero `ufs_write=1` boot arguments; rebooting the device restores Android unmodified. It requires an unlocked bootloader.

## Bring-Up Status

| Subsystem | Feature / Boot Argument | State | Notes |
| --- | --- | --- | --- |
| Loader / Early Console | `bootargs`: `gpu=0 debug=0x144 serial=3` | Not tested | Scaffolded from live FDT and `/proc/iomem`; awaiting UEFI / MediaTek boot shim |
| Display / Framebuffer | `loader.fb-base = 0xfcc20000`, `backlight.type = "soft"` | Not tested | Preloader/LK framebuffer reserved at `0xfcc20000` (`51.8 MiB`); software dimming only |
| CPU / SMP | `platform.cores = 8`, `psci = "smc"` | Not tested | Initial `boot.cpus = 1`; All-Big-Core `0x000..0x700` MPIDR layout |
| Touch | Disabled (`xiaomi_touch@0` / Goodix SPI) | Not tested | Documented only (`IRQ=9`, `RST=134`, `AVDD=73`, `1220×2712`); kept disabled |
| Buttons | Disabled (`mt6363keys` PMIC keys) | Not tested | Documented only (`KEY_POWER=0x74`, `KEY_VOLUMEDOWN=0x72`, `KEY_VOLUMEUP=0x73`) |
| GPU (Mali-G720 MC7) | `gpu=0` (software rendering) | Not tested | `mali@13000000` + `gpueb@13c00000` documented; CSF/GPUEB bring-up unstarted |
| Storage (UFS) | Read-only reference (`0x112b0000`) | Not tested | No `storage { }` section; UFS writes permanently disabled |
| Audio / Wi-Fi / BT / Modem / Camera / Thermal | — | Not tested | Excluded from initial scaffold |

## Extracted Hardware & Memory Map

All values below were extracted read-only from the running device's live `/sys/firmware/fdt`, `/proc/iomem`, `/proc/cmdline`, `/proc/cpuinfo`, and `dmesg` (never committed to this repository).

| Component | Extracted Value / Address | Source |
| --- | --- | --- |
| System RAM | Base `0x40000000`, Size `0x300000000` (12 GiB; `0x40000000`–`0x33fffffff` with `mblock` carve-outs) | FDT `/memory@40000000`, `/proc/iomem` |
| Framebuffer (LK / Preloader) | Base `0xfcc20000`, Size `0x033d0000` (~51.81 MiB) | FDT `/chosen/atag,videolfb-fb_base_l`, `mblock-17-framebuffer` |
| Display Controller & Panel | `dispsys-config@14000000` (`0x14000000`, `0x14200000`, `0x14400000`), `dsi0@1420a000`, panel `o10_42_02_0a_dsc_vdo` (`1220×2712`; stride/pixel format TBD on early boot) | FDT `dsi0@1420a000`, `xiaomi_touch@0`, `/proc/cmdline` |
| GICv3 | `GICD = 0x0c400000` (size `0x40000`), `GICR = 0x0c440000` (size `0x200000`) | FDT `/interrupt-controller@c400000`, `/proc/iomem` |
| ARM Generic Timer | `arm,armv8-timer`, `clock-frequency = 0x00c65d40` (13 MHz), PPIs `13, 14, 11, 10` (active-low) | FDT `/timer` |
| PSCI | `arm,psci-1.0`, `method = "smc"` | FDT `/psci` |
| AP UARTs | `0x11001000` (`uart0`), `0x11002000`, `0x11003000`, `0x11004000` (`mediatek,mt6577-uart`); Android cmdline sets `mtk_printk_ctrl.disable_uart=1` | FDT `serial@1100*000`, `/proc/iomem`, `/proc/cmdline` |
| Ramoops / Pstore | `ramoops.mem_address=0x6ec00000`, `mem_size=0x410000` (`pmsg_size=0x200000`, `console_size=0x200000`) inside `0x6cc00000`–`0x6f00ffff` reserved | `/proc/cmdline`, `/proc/iomem` |
| CPU PAC Flags | Both `paca` and `pacg` present in `Features` | `/proc/cpuinfo` |
| Hypervisor / KVM | `/dev/kvm` not present (`No such file or directory`); kernel booted with `kvm-arm.mode=protected` | `ls -la /dev/kvm`, `/proc/cmdline` |
| GPU (Mali-G720 MC7) | `mali@13000000` (`0x13000000`, size `0x480000`), GPUEB `0x13c00000`, `mblock-20-me_gpu_reserved` at `0x33da00000` (`2 MiB`), `mblock-19-me_GPUmputab_PMA` at `0xfc600000` (`6 MiB`) | FDT `mali@13000000`, `reserved-memory`, `/proc/iomem` |
| Touch IC (Disabled) | `xiaomi_touch@0` (`xiaomi,touch-spi`, Goodix `goodix_ts`): `IRQ = GPIO 9`, `RST = GPIO 134`, `AVDD = GPIO 73` | FDT `xiaomi_touch@0`, `dmesg` |
| Physical Keys (Disabled) | `mt6363keys` (`mediatek,mt6363-keys`): Power `0x74`, Vol-Down `0x72`, Vol-Up `0x73` | FDT `mt6363keys` |

## MediaTek Early Init & Mali-G720 Alignment Notes

Synthesized from upstream MediaTek porting notes (`origin/mtk:docs/mtk-porting.md`, `xnu-kernel-mtk-helio`) and the Pixel 8 (`shiba`) Mali-G715 device port:

1. **RAM-Only Boot & Loader Handoff:** `hakios-xnu-kernel` (`origin/common`) targets Qualcomm platforms out of the box via a UEFI loader (`arch/arm64/boot/`), whereas MediaTek devices either chainboot via a custom `mtkloader` stub placed ahead of the kernel (`0x40080000` on MT6768) or require an MT6899 UEFI/chainboot shim passed via `fastboot boot` (RAM-only). Keep `boot.cpus = <1>` and `features = <>` until GICv3 (`0x0c400000` / `0x0c440000`), the 13 MHz ARMv8 timer, and the 4+3+1 All-Big-Core PSCI `smc` bring-up are verified.
2. **Early Logging without Exposed UART:** Like other Xiaomi/MediaTek production phones, `rodin` boots with `mtk_printk_ctrl.disable_uart=1` in `/proc/cmdline` and no exposed physical UART pad. Early MediaTek HakiOS bring-up relies on **framebuffer console output** (`0xfcc20000`) and/or **ramoops/pstore** (`0x6ec00000`, `0x410000` bytes) to recover kernel panic logs across a warm reset back to Android, without ever touching UFS.
3. **No Qualcomm Drivers:** Zero Qualcomm drivers (`cpufreq-epss`, `ddr-rpmh`, `buttons-pmic`, `gpu-a660`, `adsp`, `spmi`, `tlmm`) apply to `MT6899`. `soc.dtsi` intentionally omits all Qualcomm-only blocks so `tools/boards.py check` enforces a clean SoC boundary.
4. **Mali-G720 MC7 vs. Adreno A660 & Earlier Mali Ports:**
   - `xnu-kernel-mtk-helio` (MT6768) targets an older Job-Manager **Mali-G52** (`gpu-mali-g52`), whereas **Mali-G720 MC7** on MT6899 is a 5th-Gen CSF GPU paired with MediaTek's **GPUEB** microcontroller (`0x13c00000`) and dedicated GPU reserved regions (`me_gpu_reserved`, `me_GPUmputab_PMA`).
   - Like the Pixel 8 (`shiba`) **Mali-G715** CSF bring-up (`mali_csffw`), CSF-based Mali GPUs initialize an MCU firmware image and command stream groups rather than Adreno GMU/A660 registers. Keep `gpu=0` (software rendering) until early kernel boot, MMU, and display handover are stable.

## What Is Excluded (Never Committed)

- Stock Android DTB / DTS dumps (`*.dtb`, `*.dtbo`, `phone.dts`, `rodin-fdt.*`)
- Xiaomi / MediaTek proprietary firmware blobs (`firmware/`, `mali_csffw*`, `adsp`, `scp`, `vcp`, `modem`)
- Apple IPSW files, Darwin userland binaries, APFS images, and device identities (`*.ipsw`, `*.apfs`, `*.im4p`, `identity.bin`)
- Built payloads outside published `images/` (`build/`, `*.payload`)
