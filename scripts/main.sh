#!/usr/bin/env bash
# Capture one still from each fixed camera into cam0/ and cam1/ (matched index).
# Run once per board pose: 20-30 times for intrinsics; once (board left on the
# ground) for the overlap pair. Never overwrites; auto-increments 000,001,...
set -euo pipefail

# ---- settings (check these once) ----
W=1640; H=1232          # capture resolution -- MUST match your pipeline
SETTLE=1000             # ms for auto-exposure/white-balance to settle
EXT=png                 # lossless -> sharper corners
BOARD_COLS=9; BOARD_ROWS=6   # inner corners, for the usability check
# -------------------------------------

CAM=$(command -v rpicam-still || command -v libcamera-still) \
  || { echo "rpicam-still / libcamera-still not found"; exit 1; }

mkdir -p cam0 cam1

# next free shared index
N=0
while [ -e "cam0/$(printf '%03d' "$N").$EXT" ]; do N=$((N+1)); done
IDX=$(printf '%03d' "$N")

for i in 0 1; do
  out="cam$i/$IDX.$EXT"
  "$CAM" --camera "$i" -n -t "$SETTLE" --width "$W" --height "$H" -e "$EXT" -o "$out"
  echo "saved $out"
done

# quick check: was the board detectable in both? (won't abort capture)
python3 - "$IDX" "$EXT" "$BOARD_COLS" "$BOARD_ROWS" <<'PY' || true
import sys, cv2
idx, ext, c, r = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
flags = cv2.CALIB_CB_ADAPTIVE_THRESH | cv2.CALIB_CB_NORMALIZE_IMAGE
for d in ("cam0", "cam1"):
    g = cv2.cvtColor(cv2.imread(f"{d}/{idx}.{ext}"), cv2.COLOR_BGR2GRAY)
    ok, _ = cv2.findChessboardCorners(g, (c, r), flags)
    print(f"  {d}: {'board ok' if ok else 'MISS -> reshoot this pose'}")
PY
