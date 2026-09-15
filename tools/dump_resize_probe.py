"""Generate ground truth for the cv::resize(INTER_LINEAR) calibration probe.

The C++ kernel must reproduce the exact fixed-point semantics of the OpenCV build
that produced the golden set (cv2 5.0 here), otherwise the model input differs by
+-1 LSB and the decoded trajectory drifts.  This dumps small deterministic cases
plus the real 1280x720 -> 512x288 downscale so the C++ probe can try variants.

Dev-only tool.
"""

from __future__ import annotations

import argparse
from pathlib import Path

import cv2
import numpy as np


CASES = [
    # name, src_width, src_height, kind
    ("tiny_down", 7, 5, "random"),
    ("tiny_up", 4, 3, "random"),
    ("odd_down", 320, 180, "random"),
    ("gradient", 1280, 720, "gradient"),
    ("real_720p", 1280, 720, "random"),
]


def build(name: str, width: int, height: int, kind: str, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    if kind == "random":
        return rng.integers(0, 256, size=(height, width, 3), dtype=np.uint8)
    # Gradient plus noise: exercises fractional weights and clipping.
    xs = np.linspace(0, 255, width, dtype=np.float32)[None, :, None]
    ys = np.linspace(0, 255, height, dtype=np.float32)[:, None, None]
    base = (xs * 0.5 + ys * 0.5)
    noise = rng.integers(0, 16, size=(height, width, 3))
    return np.clip(base + noise, 0, 255).astype(np.uint8)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--target-width", type=int, default=512)
    parser.add_argument("--target-height", type=int, default=288)
    args = parser.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)

    lines: list[str] = []
    for seed, (name, width, height, kind) in enumerate(CASES):
        source = build(name, width, height, kind, seed)
        resized = cv2.resize(source, (args.target_width, args.target_height))
        (args.out_dir / f"{name}.src.u8.bin").write_bytes(source.tobytes())
        (args.out_dir / f"{name}.dst.u8.bin").write_bytes(resized.tobytes())
        lines.append(f"{name} {width} {height} 3 {args.target_width} {args.target_height} 3 "
                     f"{source.nbytes} {resized.nbytes}")
        print(f"{name}: {width}x{height} -> {args.target_width}x{args.target_height} "
              f"src={source.nbytes} dst={resized.nbytes}")

    (args.out_dir / "cases.txt").write_text("\n".join(lines) + "\n", encoding="ascii")
    print(f"cases={len(CASES)} -> {args.out_dir}")
    print(f"cv2 {cv2.__version__}")


if __name__ == "__main__":
    main()
