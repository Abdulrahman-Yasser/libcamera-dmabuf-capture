#!/usr/bin/env python3
"""Compute a single camera's intrinsic calibration (camera matrix + distortion
coefficients) from a set of ChArUco board photos, using the calib.io
"charuco_250x400_8x5_50_37_DICT_4X4" board by default: the filename's "8x5"
is rows x cols (400mm/50mm=8 rows tall, 250mm/50mm=5 cols wide), so that's
squares_x=5, squares_y=8, 50mm squares, 37mm markers, DICT_4X4_50 -- verified
against real captures by checking which orientation actually yields nonzero
ChArUco corners (see --squares-x/--squares-y if your board differs).

Unlike calibrate_bev.py (one shared board shot -> camA/camB homography), this
needs MANY photos of the board from ONE camera, at different distances,
angles and positions in the frame (tilt it, move it to the corners, vary
distance) -- that's what lets calibrateCamera() separate lens distortion from
board pose. 15-25 varied shots is a reasonable target; a handful of near-
identical straight-on shots will converge to a poor/degenerate fit.

Usage:
    python3 calibrate_intrinsics.py <images_dir> [options]

<images_dir> must contain the calibration photos (png/jpg) from a single
camera -- nothing else needs to be shared with calibrate_bev.py's camA/camB
naming.

Examples:
    # Camera A's lens, ~20 shots dropped in one folder:
    python3 calibrate_intrinsics.py ~/projects/rpi_images/intrinsics_camA \\
        --out calibA.npz

    # Wide-angle Pi camera lens where the plain 5-coefficient model doesn't
    # fit well at the edges:
    python3 calibrate_intrinsics.py ~/projects/rpi_images/intrinsics_camB \\
        --out calibB.npz --rational-model
"""
import argparse
import sys
from pathlib import Path

import cv2
import numpy as np

IMAGE_EXTS = ("png", "jpg", "jpeg")


def make_board(squares_x, squares_y, square_len, marker_len, dict_name, legacy_pattern):
    dict_id = getattr(cv2.aruco, f"DICT_{dict_name}")
    d = cv2.aruco.getPredefinedDictionary(dict_id)
    b = cv2.aruco.CharucoBoard((squares_x, squares_y), square_len, marker_len, d)
    if legacy_pattern:
        b.setLegacyPattern(True)
    return b


def list_images(images_dir):
    """Globs images_dir for calibration photos, skipping this script's own
    "calib_*" output artifacts -- with --out-dir defaulting to images_dir
    itself, a rerun in the same directory would otherwise pick up the
    previous run's undistort-preview PNG as a bogus extra input photo."""
    paths = [p for ext in IMAGE_EXTS for p in images_dir.glob(f"*.{ext}")]
    paths += [p for ext in IMAGE_EXTS for p in images_dir.glob(f"*.{ext.upper()}")]
    return sorted(p for p in set(paths) if not p.name.startswith("calib_"))


def detect_charuco(detector, img_path):
    """Returns (img, charuco_corners, charuco_ids, num_corners). Corners/ids
    are forced into the Nx1x2 / Nx1 point-Mat shape drawDetectedCornersCharuco()
    requires -- detectBoard() hands back plain Nx2/N-shaped arrays in this
    OpenCV build, which cv2 happily accepts for matchImagePoints() but rejects
    there with a total()-mismatch assertion."""
    img = cv2.imread(str(img_path))
    if img is None:
        sys.exit(f"error: cannot read {img_path}")
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    cc, cids, _, _ = detector.detectBoard(gray)
    if cids is None:
        return img, cc, cids, 0
    cc = cc.reshape(-1, 1, 2).astype(np.float32)
    cids = cids.reshape(-1, 1).astype(np.int32)
    return img, cc, cids, len(cids)


def reprojection_errors(obj_points, img_points, rvecs, tvecs, K, dist):
    """Per-view RMS error (px) plus overall RMS, matching the formula OpenCV's
    own calibration.cpp sample uses: sqrt(sum of squared x/y residuals over a
    view, divided by that view's point count), then pooled over all views."""
    per_view = []
    total_sq_err, total_pts = 0.0, 0
    for i in range(len(obj_points)):
        proj, _ = cv2.projectPoints(obj_points[i], rvecs[i], tvecs[i], K, dist)
        diff = img_points[i].reshape(-1, 2) - proj.reshape(-1, 2)
        sq_err = float(np.sum(diff ** 2))
        n_pts = diff.shape[0]
        per_view.append((sq_err / n_pts) ** 0.5)
        total_sq_err += sq_err
        total_pts += n_pts
    overall = (total_sq_err / total_pts) ** 0.5
    return per_view, overall


