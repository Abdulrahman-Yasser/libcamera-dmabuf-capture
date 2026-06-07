#!/bin/bash
# Cross-compile for aarch64-agl-linux using the Yocto sysroots-components.
# No image rebuild needed — scp the binary to the RPi.
#
# Usage:
#   chmod +x cross-build.sh && ./cross-build.sh
#   scp libcamera-dmabuf-capture root@<rpi-ip>:/home/

set -euo pipefail

COMP=/media/abdu/LinuxHome/Embedded_Linux/git_ignoring/AGL/raspberrypi4/tmp/sysroots-components

# ── Cross tools ───────────────────────────────────────────────────────────────
GCC_BIN=${COMP}/x86_64/gcc-cross-aarch64/usr/bin/aarch64-agl-linux
CXX=${GCC_BIN}/aarch64-agl-linux-g++

BINUTILS_BIN=${COMP}/x86_64/binutils-cross-aarch64/usr/bin/aarch64-agl-linux

# Tell the gcc driver where to find cc1plus (the hard-coded sandbox path is
# gone after rm_work; GCC_EXEC_PREFIX + COMPILER_PATH work around that).
export GCC_EXEC_PREFIX="${COMP}/x86_64/gcc-cross-aarch64/usr/libexec/aarch64-agl-linux/"
export COMPILER_PATH="${COMP}/x86_64/gcc-cross-aarch64/usr/libexec/aarch64-agl-linux/gcc/aarch64-agl-linux/13.4.0"

# The bare 'as' in libexec is a broken symlink after rm_work. Create a temp
# wrappers dir with bare-named symlinks → real binutils binaries.
WRAPPERS=$(mktemp -d /tmp/aarch64-wrappers-XXXXXX)
for tool in as ld ar nm objcopy objdump strip ranlib; do
    real="${BINUTILS_BIN}/aarch64-agl-linux-${tool}"
    [ -f "$real" ] && ln -sf "$real" "${WRAPPERS}/${tool}"
done
export PATH="${WRAPPERS}:${GCC_BIN}:${BINUTILS_BIN}:${PATH}"
trap "rm -rf '${WRAPPERS}'" EXIT

# ── Target sysroot components ─────────────────────────────────────────────────
GLIBC=${COMP}/aarch64/glibc
LINUX_HDR=${COMP}/aarch64/linux-libc-headers
GCC_RT=${COMP}/aarch64/gcc-runtime
LIBGCC=${COMP}/aarch64/libgcc
LIBCAM=${COMP}/aarch64/rpi-libcamera
GCC_INCDIR=${COMP}/x86_64/gcc-cross-aarch64/usr/lib/aarch64-agl-linux/gcc/aarch64-agl-linux/13.4.0/include

# Collect rpath-link dirs for libcamera's transitive deps
# (udev→systemd, gnutls→nettle→gmp, yaml, …)
RPATH_LINK_FLAGS=()
while IFS= read -r -d '' libdir; do
    RPATH_LINK_FLAGS+=("-Wl,-rpath-link,${libdir}")
done < <(find "${COMP}/aarch64" -maxdepth 3 -name "lib" -path "*/usr/lib" -print0 2>/dev/null)

# ── Sanity checks ─────────────────────────────────────────────────────────────
for path in "$CXX" "$BINUTILS_BIN/aarch64-agl-linux-as" "$GLIBC" "$GCC_RT" "$LIBCAM"; do
    if [ ! -e "$path" ]; then
        echo "ERROR: not found: $path"
        exit 1
    fi
done

echo "[cross-build] compiler : $CXX"
echo "[cross-build] libcamera: $LIBCAM"
echo "[cross-build] compiling main.cpp ..."

"${CXX}" \
    -std=c++17 -O2 -Wall \
    -B "${WRAPPERS}/" \
    -B "${LIBGCC}/usr/lib/aarch64-agl-linux/13.4.0/" \
    --sysroot="${GLIBC}" \
    \
    -isystem "${GCC_INCDIR}" \
    -isystem "${GCC_INCDIR}-fixed" \
    -isystem "${LINUX_HDR}/usr/include" \
    -isystem "${GCC_RT}/usr/include/c++/13.4.0" \
    -isystem "${GCC_RT}/usr/include/c++/13.4.0/aarch64-agl-linux" \
    -I "${LIBCAM}/usr/include" \
    -I "${LIBCAM}/usr/include/libcamera" \
    \
    -L "${LIBGCC}/usr/lib/aarch64-agl-linux/13.4.0" \
    -L "${LIBGCC}/usr/lib" \
    -L "${GCC_RT}/usr/lib" \
    -L "${LIBCAM}/usr/lib" \
    -Wl,-rpath-link,"${LIBGCC}/usr/lib/aarch64-agl-linux/13.4.0" \
    -Wl,-rpath-link,"${LIBGCC}/usr/lib" \
    -Wl,-rpath-link,"${GCC_RT}/usr/lib" \
    -Wl,-rpath-link,"${LIBCAM}/usr/lib" \
    "${RPATH_LINK_FLAGS[@]}" \
    \
    main.cpp \
    -lcamera -lcamera-base \
    -lstdc++ -lm \
    \
    -o libcamera-dmabuf-capture

echo "[cross-build] done: $(file libcamera-dmabuf-capture)"
echo ""
echo "Deploy:"
echo "  scp libcamera-dmabuf-capture root@<rpi-ip>:/home/"
echo "  ssh root@<rpi-ip> ./libcamera-dmabuf-capture"
