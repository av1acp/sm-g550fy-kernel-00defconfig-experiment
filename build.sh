#!/usr/bin/env bash
#
# Stock-equivalent kernel build — Samsung SM-G550FY (o5prolte, On5 Pro 2016)
# Source: Exynos3475/android_kernel_samsung_exynos3475 branch cm-14.1
# Kernel 3.10.9, ARCH=arm (32-bit), defconfig: o5lteswa_00_defconfig (official FY)
#
# Verified facts this script relies on (checked 2026-09-23):
#   * Device /proc/version: "3.10.9-13870322 ... gcc version 4.8"  (via adb)
#   * This tree's Makefile:3.10.9 — same base as stock
#   * o5lteswa_00_defconfig: LOCALVERSION="" + AUTO=y, # CONFIG_MODULES is not set,
#     CONFIG_BUILD_ARM_APPENDED_DTB_IMAGE=y + exynos3475-universal3475
#     -> KBUILD_IMAGE = zImage-dtb (arch/arm/Makefile)
#   * Host gcc >= 10 fails at scripts/dtc link: duplicate `yylloc`
#     (-fno-common default). Fix = extern the parser copy (idempotent below).
#   * AOSP arm-eabi-4.8 marshmallow-release =110 MB, ELF x86-64
#     (stock FY /proc/version says gcc 4.8 as well).
#   * Same repack pipeline used for boot.img: stock ramdisk (Magisk) +
#     stock DTBH (device DTB) kept byte-identical, only kernel swapped.
#
set -euo pipefail
cd "$(dirname "$0")"

ARCH=arm
DEFCONFIG=o5lteswa_00_defconfig
LOCALVER="-13870322"                       # reproduces stock utsrelease
# KSU BRANCH: KernelSU requires gcc >= 4.9 (their static_assert), so this
# branch builds with arm-eabi-4.9 (burstlam mirror; ELF x86-64 verified).
# Fallback: official AOSP arm-linux-androideabi-4.9 (prefix auto-detected).
TOOLCHAIN_DIR="${TOOLCHAIN_DIR:-$HOME/toolchains/arm-eabi-4.9}"
PRIMARY_URL="https://github.com/burstlam/arm-eabi-4.9"
FALLBACK_URL="https://android.googlesource.com/platform/prebuilts/gcc/linux-x86/arm/arm-linux-androideabi-4.9"

export ARCH
export LOCALVERSION="$LOCALVER"

# KSU Manager signature gate: the kernel pre-authorizes exactly ONE
# manager certificate (KSU's "failed to grant root" = cert mismatch).
# Values below = signer cert of the Actions-built API25 manager APK
# (KernelSU_bb0be92_29918-release.apk, minSdk25)
# (apksigner: SHA-256 digest + DER length867 bytes). Upstream defaults
# (0x033b/c371...) belong to a different signing key -> rejected ours.
export KSU_EXPECTED_SIZE=744
export KSU_EXPECTED_HASH="d0161c1b3cb0dae4cdbbc8371bba636435232f0f8837eac3a305625961aac0b2"

log() { echo "[kernel-build] $*"; }

# ---------------------------------------------------------------- sanity ----
if [ ! -f "arch/arm/configs/$DEFCONFIG" ]; then
  log "ERROR: arch/arm/configs/$DEFCONFIG not found"
  exit 1
fi

# ------------------------------------------------------------- toolchain ----
if [ ! -x "$TOOLCHAIN_DIR/bin/arm-eabi-gcc" ] && [ ! -x "$TOOLCHAIN_DIR/bin/arm-linux-androideabi-gcc" ]; then
  log "fetching toolchain -> $TOOLCHAIN_DIR"
  mkdir -p "$(dirname "$TOOLCHAIN_DIR")"
  rm -rf "$TOOLCHAIN_DIR"
  if ! git clone --quiet --depth=1 "$PRIMARY_URL" "$TOOLCHAIN_DIR"; then
    log "primary clone failed — fallback: $FALLBACK_URL"
    rm -rf "$TOOLCHAIN_DIR"
    git clone --quiet --depth=1 "$FALLBACK_URL" "$TOOLCHAIN_DIR"
  fi
fi