def is_degenerate_view(objp):
    """True if a view's matched corners all sit on a single board row or
    column (e.g. only one partial column of the board visible). Such a view
    has no 2D spread, so its per-view homography in calibrateCamera's initial
    guess is rank-deficient -- OpenCV doesn't reject it, it crashes with an
    assert deep in initIntrinsicParams2D instead, so this has to be filtered
    before it ever reaches cv2.calibrateCamera."""
    pts = objp.reshape(-1, 3)[:, :2]
    xs = np.round(pts[:, 0], 6)
    ys = np.round(pts[:, 1], 6)
    return len(np.unique(xs)) < 2 or len(np.unique(ys)) < 2


def dist_labels(dist):
    """cv2.calibrateCamera's distCoeffs order is fixed regardless of which
    CALIB_*_MODEL flags were passed: with --rational-model this build always
    returns all 14 slots (padding the thin-prism/tilted-sensor terms this
    script never enables with exact zeros), not just k4-k6."""
    n = len(dist)
    known = ["k1", "k2", "p1", "p2", "k3", "k4", "k5", "k6", "s1", "s2", "s3", "s4", "taux", "tauy"]
    return known[:n] if n <= len(known) else [f"c{i}" for i in range(n)]


def save_npz(out_path, K, dist, image_w, image_h, rms, per_view_rms, used_paths, board_args):
    np.savez(
        out_path,
        K=K,
        dist=dist.flatten(),
        image_w=image_w,
        image_h=image_h,
        rms=rms,
        per_view_rms=np.array(per_view_rms),
        used_images=np.array([str(p) for p in used_paths]),
        squares_x=board_args.squares_x,
        squares_y=board_args.squares_y,
        square_len=board_args.square_len,
        marker_len=board_args.marker_len,
        dict=board_args.dict,
        legacy_pattern=not board_args.no_legacy_pattern,
    )


def save_yaml(out_path, K, dist, image_w, image_h, rms):
    """OpenCV FileStorage YAML using the same node names cv2's own
    calibration sample writes, so any cv::FileStorage-based C++ reader can
    load this directly without a project-specific schema."""
    fs = cv2.FileStorage(str(out_path), cv2.FILE_STORAGE_WRITE)
    fs.write("image_width", image_w)
    fs.write("image_height", image_h)
    fs.write("camera_matrix", K)
    fs.write("distortion_coefficients", dist)
    fs.write("avg_reprojection_error", rms)
    fs.release()


def save_lens_ini(out_path, K, dist, image_w, image_h):
    """Writes the flat key=value format libcamera-dmabuf-capture's
    lens_calib_load() (bev_config.cpp) reads, looked up at runtime as
    "<lens_dir>/<lens_model>-lens.ini" -- so this is the file to hand that
    naming convention directly, no manual transcription of fx/fy/../k1..
    needed. calib_w/calib_h record the resolution K/dist were fit at (this
    run's photos), so the C++ side can correctly rescale onto whatever
    resolution the actual video turns out to be -- same idea as
    ipm.cpp/main.cpp's CAL_W/CAL_H rescale for hb2i. dist's first 5
    coefficients are always k1,k2,p1,p2,k3 regardless of --rational-model
    (cv2's fixed ordering) -- higher-order rational terms (k4-k6) aren't
    part of this project's distortion model, so aren't written even if
    --rational-model produced them."""
    fx, fy = float(K[0, 0]), float(K[1, 1])
    cx, cy = float(K[0, 2]), float(K[1, 2])
    k1, k2, p1, p2, k3 = (float(v) for v in dist[:5])
    with open(out_path, "w") as f:
        f.write("# lens intrinsics/distortion -- written by calibrate_intrinsics.py\n")
        f.write("# looked up at runtime as <lens_dir>/<lens_model>-lens.ini\n\n")
        f.write(f"fx = {fx:.7g}\n")
        f.write(f"fy = {fy:.7g}\n")
        f.write(f"cx = {cx:.7g}\n")
        f.write(f"cy = {cy:.7g}\n")
        f.write(f"k1 = {k1:.7g}\n")
        f.write(f"k2 = {k2:.7g}\n")
        f.write(f"k3 = {k3:.7g}\n")
        f.write(f"p1 = {p1:.7g}\n")
        f.write(f"p2 = {p2:.7g}\n")
        f.write(f"calib_w = {image_w}\n")
        f.write(f"calib_h = {image_h}\n")


