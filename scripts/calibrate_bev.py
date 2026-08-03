#!/usr/bin/env python3
"""Compute matched camA/camB hb2i (board-metres -> image-pixels) homographies
for the BEV --src path from a single shared ChArUco board shot, and optionally
write them straight into an existing bev_config.ini.

This automates the exact by-hand process used to fix BUGS_FOUND.md bug #3:
independently-calibrating camA and camB against a board they didn't actually
share a position for produces two individually-plausible but mutually
mismatched homographies, which shows up as a hard, un-blended seam in the
rendered overlap. The fix is always the same shot discipline -- one board
placement, one photo from each camera, not moved in between -- this script
just does the corner detection / homography / rescale / sanity-check /
verification-render steps that used to be five one-off scripts.

Usage:
    python3 calibrate_bev.py <capture_dir> [options]

<capture_dir> must contain camA.png and camB.png (override names with
--cam-a/--cam-b) showing the same board shot, not moved between frames.

Examples:
    # Just compute and print the numbers, plus save verification images
    # next to the capture:
    python3 calibrate_bev.py ~/projects/street_captures/20260623_101549

    # Compute AND patch straight into a live config:
    python3 calibrate_bev.py ~/projects/street_captures/20260623_101549 \\
        --write ../build/bev_config.ini
"""
import argparse
import sys
from pathlib import Path

import cv2
import numpy as np

# Must match main.cpp's measured_H(): the resolution the calibration board was
# actually shot at, independent of any runtime video's own frame size.
CAL_W, CAL_H = 3280.0, 2464.0

# BEV canvas render used only for the verification images this script writes,
# not for the numbers it reports -- default matches this project's own
# bev_config.ini px_per_m.
DEFAULT_PPM = 2200.0


def make_board(squares_x, squares_y, square_len, marker_len, dict_name, legacy_pattern):
    dict_id = getattr(cv2.aruco, f"DICT_{dict_name}")
    d = cv2.aruco.getPredefinedDictionary(dict_id)
    b = cv2.aruco.CharucoBoard((squares_x, squares_y), square_len, marker_len, d)
    if legacy_pattern:
        b.setLegacyPattern(True)
    return b


def detect_corners(detector, img_path):
    img = cv2.imread(str(img_path))
    if img is None:
        sys.exit(f"error: cannot read {img_path}")
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    cc, cids, _, _ = detector.detectBoard(gray)
    n = 0 if cids is None else len(cids)
    if n == 0:
        return img, None, None, 0
    return img, cc.reshape(-1, 2), cids.flatten(), n


def board_to_image_homography(obj_xy, ids, corners):
    """board-metres -> image-pixels, RANSAC. Returns None if degenerate."""
    H, _ = cv2.findHomography(obj_xy[ids][:, :2], corners, cv2.RANSAC, 3.0)
    return H


def image_to_board_homography(obj_xy, ids, corners):
    """image-pixels -> board-metres -- the inverse direction, used only for
    the overlap-alignment-error sanity check below (not written anywhere)."""
    H, _ = cv2.findHomography(corners, obj_xy[ids][:, :2], cv2.RANSAC, 3.0)
    return H


def rescale_to_cal_space(H, img_w, img_h):
    """H was computed in this capture's own image_w x image_h pixel space;
    rescale its output side into CAL_W x CAL_H so it matches what
    main.cpp's measured_H() (and every existing hb2i in bev_config.cpp)
    assumes, regardless of what resolution this particular shot happens to
    be. A point at (x,y) in image_w x image_h is at (x*CAL_W/image_w,
    y*CAL_H/image_h) in CAL_W x CAL_H space."""
    scale = np.diag([CAL_W / img_w, CAL_H / img_h, 1.0])
    H_cal = scale @ H
    return H_cal / H_cal[2, 2]


def to_ini_string(H_cal):
    """bev_config.ini's hb2i is mat3 (ipm.h), column-major: m[col*3+row]."""
    return " ".join(f"{v:.7g}" for v in H_cal.T.flatten())


def to_cpp_string(H_cal):
    return ", ".join(f"{v:.7g}" for v in H_cal.T.flatten())


def overlap_alignment_error_mm(obj_xy, idsA, ccA, HA_i2b, idsB, ccB, HB_i2b):
    shared = sorted(set(idsA.tolist()) & set(idsB.tolist()))
    if not shared:
        return None, None
    mA = {i: p for i, p in zip(idsA, ccA)}
    mB = {i: p for i, p in zip(idsB, ccB)}

    def apply_H(H, pts):
        pts = np.asarray(pts, float).reshape(-1, 1, 2)
        return cv2.perspectiveTransform(pts, H).reshape(-1, 2)

    pA = apply_H(HA_i2b, [mA[i] for i in shared])
    pB = apply_H(HB_i2b, [mB[i] for i in shared])
    err_mm = np.linalg.norm(pA - pB, axis=1) * 1000.0
    return shared, err_mm