# auto-detect CROSS prefix from whichever gcc landed on disk
if [ -x "$TOOLCHAIN_DIR/bin/arm-eabi-gcc" ]; then
  CROSS="$TOOLCHAIN_DIR/bin/arm-eabi-"
elif [ -x "$TOOLCHAIN_DIR/bin/arm-linux-androideabi-gcc" ]; then
  CROSS="$TOOLCHAIN_DIR/bin/arm-linux-androideabi-"
else
  log "ERROR: no usable cross gcc in $TOOLCHAIN_DIR/bin"
  exit 1
fi
if ! "${CROSS}gcc" --version >/dev/null 2>&1; then
  log "ERROR: ${CROSS}gcc is not executable on this host"
  exit 1
fi
log "compiler: $("${CROSS}gcc" --version | head -1)"
export CROSS_COMPILE="$CROSS"
export KBUILD_BUILD_USER="${KBUILD_BUILD_USER:-builder}"
export KBUILD_BUILD_HOST="${KBUILD_BUILD_HOST:-$(hostname)}"

# ----------------------------------------- host-tool fix: dtc yylloc --------
# gcc >= 10 defaults to -fno-common -> bison+lex both define yylloc -> link
# error "multiple definition of `yylloc'". Fix = extern the parser copy.
if grep -q '^YYLTYPE yylloc;$' scripts/dtc/dtc-parser.tab.c_shipped; then
  sed -i 's/^YYLTYPE yylloc;$/extern YYLTYPE yylloc;/' scripts/dtc/dtc-parser.tab.c_shipped
  if grep -q '^YYLTYPE yylloc;$' scripts/dtc/dtc-parser.y; then
    sed -i 's/^YYLTYPE yylloc;$/extern YYLTYPE yylloc;/' scripts/dtc/dtc-parser.y
  fi
  log "applied dtc yylloc extern fix"
fi

# ----------------------------------------------------------- configure ------
log "make $DEFCONFIG"
make "$DEFCONFIG"

# Reproduce the stock release string exactly. AUTO would append a git hash.
if grep -q '^CONFIG_LOCALVERSION_AUTO=y' .config; then
  sed -i 's/^CONFIG_LOCALVERSION_AUTO=y/# CONFIG_LOCALVERSION_AUTO is not set/' .config
  log "disabled CONFIG_LOCALVERSION_AUTO"
fi

# ---------------------------------------------------------------- build -----
J="$(nproc)"
log "make -j$J zImage-dtb   (KBUILD_IMAGE for this config)"
make -j"$J" zImage-dtb 2>&1 | tee build.log     # pipefail propagates errors

IMG="arch/arm/boot/zImage-dtb"
if [ ! -s "$IMG" ]; then
  log "ERROR: $IMG missing or empty"
  exit 1
fi

RELEASE="$(cat include/config/kernel.release)"
EXPECTED="3.10.9${LOCALVER}"
if [ "$RELEASE" != "$EXPECTED" ]; then
  log "ERROR: kernel.release='$RELEASE' expected '$EXPECTED'"
  exit 1
fi
log "kernel.release = $RELEASE  (matches stock FY) OK"

# -------------------------------------------------------------- package ------
mkdir -p dist
cp "$IMG"          dist/zImage-dtb
cp .config         dist/kernel.config
cp System.map      dist/System.map

GIT_REV="$(git rev-parse --short HEAD 2>/dev/null || echo no-git)"
{
  echo "device      : SM-G550FY (o5prolte) / universal3475"
  echo "source      : Exynos3475/android_kernel_samsung_exynos3475 @ cm-14.1"
  echo "kernel      : $RELEASE"
  echo "defconfig   : $DEFCONFIG (official FY)"
  echo "compiler    : $("${CROSS}gcc" --version | head -1)"
  echo "source-git  : $GIT_REV"
  echo "built-utc   : $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "runner      : ${GITHUB_RUN_ID:-local}/${GITHUB_RUN_ATTEMPT:-0}"
  echo "image-sha256: $(sha256sum "$IMG" | cut -d' ' -f1)"
} > dist/build-info.txt

( cd dist && sha256sum zImage-dtb kernel.config System.map > SHA256SUMS )

log "done:"
ls -la dist/
log "artifacts staged in dist/"
