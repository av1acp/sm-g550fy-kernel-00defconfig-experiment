# SM-G550FY kernel (o5prolte) — GitHub Actions build

Kernel for **SM-G550FY (Galaxy On5 Pro 2016, India)** built automatically on
GitHub Actions.

- Source: `Exynos3475/android_kernel_samsung_exynos3475` branch **cm-14.1**
  (the community tree behind the stable LOS14.1 builds for o5prolte — proven
  on this hardware — carrying the **official Samsung `o5lteswa_00_defconfig`**)
- Kernel: **3.10.9** (`3.10.9-13870322`, exact stock release string preserved)
- Arch: **arm 32-bit** (Exynos 7570 / universal3475)
- Toolchain: **AOSP arm-eabi-4.8** (`marshmallow-release`), fallback
  `burstlam/arm-eabi-4.9` — matches stock `/proc/version` (`gcc version 4.8`)
- Output: `arch/arm/boot/zImage-dtb` (= zImage + appended
  `exynos3475-universal3475.dtb`)
- Modules: none — `# CONFIG_MODULES is not set`

## Run a build

Actions tab → **Build kernel (SM-G550FY / o5prolte)** → **Run workflow**.
Manual triggers only. On a **public** repo standard runners are free &
unlimited.

Artifact `o5lteswa-kernel-<run>`: `zImage-dtb`, `kernel.config`,
`System.map`, `build-info.txt`, `SHA256SUMS`. Failures upload
`build-log-<run>`.

## boot.img repack (how the flashable image is made)

The artifact kernel is swapped into the device's own boot container:

- header: only `kernel_size` patched (Samsung LE header, page=2048)
- ramdisk: **byte-identical** → Magisk/root preserved
- `DTBH` section (device's own DTB): **byte-identical**
- kernel: new plain zImage (appended DTB dropped — device uses its DTBH)

Verified per-repack: header magic, field patch, kernel sha256, ramdisk
byte-equality, DTBH byte-equality, alignment, section-chain sum.

## Flashing (device currently on stock)

Commands + rollback live next to the build outputs; always keep
`boot_stock` backup before flashing, verify readback md5, then reboot.