def measured_H(Hb2i_cal, ppm, bev_w, bev_h, board_half_w, board_half_h):
    """Mirrors main.cpp's measured_H(): BEV-canvas-pixel (top-origin) ->
    normalized image uv, via board-metres. Xc/Yc center the board in the
    canvas -- board_half_w/h, so this generalizes to any board size."""
    r = ppm
    S = np.array([
        [1.0 / r, 0.0, -(bev_w / 2.0) / r + board_half_w],
        [0.0, -1.0 / r, (bev_h / 2.0) / r + board_half_h],
        [0.0, 0.0, 1.0],
    ])
    H = Hb2i_cal @ S
    H = H.copy()
    H[0, :] /= CAL_W
    H[1, :] /= CAL_H
    return H


def render_verification(HA_cal, HB_cal, imgA, imgB, ppm, board_half_w, board_half_h, out_dir, tag):
    img_h, img_w = imgA.shape[:2]
    bw, bh = img_w, img_h  # canvas == this capture's own frame size, matching main.cpp

    def to_pixel_H(H_uv):
        return np.diag([img_w, img_h, 1.0]) @ H_uv

    HA_px = to_pixel_H(measured_H(HA_cal, ppm, bw, bh, board_half_w, board_half_h))
    HB_px = to_pixel_H(measured_H(HB_cal, ppm, bw, bh, board_half_w, board_half_h))

    warpA = cv2.warpPerspective(imgA, HA_px, (bw, bh), flags=cv2.WARP_INVERSE_MAP | cv2.INTER_LINEAR)
    warpB = cv2.warpPerspective(imgB, HB_px, (bw, bh), flags=cv2.WARP_INVERSE_MAP | cv2.INTER_LINEAR)

    grayA = cv2.cvtColor(warpA, cv2.COLOR_BGR2GRAY)
    grayB = cv2.cvtColor(warpB, cv2.COLOR_BGR2GRAY)
    maskA, maskB = grayA > 0, grayB > 0
    both = maskA & maskB

    blend = np.zeros_like(warpA)
    blend[maskA] = warpA[maskA]
    blend[maskB & ~maskA] = warpB[maskB & ~maskA]
    blend[both] = (warpA[both].astype(np.float32) * 0.5 + warpB[both].astype(np.float32) * 0.5).astype(np.uint8)

    # Auto-brighten purely for eyeballing: these shots range from very dim
    # (indoor) to full daylight, so a fixed multiplier either does nothing or
    # blows out highlights depending on which. Scale so the brightest 1% of
    # covered pixels lands near white, clamped to a sane range either way.
    sep = np.hstack([warpA, warpB])
    covered = sep[np.hstack([maskA, maskB])]
    p99 = np.percentile(covered, 99) if covered.size else 255.0
    boost = float(np.clip(235.0 / max(p99, 1.0), 1.0, 8.0))
    blend_boosted = np.clip(blend.astype(np.float32) * boost, 0, 255).astype(np.uint8)
    sep_boosted = np.clip(sep.astype(np.float32) * boost, 0, 255).astype(np.uint8)

    blend_path = out_dir / f"calib_{tag}_blend.png"
    sep_path = out_dir / f"calib_{tag}_sep.png"
    cv2.imwrite(str(blend_path), blend_boosted)
    cv2.imwrite(str(sep_path), sep_boosted)

    coverage = dict(
        camA=maskA.mean() * 100.0,
        camB=maskB.mean() * 100.0,
        overlap=both.mean() * 100.0,
    )
    return blend_path, sep_path, coverage


