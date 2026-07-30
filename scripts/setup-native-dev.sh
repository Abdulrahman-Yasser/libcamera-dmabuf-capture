#!/usr/bin/env bash
# Native x86_64 dev build: lets --src file-mode (no live camera) run and be
# screenshotted on a regular Linux desktop GPU, without the AGL-SDK aarch64
# cross toolchain or target hardware. Builds a minimal libcamera from source
# into $HOME/.local (no sudo -- Ubuntu's packaged libcamera-dev is a 2020
# snapshot too old to match this project's API) and configures build-native/
# against it. Idempotent: safe to re-run, skips work that's already done.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBCAMERA_TAG="v0.4.0"
LIBCAMERA_SRC="$HOME/.cache/libcamera-native/libcamera"
LIBCAMERA_PREFIX="$HOME/.local/libcamera-install"
PKGCONFIG_DIR="$LIBCAMERA_PREFIX/lib/x86_64-linux-gnu/pkgconfig"
SHIM_DIR="$LIBCAMERA_PREFIX/extra-pkgconfig"

export PATH="$HOME/.local/bin:$PATH"

echo "== [1/5] Python build deps (meson/jinja2/ply/pyyaml) =="
if ! command -v meson >/dev/null 2>&1; then
    pip3 install --user --quiet meson jinja2 ply pyyaml
else
    echo "meson already installed: $(meson --version)"
fi

echo "== [2/5] libcamera source ($LIBCAMERA_TAG) =="
if [ ! -d "$LIBCAMERA_SRC" ]; then
    mkdir -p "$(dirname "$LIBCAMERA_SRC")"
    git clone --depth 1 --branch "$LIBCAMERA_TAG" \
        https://git.libcamera.org/libcamera/libcamera.git "$LIBCAMERA_SRC"
else
    echo "already cloned at $LIBCAMERA_SRC"
fi

echo "== [3/5] libcamera build+install (minimal: no pipelines/IPAs/gstreamer/apps) =="
if [ ! -f "$PKGCONFIG_DIR/libcamera.pc" ]; then
    meson setup "$LIBCAMERA_SRC/build-native" -C "$LIBCAMERA_SRC" \
        --prefix="$LIBCAMERA_PREFIX" \
        -Dpipelines=[] -Dipas=[] -Dgstreamer=disabled -Dcam=disabled \
        -Dqcam=disabled -Ddocumentation=disabled -Dlc-compliance=disabled \
        -Dtracing=disabled -Dandroid=disabled -Dpycamera=disabled -Dtest=false
    ninja -C "$LIBCAMERA_SRC/build-native"
    ninja -C "$LIBCAMERA_SRC/build-native" install
else
    echo "already installed at $LIBCAMERA_PREFIX"
fi

echo "== [4/5] libunwind.pc shim (gstreamer-1.0.pc lists it Requires.private," \
     "but Ubuntu's libunwind-18-dev ships no .pc) =="
mkdir -p "$SHIM_DIR"
if [ ! -f "$SHIM_DIR/libunwind.pc" ]; then
    cat > "$SHIM_DIR/libunwind.pc" <<'EOF'
prefix=/usr
libdir=${prefix}/lib/x86_64-linux-gnu
includedir=${prefix}/include/libunwind

Name: libunwind
Description: libunwind (shim .pc, Ubuntu's libunwind-18-dev ships no .pc)
Version: 18.1.8
Libs: -L${libdir} -lunwind
Cflags: -I${includedir}
EOF
fi

echo "== [5/5] configure + build build-native/ =="
export PKG_CONFIG_PATH="$PKGCONFIG_DIR:$SHIM_DIR:${PKG_CONFIG_PATH:-}"
cmake -S "$REPO_DIR" -B "$REPO_DIR/build-native" -DENABLE_WAYLAND_PREVIEW=OFF
cmake --build "$REPO_DIR/build-native" -j"$(nproc)"

cat <<EOF

Done. Binary at $REPO_DIR/build-native/libcamera-dmabuf-capture

To run it, this exact LD_LIBRARY_PATH is needed (libcamera isn't in a
system-wide location):
  export LD_LIBRARY_PATH="$LIBCAMERA_PREFIX/lib/x86_64-linux-gnu:\$LD_LIBRARY_PATH"
  cd "$REPO_DIR/build-native"
  cp ../build/bev_config.ini .   # or your own tuned config
  ./libcamera-dmabuf-capture --src <camA.h265> --src <camB.h265> --blend pyramid
  # press 's' for a snapshot -> /tmp/snapshot_NNN.png (file-mode only accepts
  # raw .h265 elementary streams -- see file_source.cpp's filesrc!h265parse
  # pipeline, no container demuxer)
EOF