def render_undistort_preview(img, K, dist, out_dir, tag):
    h, w = img.shape[:2]
    newK, roi = cv2.getOptimalNewCameraMatrix(K, dist, (w, h), alpha=1.0)
    undistorted = cv2.undistort(img, K, dist, None, newK)
    side_by_side = np.hstack([img, undistorted])
    out_path = out_dir / f"calib_{tag}_undistort_preview.png"
    cv2.imwrite(str(out_path), side_by_side)
    return out_path


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("images_dir", type=Path, help="directory of calibration photos from one camera")
    ap.add_argument("--squares-x", type=int, default=5, help="columns")
    ap.add_argument("--squares-y", type=int, default=8, help="rows")
    ap.add_argument("--square-len", type=float, default=0.050, help="metres")
    ap.add_argument("--marker-len", type=float, default=0.037, help="metres")
    ap.add_argument("--dict", default="4X4_50", help="ArUco dictionary, e.g. 4X4_50, 5X5_100")
    ap.add_argument("--no-legacy-pattern", action="store_true", help="board printed with the post-4.6 ArUco pattern")
    ap.add_argument("--min-corners", type=int, default=6, help="minimum ChArUco corners for a photo to be used")
    ap.add_argument("--min-images", type=int, default=3, help="hard floor -- below this, calibration is refused")
    ap.add_argument("--warn-images", type=int, default=10,
                     help="print a warning if fewer usable photos than this (still runs)")
    ap.add_argument("--warn-px", type=float, default=1.0, help="flag any single view's reprojection error above this (px)")
    ap.add_argument("--rational-model", action="store_true",
                     help="8-coefficient distortion model (k1..k6,p1,p2) -- fits wide-angle lens edges better")
    ap.add_argument("--zero-tangent-dist", action="store_true", help="assume no lens/sensor tilt (p1=p2=0)")
    ap.add_argument("--fix-principal-point", action="store_true", help="pin principal point to image center")
    ap.add_argument("--out", type=Path, default=None, help="output .npz (default: <images_dir>/intrinsics.npz)")
    ap.add_argument("--yaml", type=Path, default=None, help="also write an OpenCV FileStorage YAML here")
    ap.add_argument("--lens-out", type=Path, default=None,
                     help="also write libcamera-dmabuf-capture's lens-ini format here "
                          "(e.g. imx219-lens.ini) -- see save_lens_ini()")
    ap.add_argument("--out-dir", type=Path, default=None, help="verification images dir (default: images_dir itself)")
    ap.add_argument("--tag", default=None, help="verification image filename tag (default: images_dir's name)")
    ap.add_argument("--no-detections", action="store_true", help="skip writing per-photo corner-overlay debug images")
    args = ap.parse_args()

    board = make_board(args.squares_x, args.squares_y, args.square_len, args.marker_len,
                        args.dict, not args.no_legacy_pattern)
    detector = cv2.aruco.CharucoDetector(board)

    image_paths = list_images(args.images_dir)
    if not image_paths:
        sys.exit(f"error: no {'/'.join(IMAGE_EXTS)} images found in {args.images_dir}")

    out_dir = args.out_dir or args.images_dir
    tag = args.tag or args.images_dir.name
    detections_dir = out_dir / "detections"
    if not args.no_detections:
        detections_dir.mkdir(parents=True, exist_ok=True)

    obj_points, img_points, used_paths, used_ncorners = [], [], [], []
    image_size = None
    skipped = []
    skipped_degenerate = []
    for path in image_paths:
        img, cc, cids, n = detect_charuco(detector, path)
        if image_size is None:
            image_size = (img.shape[1], img.shape[0])
        elif (img.shape[1], img.shape[0]) != image_size:
            sys.exit(f"error: {path.name} is {img.shape[1]}x{img.shape[0]}, expected "
                      f"{image_size[0]}x{image_size[1]} -- all photos must come from the "
                      f"same camera at the same resolution (put other cameras in a "
                      f"separate directory)")

        if n < args.min_corners:
            skipped.append((path.name, n))
            continue

        objp, imgp = board.matchImagePoints(cc, cids)
        if is_degenerate_view(objp):
            skipped_degenerate.append((path.name, n))
            continue
        obj_points.append(objp)
        img_points.append(imgp)
        used_paths.append(path)
        used_ncorners.append(n)

        if not args.no_detections:
            annotated = img.copy()
            cv2.aruco.drawDetectedCornersCharuco(annotated, cc, cids)
            cv2.imwrite(str(detections_dir / f"{path.stem}_corners.png"), annotated)

    print(f"{len(used_paths)}/{len(image_paths)} photos usable (>= {args.min_corners} corners)")
    if skipped:
        print("skipped (too few corners -- board partly out of frame, blurry, or "
              "poorly lit):")
        for name, n in skipped:
            print(f"  {name}: {n} corners")
    if skipped_degenerate:
        print("skipped (corners all fall on a single board row/column -- enough "
              "corners, but no 2D spread to constrain calibration):")
        for name, n in skipped_degenerate:
            print(f"  {name}: {n} corners")

    if len(used_paths) < args.min_images:
        sys.exit(f"error: only {len(used_paths)} usable photos, need >= {args.min_images}. "
                  f"Take more shots of the board at varied distances/angles/positions.")
    if len(used_paths) < args.warn_images:
        print(f"warning: only {len(used_paths)} usable photos (recommend >= {args.warn_images} "
              f"varied poses) -- result may be unreliable, especially the distortion terms.")

    flags = 0
    if args.rational_model:
        flags |= cv2.CALIB_RATIONAL_MODEL
    if args.zero_tangent_dist:
        flags |= cv2.CALIB_ZERO_TANGENT_DIST
    if args.fix_principal_point:
        flags |= cv2.CALIB_FIX_PRINCIPAL_POINT

    rms, K, dist, rvecs, tvecs = cv2.calibrateCamera(obj_points, img_points, image_size, None, None, flags=flags)
    per_view_rms, overall_rms = reprojection_errors(obj_points, img_points, rvecs, tvecs, K, dist)
    dist = dist.flatten()

    print()
    print(f"reprojection error: overall={overall_rms:.3f}px (cv2-reported rms={rms:.3f}px)")
    flagged = [(p.name, e) for p, e in zip(used_paths, per_view_rms) if e > args.warn_px]
    if flagged:
        print(f"warning: {len(flagged)} view(s) above --warn-px={args.warn_px}px -- consider "
              f"dropping these and rerunning (board moved during shot / motion blur / bad corner match):")
        for name, e in sorted(flagged, key=lambda x: -x[1]):
            print(f"  {name}: {e:.3f}px")

    print()
    print(f"image size: {image_size[0]}x{image_size[1]}")
    print("camera_matrix (K):")
    print(np.array2string(K, precision=3, suppress_small=True))
    print("distortion coefficients:")
    for label, v in zip(dist_labels(dist), dist):
        print(f"  {label} = {v:.6g}")

    out_path = args.out or (args.images_dir / "intrinsics.npz")
    save_npz(out_path, K, dist, image_size[0], image_size[1], overall_rms, per_view_rms, used_paths, args)
    print()
    print(f"wrote {out_path}  (keys: K, dist, image_w, image_h, rms, per_view_rms, used_images, "
          f"squares_x, squares_y, square_len, marker_len, dict, legacy_pattern)")

    if args.yaml:
        save_yaml(args.yaml, K, dist, image_size[0], image_size[1], overall_rms)
        print(f"wrote {args.yaml}")

    if args.lens_out:
        save_lens_ini(args.lens_out, K, dist, image_size[0], image_size[1])
        print(f"wrote {args.lens_out}  (keys: fx, fy, cx, cy, k1, k2, k3, p1, p2, "
              f"calib_w, calib_h -- drop straight into --lens-dir)")

    best_path = used_paths[int(np.argmax(used_ncorners))]
    best_img = cv2.imread(str(best_path))
    preview_path = render_undistort_preview(best_img, K, dist, out_dir, tag)
    print(f"wrote {preview_path}  (left=original, right=undistorted -- straight lines in the "
          f"scene should actually look straight on the right; curved-inward/outward edges mean "
          f"a bad fit)")


if __name__ == "__main__":
    main()
