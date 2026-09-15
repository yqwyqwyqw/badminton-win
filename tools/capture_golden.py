"""Capture a PyTorch golden reference set for the TrackNet inference chain (M0).

The ONNX refactor must be proven equivalent to the current PyTorch path, so this
script freezes the exact artifacts the refactor is measured against:

  * input-windows.npz     preprocessed model inputs (uint8, exact: value*255)
  * heatmaps-pytorch.npz  raw TrackNet outputs (float16) for those same windows
  * trajectory-pytorch.csv full trajectory for the analysed frame range
  * background-median.npy global background median (BGR uint8, as the runner caches it)
  * meta.json             hashes, model params, chunk params, versions

The preprocessing / windowing / ensemble / decode logic is imported from the
project's own validation runner so the golden set cannot drift from the product
path.  Only the model call is wrapped, and only to record inputs and outputs.

Dev-only tool.  Requires torch (`.venv-validation`); never shipped.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import platform
import sys
import time
from pathlib import Path
from types import SimpleNamespace

import cv2
import numpy as np
import pandas as pd
import torch


SCHEMA_VERSION = 1


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--video", required=True, type=Path)
    parser.add_argument("--tracknet-file", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--project-root", type=Path, default=None,
                        help="repository root (defaults to this file's parent's parent)")
    parser.add_argument("--runner", type=Path, default=None,
                        help="path to validation/run_chunked_tracknet.py")
    parser.add_argument("--max-frames", type=int, default=1200,
                        help="number of frames to analyse (bounded for turnaround time)")
    parser.add_argument("--chunk-seconds", type=float, default=6.0)
    parser.add_argument("--overlap-seconds", type=float, default=0.5)
    parser.add_argument("--batch-size", type=int, default=8)
    parser.add_argument("--eval-mode", choices=["nonoverlap", "average", "weight"],
                        default="nonoverlap")
    parser.add_argument("--median-samples", type=int, default=41)
    parser.add_argument("--device", default="cuda", choices=["cuda", "cpu"])
    parser.add_argument("--keep-windows", type=int, default=24,
                        help="how many model input windows to freeze")
    parser.add_argument("--window-stride", type=int, default=37,
                        help="take every Nth window; spread samples over the clip")
    parser.add_argument("--heatmap-dtype", choices=["float16", "float32"], default="float16",
                        help="float32 gives an exact parity floor; float16 halves the file size")
    return parser.parse_args()


def load_runner(path: Path):
    """Import the project's validation runner by path, so we reuse its logic."""
    spec = importlib.util.spec_from_file_location("chunked_runner", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import runner: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def ensure_vendor_on_path(project_root: Path) -> Path:
    """The runner imports `tracknet.*`; mirror the app's PYTHONPATH setup."""
    vendor = (project_root / ".tools" / "vendor" / "BadmintonTrackNet").resolve()
    if not vendor.is_dir():
        raise FileNotFoundError(f"vendored package not found: {vendor}")
    text = str(vendor)
    if text not in sys.path:
        sys.path.insert(0, text)
    return vendor


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


class Recorder(torch.nn.Module):
    """Wrap the TrackNet module and freeze a sample of inputs/outputs."""

    def __init__(self, inner: torch.nn.Module, keep: int, stride: int,
                 heatmap_dtype: str = "float16"):
        super().__init__()
        self.inner = inner
        self.keep = int(keep)
        self.stride = max(1, int(stride))
        self.heatmap_dtype = np.float32 if heatmap_dtype == "float32" else np.float16
        self.seen = 0
        self.inputs: list[np.ndarray] = []
        self.outputs: list[np.ndarray] = []
        self.window_index: list[int] = []

    @torch.inference_mode()
    def forward(self, x):  # x: (N, 27, 288, 512) float32 on device
        y = self.inner(x)
        if len(self.inputs) < self.keep and self.seen % self.stride == 0:
            # The preprocessed input is uint8/255, so *255 round-trips exactly.
            quantised = torch.round(x.detach().float() * 255.0).clamp_(0, 255).to(torch.uint8)
            self.inputs.append(quantised.cpu().numpy())
            self.outputs.append(y.detach().float().to(torch.float32).cpu().numpy()
                                .astype(self.heatmap_dtype))
            self.window_index.append(self.seen)
        self.seen += 1
        return y


def main() -> None:
    args = parse_args()
    project_root = (args.project_root or Path(__file__).resolve().parent.parent).resolve()
    runner_path = (args.runner or project_root / "validation" / "run_chunked_tracknet.py").resolve()
    ensure_vendor_on_path(project_root)
    runner = load_runner(runner_path)

    args.video = args.video.resolve()
    args.tracknet_file = args.tracknet_file.resolve()
    args.output_dir = args.output_dir.resolve()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    metadata = runner.inspect_video(args.video)
    total_frames = int(metadata["frames"])
    target_frames = min(total_frames, args.max_frames or total_frames)
    core_frames = max(8, int(round(args.chunk_seconds * metadata["fps"])))
    overlap_frames = max(7, int(round(args.overlap_seconds * metadata["fps"])))
    chunk_count = int(np.ceil(target_frames / core_frames))
    if args.eval_mode == "nonoverlap" and args.chunk_seconds > 0:
        seq_guess = 8  # confirmed from the checkpoint param_dict; re-read below
    print(f"[M0] video={args.video.name} {metadata['width']}x{metadata['height']} "
          f"fps={metadata['fps']:.3f} frames={total_frames} analysing={target_frames}", flush=True)

    median_path = args.output_dir / "background-median.npy"
    started = time.monotonic()
    median = runner.global_median(args.video, metadata, args.median_samples, median_path)
    print(f"[M0] background median ready: shape={median.shape} dtype={median.dtype} "
          f"({time.monotonic() - started:.1f}s)", flush=True)

    pipeline_args = SimpleNamespace(
        device=args.device,
        tracknet_file=str(args.tracknet_file),
        inpaintnet_file="",
    )
    pipeline = runner._load_pipeline(pipeline_args)
    seq_len = int(pipeline.seq_len)
    if args.eval_mode == "nonoverlap" and seq_guess != seq_len:
        print(f"[M0] NOTE: seq_len from checkpoint is {seq_len}, not {seq_guess}", flush=True)

    recorder = Recorder(pipeline.tracknet, args.keep_windows, args.window_stride,
                        args.heatmap_dtype)
    pipeline.tracknet = recorder

    rows: list[pd.DataFrame] = []
    infer_started = time.monotonic()
    for chunk_index in range(chunk_count):
        core_start = chunk_index * core_frames
        core_end = min(target_frames, core_start + core_frames)
        read_start = max(0, core_start - overlap_frames)
        read_end = min(total_frames, core_end + overlap_frames)
        if args.eval_mode == "nonoverlap":
            read_start -= read_start % seq_len
        frames = runner.read_frames(args.video, read_start, read_end)
        frame_ids = np.arange(read_start, read_end, dtype=np.int64)
        trajectory = runner.predict_frames_with_global_median(
            pipeline,
            frames,
            frame_ids,
            eval_mode=args.eval_mode,
            batch_size=args.batch_size,
            width=metadata["width"],
            height=metadata["height"],
            fps=metadata["fps"],
            median=median,
        )
        data = trajectory.to_dataframe()
        data = data[(data.Frame >= core_start) & (data.Frame < core_end)].copy()
        if len(data) != core_end - core_start:
            raise ValueError(
                f"chunk {chunk_index} produced {len(data)} frames, expected {core_end - core_start}"
            )
        rows.append(data)
        del frames, trajectory, data
        if args.device == "cuda":
            torch.cuda.empty_cache()
        done = core_end
        elapsed = time.monotonic() - infer_started
        print(f"[M0] chunk {chunk_index + 1}/{chunk_count} frames={done}/{target_frames} "
              f"elapsed={elapsed:.1f}s rate={done / max(elapsed, 1e-6):.2f} fps", flush=True)

    trajectory_csv = args.output_dir / "trajectory-pytorch.csv"
    trajectory = pd.concat(rows, ignore_index=True).sort_values("Frame")
    trajectory.to_csv(trajectory_csv, index=False)
    infer_seconds = time.monotonic() - infer_started

    inputs = np.concatenate(recorder.inputs, axis=0) if recorder.inputs else np.zeros((0,), np.uint8)
    outputs = np.concatenate(recorder.outputs, axis=0) if recorder.outputs else np.zeros((0,), np.float16)
    np.savez_compressed(args.output_dir / "input-windows.npz",
                        inputs=inputs, window_index=np.asarray(recorder.window_index, np.int64))
    np.savez_compressed(args.output_dir / "heatmaps-pytorch.npz", heatmaps=outputs)

    meta = {
        "schemaVersion": SCHEMA_VERSION,
        "createdAtUnix": time.time(),
        "video": {
            "path": str(args.video),
            "size": args.video.stat().st_size,
            "sha256": sha256(args.video),
            **metadata,
        },
        "model": {
            "path": str(args.tracknet_file),
            "size": args.tracknet_file.stat().st_size,
            "sha256": sha256(args.tracknet_file),
            "seqLen": seq_len,
            "bgMode": pipeline.bg_mode,
            "inputShape": [1, (seq_len + 1) * 3 if pipeline.bg_mode == "concat" else seq_len * 3, 288, 512],
            "outputShape": [1, seq_len, 288, 512],
        },
        "analysis": {
            "chunkSeconds": args.chunk_seconds,
            "overlapSeconds": args.overlap_seconds,
            "batchSize": args.batch_size,
            "evalMode": args.eval_mode,
            "analyzedFrames": int(target_frames),
            "coreFramesPerChunk": int(core_frames),
            "overlapFrames": int(overlap_frames),
            "chunkCount": int(chunk_count),
            "medianSamples": args.median_samples,
            "device": args.device,
            "inferenceSeconds": round(infer_seconds, 3),
            "inferenceFps": round(target_frames / max(infer_seconds, 1e-6), 3),
        },
        "samples": {
            "inputWindows": int(inputs.shape[0]),
            "inputDtype": "uint8 (exact value*255 of the float32 model input)",
            "heatmapDtype": "float32" if args.heatmap_dtype == "float32" else "float16",
            "windowIndex": recorder.window_index,
        },
        "environment": {
            "python": sys.version.split()[0],
            "torch": torch.__version__,
            "opencv": cv2.__version__,
            "numpy": np.__version__,
            "pandas": pd.__version__,
            "cudaAvailable": torch.cuda.is_available(),
            "gpu": torch.cuda.get_device_name(0) if torch.cuda.is_available() else None,
            "platform": platform.platform(),
        },
    }
    (args.output_dir / "meta.json").write_text(
        json.dumps(meta, ensure_ascii=False, indent=2), encoding="utf-8")

    visible = int(trajectory["Visibility"].sum())
    print(f"[M0] done: frames={len(trajectory)} visible={visible} "
          f"({100.0 * visible / max(len(trajectory), 1):.2f}%) "
          f"inference={infer_seconds:.1f}s ({target_frames / max(infer_seconds, 1e-6):.2f} fps)", flush=True)
    print(f"[M0] artifacts -> {args.output_dir}", flush=True)


if __name__ == "__main__":
    main()
