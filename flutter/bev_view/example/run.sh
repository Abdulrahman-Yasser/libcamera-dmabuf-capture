#!/usr/bin/env bash
# Build this example and run it on an ivi-homescreen shell.
#
# Assembles an ivi-layout bundle (data/flutter_assets + data/icudtl.dat) from
# `flutter build bundle`, then launches the shell with libihs_shared, the
# Flutter engine and the native-asset libbev_view.so on the library path.
#
# Every path is host layout, so every one is overridable:
#
#   IHS_DIR     ivi-homescreen checkout          (default: beside this repository)
#   IHS_BUILD   its build dir, holding shell/ + shared/  (default: first one found)
#   ENGINE_DIR  Flutter engine bundle (lib/ + data/icudtl.dat) (default: probed)
#   FLUTTER     the Flutter SDK's flutter binary (default: the one on PATH)
#   BACKEND     wayland-egl | wayland-vulkan | drm-kms-egl | drm-kms-vulkan | ...
#   W, H        window size
#
# ENGINE_DIR must be a DEBUG (JIT) engine whose Dart matches the SDK in
# FLUTTER: `flutter build bundle` emits a JIT kernel, and a newer or older
# engine rejects it at launch with
#   Dart Error: Can't load Kernel binary: Invalid kernel binary format version
# which reads like an app failure but is only a mismatched pair. Point FLUTTER
# and ENGINE_DIR at an SDK and an engine built from the same revision.
#
# The view renders the same way on every backend; only who allocates its
# buffers changes (see README.md). wayland-vulkan / drm-kms-vulkan additionally
# need IVI_VK_DMABUF=1, since the WSI-swapchain fallback does not composite
# platform-view images -- set here.
#
# BEV_* variables choose what the view renders and pass straight through to
# the app; see README.md.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"

IHS_DIR="${IHS_DIR:-$REPO/../ivi-homescreen}"
if [ -z "${IHS_BUILD:-}" ]; then
  for d in "$IHS_DIR"/build-pv "$IHS_DIR"/cmake-build-* "$IHS_DIR"/build*; do
    if [ -x "$d/shell/homescreen" ]; then IHS_BUILD="$(basename "$d")"; break; fi
  done
fi
IHS_BUILD="${IHS_BUILD:-build}"
SHELL_BIN="${SHELL_BIN:-$IHS_DIR/$IHS_BUILD/shell/homescreen}"
LIBDIR="${LIBDIR:-$IHS_DIR/$IHS_BUILD/shared}"
FLUTTER="${FLUTTER:-flutter}"
FLUTTER_SDK="$(cd "$(dirname "$(command -v "$FLUTTER" 2>/dev/null || echo /nonexistent)")/.." 2>/dev/null && pwd || true)"
SDK_ENGINE="$(cat "$FLUTTER_SDK/bin/cache/engine.stamp" 2>/dev/null || true)"

# The revision an engine bundle was built from, as it embeds it.
# The `|| true` is load-bearing: grep -m1 exits early, strings takes SIGPIPE,
# and under `set -o pipefail` that would abort the script.
engine_revision() {
  { strings -a "$1/lib/libflutter_engine.so" 2>/dev/null | grep -m1 -E '^[0-9a-f]{40}$'; } || true
}

