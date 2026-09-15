"""Pipeline-level parity: run the whole chunked TrackNet chain on ONNX and compare
against the PyTorch golden trajectory and the rally segmentation (M1, part 2).

The product path is reproduced exactly by reusing the project's own runner
primitives (frame reading, background median, windowing, ensemble, heatmap decode,
chunk grid) and only swapping the model call for an ONNX Runtime session.  That
isolates the model as the single difference, so any trajectory change is
attributable to the export.

Also reports throughput per execution provider, which is the data point behind the
"DirectML is required for a usable CPU-only machine" decision.

Dev-only tool.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
import onnxruntime as ort
import pandas as pd
import torch


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--video", required=True, type=Path)
    parser.add_argument("--tracknet-file", required=True, type=Path,
                        help="PyTorch checkpoint; only used to rebuild the pipeline config")
    parser.add_argument("--onnx", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--project-root", type=Path, default=None)
    parser.add_argument("--golden-dir", type=Path, default=None,
                        help="directory with background-median.npy and app-run/trajectory-partial.csv")
    parser.add_argument("--golden-trajectory", type=Path, default=None,
                        help="reference Frame,Visibility,X,Y CSV (defaults to <golden>/app-run)")
    parser.add_argument("--max-frames", type=int, default=0, help="0 = whole video")
    parser.add_argument("--chunk-seconds", type=float, default=6.0)
    parser.add_argument("--overlap-seconds", type=float, default=0.5)
    parser.add_argument("--batch-size", type=int, default=8)
    parser.add_argument("--eval-mode", choices=["nonoverlap", "average", "weight"],
                        default="nonoverlap")
    parser.add_argument("--provider", choices=["cpu", "dml"], default="dml")
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--label", default=None)
    return parser.parse_args()


def ensure_vendor_on_path(project_root: Path) -> None:
    vendor = (project_root / ".tools" / "vendor" / "BadmintonTrackNet").resolve()
    if str(vendor) not in sys.path:
        sys.path.insert(0, str(vendor))


def load_runner(path: Path):
    spec = importlib.util.spec_from_file_location("chunked_runner", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import runner: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def make_session(path: Path, provider: str, threads: int) -> ort.InferenceSession:
    options = ort.SessionOptions()
    options.intra_op_num_threads = threads
    options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
    name = "DmlExecutionProvider" if provider == "dml" else "CPUExecutionProvider"
    available = ort.get_available_providers()
    if name not in available:
        raise RuntimeError(f"{name} unavailable; have {available}")
    return ort.InferenceSession(str(path), options, providers=[name])


class OnnxTrackNet(torch.nn.Module):
    """Drop-in replacement for the torch module inside InferencePipeline."""

    def __init__(self, session: ort.InferenceSession, input_name: str, output_name: str):
        super().__init__()
        self.session = session
        self.input_name = input_name
        self.output_name = output_name

    @torch.inference_mode()
    def forward(self, x):
        array = x.detach().to(torch.float32).cpu().numpy()
        result = self.session.run([self.output_name], {self.input_name: array})[0]
        return torch.from_numpy(np.asarray(result, dtype=np.float32))


def compare_trajectories(reference: pd.DataFrame, candidate: pd.DataFrame) -> dict:
    merged = reference.merge(candidate, on="Frame", suffixes=("_ref", "_onnx"), how="outer")
    both_visible = merged[(merged.Visibility_ref == 1) & (merged.Visibility_onnx == 1)].copy()
    distance = np.hypot(both_visible.X_ref - both_visible.X_onnx,
                        both_visible.Y_ref - both_visible.Y_onnx)
    disagreements = merged[merged.Visibility_ref != merged.Visibility_onnx]
    exact = int((distance == 0).sum()) if len(distance) else 0
    return {
        "referenceFrames": int(len(reference)),
        "candidateFrames": int(len(candidate)),
        "framesOnlyInReference": int(merged.Visibility_ref.notna().sum() - len(merged.dropna(subset=["Visibility_onnx"]))),
        "jointVisibleFrames": int(len(both_visible)),
        "visibilityMismatchFrames": int(len(disagreements)),
        "visibilityMismatchFraction": float(len(disagreements) / max(len(merged), 1)),
        "mismatchFrameSample": disagreements.Frame.head(20).astype(int).tolist(),
        "coordIdenticalFrames": exact,
        "coordIdenticalFraction": float(exact / len(distance)) if len(distance) else None,
        "coordP50Px": float(np.percentile(distance, 50)) if len(distance) else None,
        "coordP95Px": float(np.percentile(distance, 95)) if len(distance) else None,
        "coordP999Px": float(np.percentile(distance, 99.9)) if len(distance) else None,
        "coordMaxPx": float(distance.max()) if len(distance) else None,
    }


def run_analyzer(python: Path, analyzer: Path, video: Path, trajectory: Path,
                 workdir: Path) -> pd.DataFrame:
    workdir.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [str(python), str(analyzer), "--video", str(video),
         "--trajectory", str(trajectory), "--output-dir", str(workdir)],
        check=True, stdout=subprocess.DEVNULL,
    )
    return pd.read_csv(workdir / "rallies-provisional.csv")


def main() -> None:
    args = parse_args()
    project_root = (args.project_root or Path(__file__).resolve().parent.parent).resolve()
    ensure_vendor_on_path(project_root)
    runner = load_runner(project_root / "validation" / "run_chunked_tracknet.py")

    args.video = args.video.resolve()
    args.onnx = args.onnx.resolve()
    args.output_dir = args.output_dir.resolve()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    label = args.label or f"{args.onnx.stem}-{args.provider}"

    metadata = runner.inspect_video(args.video)
    total_frames = int(metadata["frames"])
    target_frames = total_frames if args.max_frames <= 0 else min(total_frames, args.max_frames)
    core_frames = max(8, int(round(args.chunk_seconds * metadata["fps"])))
    overlap_frames = max(7, int(round(args.overlap_seconds * metadata["fps"])))
    chunk_count = int(np.ceil(target_frames / core_frames))

    golden_dir = args.golden_dir or (project_root / "validation-output" / "golden"
                                     / "VID20260828212318_1")
    median_path = golden_dir / "background-median.npy"
    if not median_path.exists():
        median_path = args.output_dir / "background-median.npy"
    median = runner.global_median(args.video, metadata, 41, median_path)

    pipeline_args = type("A", (), {"device": "cpu", "tracknet_file": str(args.tracknet_file),
                                   "inpaintnet_file": ""})()
    pipeline = runner._load_pipeline(pipeline_args)
    seq_len = int(pipeline.seq_len)

    session = make_session(args.onnx, args.provider, args.threads)
    pipeline.tracknet = OnnxTrackNet(session, session.get_inputs()[0].name,
                                     session.get_outputs()[0].name)
    print(f"[M1b] {label}: provider={args.provider} seq_len={seq_len} "
          f"frames={target_frames} chunks={chunk_count} batch={args.batch_size}", flush=True)

    rows: list[pd.DataFrame] = []
    started = time.monotonic()
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
            pipeline, frames, frame_ids,
            eval_mode=args.eval_mode, batch_size=args.batch_size,
            width=metadata["width"], height=metadata["height"],
            fps=metadata["fps"], median=median,
        )
        data = trajectory.to_dataframe()
        data = data[(data.Frame >= core_start) & (data.Frame < core_end)].copy()
        rows.append(data)
        del frames, trajectory, data
        elapsed = time.monotonic() - started
        print(f"[M1b] {label}: chunk {chunk_index + 1}/{chunk_count} "
              f"frames={core_end}/{target_frames} {elapsed:.1f}s "
              f"{core_end / max(elapsed, 1e-6):.2f} fps", flush=True)

    candidate = pd.concat(rows, ignore_index=True).sort_values("Frame").reset_index(drop=True)
    candidate_csv = args.output_dir / f"trajectory-{label}.csv"
    candidate.to_csv(candidate_csv, index=False)
    elapsed_total = time.monotonic() - started

    reference_path = args.golden_trajectory or (golden_dir / "app-run" / "trajectory-partial.csv")
    report: dict = {
        "label": label,
        "onnx": str(args.onnx),
        "provider": args.provider,
        "batchSize": args.batch_size,
        "evalMode": args.eval_mode,
        "analyzedFrames": int(target_frames),
        "elapsedSeconds": round(elapsed_total, 3),
        "framesPerSecond": round(target_frames / max(elapsed_total, 1e-6), 3),
        "realtimeFactor": round(target_frames / max(elapsed_total, 1e-6) / metadata["fps"], 4),
        "trajectoryCsv": str(candidate_csv),
        "thresholds": {"visibilityMismatchFraction": 0.005, "coordP95Px": 1.5},
    }

    if reference_path.exists():
        reference = pd.read_csv(reference_path)
        if len(reference) >= target_frames:
            reference = reference[reference.Frame < target_frames]
        comparison = compare_trajectories(reference, candidate)
        report["trajectoryComparison"] = comparison
        print(f"[M1b] trajectory: visible-mismatch={comparison['visibilityMismatchFrames']}"
              f"/{comparison['referenceFrames']} "
              f"({comparison['visibilityMismatchFraction']:.4%}) "
              f"identical={comparison['coordIdenticalFraction']:.4%} "
              f"p95={comparison['coordP95Px']} max={comparison['coordMaxPx']}", flush=True)

        analyzer = project_root / "validation" / "analyze_tracknet_rallies.py"
        python = Path(sys.executable)
        # Analyse exactly the same frame range on both sides, otherwise a bounded
        # run is compared against a full-clip segmentation.
        reference_csv = args.output_dir / f"trajectory-reference-{label}.csv"
        reference.to_csv(reference_csv, index=False)
        reference_rallies = run_analyzer(python, analyzer, args.video, reference_csv,
                                        args.output_dir / "analysis-reference")
        candidate_rallies = run_analyzer(python, analyzer, args.video, candidate_csv,
                                        args.output_dir / "analysis-candidate")
        reference_rallies.to_csv(args.output_dir / "rallies-reference.csv", index=False)
        candidate_rallies.to_csv(args.output_dir / "rallies-candidate.csv", index=False)
        rally_report = {
            "referenceRallyCount": int(len(reference_rallies)),
            "candidateRallyCount": int(len(candidate_rallies)),
            "referenceRallies": reference_rallies.to_dict("records"),
            "candidateRallies": candidate_rallies.to_dict("records"),
        }
        same = (len(reference_rallies) == len(candidate_rallies))
        if same and len(reference_rallies):
            max_start = float((reference_rallies.start_frame - candidate_rallies.start_frame)
                              .abs().max())
            max_end = float((reference_rallies.end_frame - candidate_rallies.end_frame).abs().max())
            rally_report["maxStartFrameDelta"] = max_start
            rally_report["maxEndFrameDelta"] = max_end
            same = max_start <= max(1, int(0.2 * metadata["fps"])) and \
                max_end <= max(1, int(0.2 * metadata["fps"]))
        rally_report["pass"] = bool(same)
        report["rallyComparison"] = rally_report
        print(f"[M1b] rallies: reference={len(reference_rallies)} candidate={len(candidate_rallies)}"
              f" -> {'PASS' if same else 'DIFF'}", flush=True)
    else:
        print(f"[M1b] no reference trajectory at {reference_path}", flush=True)

    (args.output_dir / f"report-{label}.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"[M1b] report -> {args.output_dir / f'report-{label}.json'}", flush=True)


if __name__ == "__main__":
    main()
