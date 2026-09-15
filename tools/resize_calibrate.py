"""Find which formulation reproduces cv2.resize(INTER_LINEAR) on this build.

Iterating in numpy is far faster than rebuilding C++.  Candidates include the
classic two-pass 11-bit fixed point path, a single-pass 2D float bilinear, the
area filter, and OpenCV's INTER_LINEAR_EXACT, all compared against cv2's
INTER_LINEAR output for the same input.

Dev-only tool.
"""

from __future__ import annotations

import argparse
from pathlib import Path

import cv2
import numpy as np

COEF_BITS = 11
COEF_SCALE = 1 << COEF_BITS


def axes(src: int, dst: int) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    scale = src / dst
    d = np.arange(dst, dtype=np.float64)
    fx = (d + 0.5) * scale - 0.5
    sx = np.floor(fx).astype(np.int64)
    frac = fx - sx
    sx = np.where(sx < 0, 0, sx)
    frac = np.where(sx < 0, 0.0, frac)
    too_far = sx + 1 >= src
    sx = np.where(too_far, src - 2, sx)
    frac = np.where(too_far, 0.0, frac)
    return sx, frac, scale


def two_pass_fixed(src: np.ndarray, dst_w: int, dst_h: int, rounding: int) -> np.ndarray:
    h, w, c = src.shape
    sx, fx, _ = axes(w, dst_w)
    sy, fy, _ = axes(h, dst_h)
    w0 = np.rint((1.0 - fx) * COEF_SCALE).astype(np.int64)
    w1 = COEF_SCALE - w0
    a = src[:, sx, :].astype(np.int64)
    b = src[:, np.minimum(sx + 1, w - 1), :].astype(np.int64)
    mid = (a * w0[None, :, None] + b * w1[None, :, None] + rounding) >> COEF_BITS
    mid = np.clip(mid, 0, 255).astype(np.uint8)
    v0 = np.rint((1.0 - fy) * COEF_SCALE).astype(np.int64)
    v1 = COEF_SCALE - v0
    a2 = mid[sy, :, :].astype(np.int64)
    b2 = mid[np.minimum(sy + 1, h - 1), :, :].astype(np.int64)
    out = (a2 * v0[:, None, None] + b2 * v1[:, None, None] + rounding) >> COEF_BITS
    return np.clip(out, 0, 255).astype(np.uint8)


def single_pass_float(src: np.ndarray, dst_w: int, dst_h: int, mode: str) -> np.ndarray:
    h, w, c = src.shape
    sx, fx, _ = axes(w, dst_w)
    sy, fy, _ = axes(h, dst_h)
    a = src[np.ix_(np.arange(h), sx)].astype(np.float64)
    b = src[np.ix_(np.arange(h), np.minimum(sx + 1, w - 1))].astype(np.float64)
    horiz = a * (1 - fx)[None, :, None] + b * fx[None, :, None]
    a2 = horiz[sy, :, :]
    b2 = horiz[np.minimum(sy + 1, h - 1), :, :]
    out = a2 * (1 - fy)[:, None, None] + b2 * fy[:, None, None]
    if mode == "trunc":
        return np.clip(out, 0, 255).astype(np.uint8)
    return np.clip(np.rint(out), 0, 255).astype(np.uint8)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe-dir", required=True, type=Path)
    parser.add_argument("--case", default="real_720p")
    args = parser.parse_args()

    header = None
    for line in (args.probe_dir / "cases.txt").read_text(encoding="ascii").splitlines():
        parts = line.split()
        if parts[0] == args.case:
            header = parts
    if header is None:
        raise SystemExit(f"case {args.case} not found")
    _, sw, sh, sc, dw, dh, dc = header[:7]
    sw, sh, sc, dw, dh, dc = int(sw), int(sh), int(sc), int(dw), int(dh), int(dc)
    src = np.frombuffer((args.probe_dir / f"{args.case}.src.u8.bin").read_bytes(),
                        dtype=np.uint8).reshape(sh, sw, sc)
    reference = np.frombuffer((args.probe_dir / f"{args.case}.dst.u8.bin").read_bytes(),
                              dtype=np.uint8).reshape(dh, dw, dc)
    total = reference.size

    def report(name: str, candidate: np.ndarray | None) -> None:
        if candidate is None:
            print(f"{name:<34} n/a")
            return
        diff = np.abs(candidate.astype(np.int32) - reference.astype(np.int32))
        print(f"{name:<34} mismatch={np.count_nonzero(diff):>9} / {total} "
              f"({100.0 * np.count_nonzero(diff) / total:7.4f}%) max={diff.max()}")

    print(f"case={args.case} {sw}x{sh}x{sc} -> {dw}x{dh}x{dc} cv2={cv2.__version__}")
    report("two_pass_fixed rc=1024", two_pass_fixed(src, dw, dh, 1 << (COEF_BITS - 1)))
    report("two_pass_fixed rc=0", two_pass_fixed(src, dw, dh, 0))
    report("single_pass_float round", single_pass_float(src, dw, dh, "round"))
    report("single_pass_float trunc", single_pass_float(src, dw, dh, "trunc"))
    report("cv2 INTER_AREA", cv2.resize(src, (dw, dh), interpolation=cv2.INTER_AREA))
    report("cv2 INTER_LINEAR_EXACT", cv2.resize(src, (dw, dh), interpolation=cv2.INTER_LINEAR_EXACT))
    report("cv2 INTER_NEAREST", cv2.resize(src, (dw, dh), interpolation=cv2.INTER_NEAREST))

    # Does INTER_LINEAR_EXACT equal INTER_LINEAR on this build?
    exact = cv2.resize(src, (dw, dh), interpolation=cv2.INTER_LINEAR_EXACT)
    report("exact vs linear (should be 0)", exact)


if __name__ == "__main__":
    main()