def patch_ini_hb2i(ini_path, slot_a, slot_b, hb2i_a_str, hb2i_b_str):
    lines = ini_path.read_text().splitlines(keepends=True)
    current_slot = None
    targets = {slot_a: hb2i_a_str, slot_b: hb2i_b_str}
    patched = set()
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("[slot") and stripped.endswith("]"):
            try:
                current_slot = int(stripped[5:-1])
            except ValueError:
                current_slot = None
            continue
        if current_slot in targets and stripped.startswith("hb2i"):
            lines[i] = f"hb2i = {targets[current_slot]}\n"
            patched.add(current_slot)
    missing = set(targets) - patched
    if missing:
        sys.exit(
            f"error: {ini_path} has no 'hb2i = ...' line under "
            f"{sorted('[slot%d]' % s for s in missing)} -- create those "
            f"sections first (see bev_config_save()'s format), then rerun "
            f"with --write."
        )
    ini_path.write_text("".join(lines))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture_dir", type=Path, help="directory containing camA.png/camB.png")
    ap.add_argument("--cam-a", default="camA.png", help="camera A image filename within capture_dir")
    ap.add_argument("--cam-b", default="camB.png", help="camera B image filename within capture_dir")
    ap.add_argument("--squares-x", type=int, default=6)
    ap.add_argument("--squares-y", type=int, default=8)
    ap.add_argument("--square-len", type=float, default=0.090, help="metres")
    ap.add_argument("--marker-len", type=float, default=0.070, help="metres")
    ap.add_argument("--dict", default="4X4_50", help="ArUco dictionary, e.g. 4X4_50, 5X5_100")
    ap.add_argument("--no-legacy-pattern", action="store_true", help="board printed with the post-4.6 ArUco pattern")
    ap.add_argument("--min-corners", type=int, default=4)
    ap.add_argument("--warn-mm", type=float, default=3.0, help="flag overlap alignment error above this (mm)")
    ap.add_argument("--ppm", type=float, default=DEFAULT_PPM, help="px_per_m for the verification render only")
    ap.add_argument("--out-dir", type=Path, default=None, help="default: capture_dir itself")
    ap.add_argument("--write", type=Path, default=None, help="existing bev_config.ini to patch in place")
    ap.add_argument("--slot-a", type=int, default=0)
    ap.add_argument("--slot-b", type=int, default=1)
    ap.add_argument("--tag", default=None, help="verification image filename tag (default: capture_dir's name)")
    args = ap.parse_args()

    board = make_board(args.squares_x, args.squares_y, args.square_len, args.marker_len,
                        args.dict, not args.no_legacy_pattern)
    detector = cv2.aruco.CharucoDetector(board)
    obj_xy = board.getChessboardCorners()
    board_half_w = args.squares_x * args.square_len / 2.0
    board_half_h = args.squares_y * args.square_len / 2.0

    path_a = args.capture_dir / args.cam_a
    path_b = args.capture_dir / args.cam_b
    imgA, ccA, idsA, nA = detect_corners(detector, path_a)
    imgB, ccB, idsB, nB = detect_corners(detector, path_b)
    print(f"camA ({path_a.name}): {nA} ChArUco corners")
    print(f"camB ({path_b.name}): {nB} ChArUco corners")
    if nA < args.min_corners or nB < args.min_corners:
        sys.exit(f"error: need >= {args.min_corners} corners per camera (board fully "
                  f"in frame, in focus, not moved between the two shots)")

    HA_b2i = board_to_image_homography(obj_xy, idsA, ccA)
    HB_b2i = board_to_image_homography(obj_xy, idsB, ccB)
    if HA_b2i is None or HB_b2i is None:
        sys.exit("error: findHomography failed (degenerate corner set) for camA or camB")

    HA_cal = rescale_to_cal_space(HA_b2i, imgA.shape[1], imgA.shape[0])
    HB_cal = rescale_to_cal_space(HB_b2i, imgB.shape[1], imgB.shape[0])

    HA_i2b = image_to_board_homography(obj_xy, idsA, ccA)
    HB_i2b = image_to_board_homography(obj_xy, idsB, ccB)
    shared, err_mm = overlap_alignment_error_mm(obj_xy, idsA, ccA, HA_i2b, idsB, ccB, HB_i2b)
    if shared is None:
        print("warning: camA/camB share no detected ChArUco corners -- can't "
              "measure overlap alignment error directly. Still writeable, but "
              "unverified; reposition so both cameras catch some of the same "
              "board corners if possible.")
    else:
        median, mean, mx = np.median(err_mm), err_mm.mean(), err_mm.max()
        print(f"shared corners: {len(shared)}  overlap alignment error: "
              f"median={median:.2f}mm mean={mean:.2f}mm max={mx:.2f}mm")
        if median > args.warn_mm:
            print(f"warning: median error > {args.warn_mm}mm -- board may have "
                  f"moved between shots, or corner detection is unreliable here "
                  f"(motion blur / partial occlusion / board not flat). Check the "
                  f"verification images below before trusting this.")

    hb2i_a_str = to_ini_string(HA_cal)
    hb2i_b_str = to_ini_string(HB_cal)
    print()
    print(f"[slot{args.slot_a}] hb2i = {hb2i_a_str}")
    print(f"[slot{args.slot_b}] hb2i = {hb2i_b_str}")
    print()
    print("bev_config.cpp mat3{{...}} form:")
    print(f"  slot{args.slot_a}: mat3{{{{ {to_cpp_string(HA_cal)} }}}}")
    print(f"  slot{args.slot_b}: mat3{{{{ {to_cpp_string(HB_cal)} }}}}")

    out_dir = args.out_dir or args.capture_dir
    tag = args.tag or args.capture_dir.name
    blend_path, sep_path, coverage = render_verification(
        HA_cal, HB_cal, imgA, imgB, args.ppm, board_half_w, board_half_h, out_dir, tag)
    print()
    print(f"camA coverage={coverage['camA']:.1f}%  camB coverage={coverage['camB']:.1f}%  "
          f"overlap={coverage['overlap']:.1f}%")
    print(f"wrote {blend_path}")
    print(f"wrote {sep_path}  (camA | camB side by side -- same board markers "
          f"should land in the same relative position in both halves)")

    if args.write:
        if not args.write.exists():
            sys.exit(f"error: --write target {args.write} does not exist -- "
                     f"create a bev_config.ini with [slot{args.slot_a}]/"
                     f"[slot{args.slot_b}] sections first (e.g. copy an "
                     f"existing one), this only patches the hb2i line.")
        patch_ini_hb2i(args.write, args.slot_a, args.slot_b, hb2i_a_str, hb2i_b_str)
        print()
        print(f"patched hb2i for [slot{args.slot_a}]/[slot{args.slot_b}] in {args.write}")


if __name__ == "__main__":
    main()
