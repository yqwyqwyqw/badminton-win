"""Dump the golden tensors to raw float32 binaries for the C++ smoke test (M2 step 1).

C++ cannot read .npz (a zip of npy files) without extra dependencies, so the
golden windows and PyTorch heatmaps are flattened to plain little-endian float32
files plus a tiny dimension descriptor.  The values are identical to what the
Python parity run fed to ONNX Runtime (already divided by 255).

Dev-only tool.
"""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--golden-dir", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--limit", type=int, default=8,
                        help="windows to dump (8 = one batch of the product batch size)")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    inputs = np.load(args.golden_dir / "input-windows.npz")["inputs"]
    reference = np.load(args.golden_dir / "heatmaps-pytorch.npz")["heatmaps"]
    limit = min(args.limit, inputs.shape[0])
    inputs = inputs[:limit].astype(np.float32) / 255.0
    reference = reference[:limit].astype(np.float32)

    (args.out_dir / "inputs.f32.bin").write_bytes(np.ascontiguousarray(inputs).tobytes())
    (args.out_dir / "reference.f32.bin").write_bytes(np.ascontiguousarray(reference).tobytes())

    n, c_in, height, width = inputs.shape
    c_out = reference.shape[1]
    (args.out_dir / "dims.txt").write_text(
        f"{n} {c_in} {c_out} {height} {width}\n", encoding="ascii")

    print(f"windows={n} in={c_in}x{height}x{width} out={c_out}x{height}x{width}")
    print(f"inputs.f32.bin    {inputs.nbytes / 2**20:.1f} MiB")
    print(f"reference.f32.bin {reference.nbytes / 2**20:.1f} MiB")
    for name, array in (("input", inputs), ("reference", reference)):
        print(f"{name}: min={array.min():.6f} max={array.max():.6f} mean={array.mean():.6f}")


if __name__ == "__main__":
    main()
