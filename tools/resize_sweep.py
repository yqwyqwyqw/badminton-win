"""Sweep resize formulations to find the one that reproduces cv2 5.0 INTER_LINEAR.

Candidate space: single-pass vs two-pass, float vs fixed-point coefficient depth,
weight convention (w0 from (1-frac) vs w1 from frac) and rounding mode.

Dev-only tool.
"""

from __future__ import annotations

import argparse
import itertools
from pathlib import Path

import cv2
import numpy as np


def axes(src: int, dst: int) -> tuple[np.ndarray, np.ndarray]:
    scale = src / dst
    d = np.arange(dst, dtype=np.float64)
    fx = (d + 0.5) * scale - 0.5
    sx_raw = np.floor(fx)
    frac = fx - sx_raw
    sx = sx_raw.copy()
    frac = frac.copy()
    left = sx_raw < 0
    sx[left] = 0
    frac[left] = 0.0
    right = sx_raw + 1 >= src
    sx[right] = src - 2
    frac[right] = 0.0
    return sx.astype(np.int64), frac


def weights(frac: np.ndarray, bits: int, convention: str) -> tuple[np.ndarray, np.ndarray]:
    scale = 1 << bits
    if convention == "w0":
        w0 = np.rint((1.0 - frac) * scale).astype(np.int64)
        w1 = scale - w0
    else:
        w1 = np.rint(frac * scale).astype(np.int64)
        w0 = scale - w1
    return w0, w1


def round_mode(values: np.ndarray, mode: str) -> np.ndarray:
    if mode == "rint":
        return np.rint(values)
    if mode == "half_up":
        return np.floor(values + 0.5)
    if mode == "trunc":
        return np.trunc(values)
    raise ValueError(mode)


def resize(src: np.ndarray, dst_w: int, dst_h: int, *, passes: int, bits: int, convention: str,
           rounding: int, mode: str) -> np.ndarray:
    h, w, c = src.shape
    sx, fx = axes(w, dst_w)
    sy, fy = axes(h, dst_h)
    if passes == 1:
        if bits == 0:  # float single pass
            a = src[np.ix_(np.arange(h), sx)].astype(np.float64)
            b = src[np.ix_(np.arange(h), np.minimum(sx + 1, w - 1))].astype(np.float64)
            horiz = a * (1 - fx)[None, :, None] + b * fx[None, :, None]
            a2 = horiz[sy]
            b2 = horiz[np.minimum(sy + 1, h - 1)]
            out = a2 * (1 - fy)[:, None, None] + b2 * fy[:, None, None]
            return np.clip(round_mode(out, mode), 0, 255).astype(np.uint8)
        w0x, w1x = weights(fx, bits, convention)
        w0y, w1y = weights(fy, bits, convention)
        scale = 1 << bits
        a = src[np.ix_(np.arange(h), sx)].astype(np.int64)
        b = src[np.ix_(np.arange(h), np.minimum(sx + 1, w - 1))].astype(np.int64)
        # 2D weights combined in one integer expression, like a fused kernel.
        horiz = a * w0x[None, :, None] + b * w1x[None, :, None]
        a2 = horiz[sy]
        b2 = horiz[np.minimum(sy + 1, h - 1)]
        out = (a2 * w0y[:, None, None] + b2 * w1y[:, None, None] + rounding) >> (2 * bits)
        return np.clip(out, 0, 255).astype(np.uint8)

    # two passes, 8-bit intermediate
    if bits == 0:
        a = src[np.ix_(np.arange(h), sx)].astype(np.float64)
        b = src[np.ix_(np.arange(h), np.minimum(sx + 1, w - 1))].astype(np.float64)
        mid = round_mode(a * (1 - fx)[None, :, None] + b * fx[None, :, None], mode)
        a2 = mid[sy]
        b2 = mid[np.minimum(sy + 1, h - 1)]
        out = round_mode(a2 * (1 - fy)[:, None, None] + b2 * fy[:, None, None], mode)
        return np.clip(out, 0, 255).astype(np.uint8)
    w0x, w1x = weights(fx, bits, convention)
    w0y, w1y = weights(fy, bits, convention)
    a = src[np.ix_(np.arange(h), sx)].astype(np.int64)
    b = src[np.ix_(np.arange(h), np.minimum(sx + 1, w - 1))].astype(np.int64)
    mid = (a * w0x[None, :, None] + b * w1x[None, :, None] + rounding) >> bits
    mid = np.clip(mid, 0, 255)
    a2 = mid[sy]
    b2 = mid[np.minimum(sy + 1, h - 1)]
    out = (a2 * w0y[:, None, None] + b2 * w1y[:, None, None] + rounding) >> bits
    return np.clip(out, 0, 255).astype(np.uint8)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe-dir", required=True, type=Path)
    parser.add_argument("--cases", default="real_720p,gradient,tiny_down,odd_down,tiny_up")
    args = parser.parse_args()
    wanted = args.cases.split(",")

    loaded = []
    for line in (args.probe_dir / "cases.txt").read_text(encoding="ascii").splitlines():
        parts = line.split()
        if parts[0] not in wanted:
            continue
        _, sw, sh, sc, dw, dh, dc = parts[:7]
        sw, sh, dw, dh = int(sw), int(sh), int(dw), int(dh)
        source = np.frombuffer((args.probe_dir / f"{parts[0]}.src.u8.bin").read_bytes(),
                               dtype=np.uint8).reshape(sh, sw, int(sc))
        reference = np.frombuffer((args.probe_dir / f"{parts[0]}.dst.u8.bin").read_bytes(),
                                  dtype=np.uint8).reshape(dh, dw, int(dc))
        loaded.append((parts[0], source, reference, dw, dh))

    results = []
    combos = []
    for passes in (1, 2):
        for bits in (0, 8, 10, 11, 12, 14, 15, 16):
            for convention in ("w0", "w1"):
                for rounding in ({0, 1 << (bits - 1)} if bits else {0}):
                    for mode in (("rint", "half_up", "trunc") if bits == 0 else ("n/a",)):
                        combos.append((passes, bits, convention, rounding, mode))
    for combo in combos:
        passes, bits, convention, rounding, mode = combo
        total = 0
        mismatch = 0
        worst = 0
        for name, source, reference, dw, dh in loaded:
            try:
                out = resize(source, dw, dh, passes=passes, bits=bits, convention=convention,
                             rounding=rounding, mode=mode)
            except Exception:
                mismatch += reference.size
                total += reference.size
                continue
            diff = np.count_nonzero(out != reference)
            mismatch += int(diff)
            worst = max(worst, 100.0 * diff / reference.size)
            total += reference.size
        results.append((mismatch, worst, combo))
    results.sort(key=lambda item: item[0])

    print(f"cases={[name for name, *_ in loaded]}")
    print(f"{'passes':>6} {'bits':>5} {'conv':>5} {'round':>6} {'mode':>8} "
          f"{'mismatch':>10} {'worst%':>8}")
    for mismatch, worst, (passes, bits, convention, rounding, mode) in results[:14]:
        print(f"{passes:>6} {bits:>5} {convention:>5} {rounding:>6} {mode:>8} "
              f"{mismatch:>10} {worst:>8.4f}")


if __name__ == "__main__":
    main()