# Prefer a debug engine built from this SDK's own engine revision; any debug
# bundle is better than none, but only the matching one will run the kernel.
if [ -z "${ENGINE_DIR:-}" ]; then
  FALLBACK=""
  for d in "${FLUTTER_WORKSPACE:-$HOME/.config/flutter_workspace}"/flutter-engine/bundle-debug-* \
           "$HOME"/workspace-automation/.config/flutter_workspace/flutter-engine/bundle-debug-* \
           "$REPO"/../*/.config/flutter_workspace/flutter-engine/bundle-debug-*; do
    [ -f "$d/data/icudtl.dat" ] && [ -f "$d/lib/libflutter_engine.so" ] || continue
    [ -n "$FALLBACK" ] || FALLBACK="$d"
    if [ -n "$SDK_ENGINE" ] && [ "$(engine_revision "$d")" = "$SDK_ENGINE" ]; then
      ENGINE_DIR="$d"
      break
    fi
  done
  ENGINE_DIR="${ENGINE_DIR:-$FALLBACK}"
fi
BACKEND="${BACKEND:-wayland-egl}"
W="${W:-1280}"
H="${H:-720}"

[ -e "$SHELL_BIN" ] || { echo "missing: $SHELL_BIN (set IHS_DIR / IHS_BUILD)" >&2; exit 1; }
if [ -z "${ENGINE_DIR:-}" ] || [ ! -e "$ENGINE_DIR/data/icudtl.dat" ]; then
  echo "no Flutter engine bundle found. Looked for bundle-debug-* under:" >&2
  echo "  ${FLUTTER_WORKSPACE:-$HOME/.config/flutter_workspace}/flutter-engine/" >&2
  echo "  $HOME/workspace-automation/.config/flutter_workspace/flutter-engine/" >&2
  echo "  $REPO/../*/.config/flutter_workspace/flutter-engine/" >&2
  echo "Set ENGINE_DIR to a debug (JIT) engine matching \$FLUTTER; see the header." >&2
  exit 1
fi

# A mismatched pair fails at launch inside Dart, which reads like an app bug.
ENGINE_REV="$(engine_revision "$ENGINE_DIR")"
if [ -n "$SDK_ENGINE" ] && [ -n "$ENGINE_REV" ] && [ "$SDK_ENGINE" != "$ENGINE_REV" ]; then
  echo "warning: $FLUTTER builds for engine ${SDK_ENGINE:0:12}, but" >&2
  echo "         $ENGINE_DIR is ${ENGINE_REV:0:12}." >&2
  echo "         Expect 'Invalid kernel binary format version'; point FLUTTER and" >&2
  echo "         ENGINE_DIR at an SDK and engine from the same revision." >&2
fi

cd "$HERE"

# The native-assets hook asks flutter_tools for a C compiler config, which it
# reads from a CMake cache `flutter build linux` would normally leave behind.
# There is no desktop runner here (the shell is the runner), so seed the few
# values it reads.
CC_CACHE="$HERE/build/flutter_assets/linux/x64/debug/CMakeCache.txt"
if [ ! -f "$CC_CACHE" ]; then
  echo ">> seeding compiler config for the native-assets hook"
  mkdir -p "$(dirname "$CC_CACHE")"
  cat > "$CC_CACHE" <<EOF
CMAKE_C_COMPILER:FILEPATH=$(command -v gcc)
CMAKE_CXX_COMPILER:FILEPATH=$(command -v g++)
CMAKE_LINKER:FILEPATH=$(command -v ld)
CMAKE_AR:FILEPATH=$(command -v ar)
CMAKE_BUILD_TYPE:STRING=Debug
EOF
fi

echo ">> flutter build bundle (native assets, linux-x64)"
"$FLUTTER" build bundle --target-platform linux-x64

echo ">> assembling ivi-layout bundle"
DEST="$HERE/.ivi-bundle"
rm -rf "$DEST"
mkdir -p "$DEST/data/flutter_assets"
cp -a "$HERE/build/flutter_assets/." "$DEST/data/flutter_assets/"
cp -a "$ENGINE_DIR/data/icudtl.dat" "$DEST/data/icudtl.dat"
NADIR="$DEST/data/flutter_assets/native_assets/linux"

VK_ENV=()
case "$BACKEND" in
  *vulkan*) VK_ENV=(IVI_VK_DMABUF=1) ;;
esac

echo ">> launching (backend=$BACKEND, ${W}x${H}, BEV_MODE=${BEV_MODE:-pattern})"
exec env LD_LIBRARY_PATH="$LIBDIR:$ENGINE_DIR/lib:$NADIR" "${VK_ENV[@]}" \
  "$SHELL_BIN" -b "$DEST" --backend "$BACKEND" -w "$W" --height "$H" "$@"
