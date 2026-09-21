#!/usr/bin/env bash
# Run a deployed bev_view bundle on a board, from the bundle directory.
#
# `emb cross ... --deploy <user@host> --deploy-dir <dir>` puts the shell, the
# engine, the app image and libbev_view.so on the device; this starts it with
# the backend and geometry the board actually has. Copy it into the bundle
# (scp board/run-bev.sh <user@host>:<dir>/) and run it there:
#
#   ./run-bev.sh                       # test pattern, the default
#   BEV_MODE=camera ./run-bev.sh       # the live camera
#   BACKEND=drm-kms-vulkan ./run-bev.sh
#   DEVICE=/dev/dri/card0 W=1280 H=1440 ./run-bev.sh    # override the pick
#
# Defaults suit a headless Pi: no compositor is running, so the shell talks to
# KMS directly. The renderer is v3d; the shell opens the KMS device and the
# platform view opens the render node (BEV_RENDER_NODE overrides it).
set -u

B="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BACKEND="${BACKEND:-drm-kms-egl}"

# Which output to drive. Boards differ in ways that matter: a Pi 4 puts the
# 7-inch DSI panel on vc4's card (800x480), while a Pi 5 drives DSI from a
# separate rp1-dsi card (800x1280, portrait) with vc4 holding HDMI — so a
# hard-coded card number or geometry is wrong on one of them. Prefer a
# connected DSI panel, else the first connected output, and take the mode the
# connector reports rather than assuming one.
pick_output() {
    local fallback="" s dir conn card mode
    for s in /sys/class/drm/card*-*/status; do
        [ "$(cat "$s" 2>/dev/null)" = "connected" ] || continue
        dir="$(dirname "$s")"
        conn="$(basename "$dir")"          # e.g. card1-DSI-2
        card="${conn%%-*}"                 # e.g. card1
        mode="$(head -1 "$dir/modes" 2>/dev/null)"
        [ -n "$mode" ] || continue
        case "$conn" in
            *DSI*) echo "$card $mode"; return 0 ;;
        esac
        [ -n "$fallback" ] || fallback="$card $mode"
    done
    [ -n "$fallback" ] && echo "$fallback"
}

read -r _card _mode <<<"$(pick_output)"
DEVICE="${DEVICE:-${_card:+/dev/dri/$_card}}"
W="${W:-${_mode%%x*}}"
H="${H:-${_mode##*x}}"
: "${W:=800}" "${H:=480}"

# The bundle's own lib/ first. libdisplay-info.so.2 is not in PiOS trixie's
# base image while the shell links it, so a copy staged beside the bundle (or
# anywhere named here) is what keeps the shell loadable. PiOS bookworm and the
# Pi 5 image already carry it, so the staged copy is only a fallback.
export LD_LIBRARY_PATH="$B/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# What the view renders; see the example README for the full BEV_* set.
export BEV_MODE="${BEV_MODE:-pattern}"

echo "[run-bev] ${BACKEND} ${DEVICE:-(shell default)} ${W}x${H} mode=${BEV_MODE}"

cd "$B" || exit 1
exec ./homescreen -b "$B" --backend "$BACKEND" \
    ${DEVICE:+--drm-device "$DEVICE"} -w "$W" --height "$H" "$@"
