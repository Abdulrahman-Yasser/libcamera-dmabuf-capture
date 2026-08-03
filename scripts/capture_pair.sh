#!/bin/bash
# Simultaneous capture from both IMX219 on Pi 5.
# Pi 5 has NO hardware H.264 -> video uses MJPEG (software JPEG), not h264.
# Each run saves to its own timestamped folder, so re-running never overwrites.
# Order: still first (used by align_overlap.py), then a video clip as backup.
set -e

W=1640            # match your calibration resolution
H=1232
SETTLE=2000       # ms for AE/AWB to settle before the still
DUR=30000          # ms of video

SESSION="captures/$(date +%Y%m%d_%H%M%S)"
mkdir -p "$SESSION"

# 1) stills first — this is what align_overlap.py reads
rpicam-still --camera 0 --width $W --height $H -t $SETTLE --nopreview -o "$SESSION/camA.png" &
rpicam-still --camera 1 --width $W --height $H -t $SETTLE --nopreview -o "$SESSION/camB.png" &
wait

# 2) then a video clip, kept for grabbing other frames if a still is blurry
rpicam-vid --camera 0 --codec mjpeg --width $W --height $H -t $DUR --nopreview -o "$SESSION/camA.mjpeg" &
rpicam-vid --camera 1 --codec mjpeg --width $W --height $H -t $DUR --nopreview -o "$SESSION/camB.mjpeg" &
wait

# point the fixed names align_overlap.py reads at this newest still
ln -sf "$SESSION/camA.png" camA.png
ln -sf "$SESSION/camB.png" camB.png
echo "-> saved in $SESSION/  (stills camA.png/camB.png + video camA.mjpeg/camB.mjpeg)"
