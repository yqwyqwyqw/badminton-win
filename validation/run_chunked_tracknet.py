"""Resumable TrackNet validation with incremental rally clip emission.

The runner loads TrackNet once, processes a bounded group of frames at a time,
writes every completed core chunk immediately, and rebuilds a partial global
trajectory.  A closed rally is emitted as a standalone review clip while later
chunks continue in the background.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import signal
import subprocess
import sys
import time
from pathlib import Path
from types import SimpleNamespace

import cv2
import numpy as np
import pandas as pd
import torch

from tracknet.data.video import iter_windows
from tracknet.inference.tracknet import _load_pipeline


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--video", required=True, type=Path)
    parser.add_argument("--tracknet-file", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--chunk-seconds", type=float, default=6.0)
    parser.add_argument("--overlap-seconds", type=float, default=0.5)
    parser.add_argument("--batch-size", type=int, default=8)
    parser.add_argument("--eval-mode", choices=["nonoverlap", "average", "weight"], default="nonoverlap")
    parser.add_argument("--median-samples", type=int, default=41)
    parser.add_argument("--max-frames", type=int)
    parser.add_argument("--max-rallies", type=int)
    parser.add_argument("--ffmpeg", type=Path)
    parser.add_argument("--analyzer", type=Path)
    clip_group = parser.add_mutually_exclusive_group()
    clip_group.add_argument(
        "--write-rally-clips",
        dest="write_rally_clips",
        action="store_true",
        help="encode standalone rally MP4 files (default)",
    )
    clip_group.add_argument(
        "--no-rally-clips",
        dest="write_rally_clips",
        action="store_false",
        help="only write time indexes and do not encode standalone rally MP4 files",
    )
    parser.set_defaults(write_rally_clips=True)
    return parser.parse_args()


def replace_with_retry(temporary: Path, destination: Path) -> None:
    """Replace a UI-polled file, tolerating short-lived Windows read locks."""
    deadline = time.monotonic() + 5.0
    delay = 0.025
    while True:
        try:
            os.replace(temporary, destination)
            return
        except OSError as error:
            # On Windows the Qt UI can briefly hold the destination while its
            # refresh timer reads it. Access denied/sharing violation is
            # transient; other errors still need to fail immediately.
            if getattr(error, "winerror", None) not in (5, 32):
                raise
            if time.monotonic() >= deadline:
                raise
            time.sleep(delay)
            delay = min(delay * 1.6, 0.25)


def atomic_json(path: Path, value: dict | list) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    replace_with_retry(temporary, path)


def atomic_csv(path: Path, data: pd.DataFrame) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    data.to_csv(temporary, index=False)
    replace_with_retry(temporary, path)


def inspect_video(path: Path) -> dict:
    capture = cv2.VideoCapture(str(path))
    if not capture.isOpened():
        raise ValueError(f"cannot open video: {path}")
    result = {
        "width": int(capture.get(cv2.CAP_PROP_FRAME_WIDTH)),
        "height": int(capture.get(cv2.CAP_PROP_FRAME_HEIGHT)),
        "fps": float(capture.get(cv2.CAP_PROP_FPS)),
        "frames": int(capture.get(cv2.CAP_PROP_FRAME_COUNT)),
    }
    capture.release()
    if result["fps"] <= 0 or result["frames"] <= 0:
        raise ValueError(f"invalid video metadata: {path}")
    return result


def fingerprint(video: Path, model: Path, metadata: dict, args: argparse.Namespace) -> dict:
    video_stat = video.stat()
    model_stat = model.stat()
    return {
        "chunkSchemaVersion": 2,
        "video": str(video.resolve()),
        "videoSize": video_stat.st_size,
        "videoMtimeNs": video_stat.st_mtime_ns,
        "model": str(model.resolve()),
        "modelSize": model_stat.st_size,
        "width": metadata["width"],
        "height": metadata["height"],
        "fps": metadata["fps"],
        "frames": metadata["frames"],
        "chunkSeconds": args.chunk_seconds,
        "overlapSeconds": args.overlap_seconds,
        "evalMode": args.eval_mode,
    }


def global_median(video: Path, metadata: dict, samples: int, cache: Path) -> np.ndarray:
    if cache.exists():
        value = np.load(cache)
        expected = (metadata["height"], metadata["width"], 3)
        if value.shape != expected:
            raise ValueError(f"cached median has shape {value.shape}, expected {expected}")
        return value
    capture = cv2.VideoCapture(str(video))
    if not capture.isOpened():
        raise ValueError(f"cannot open video for median: {video}")
    indices = np.linspace(0, metadata["frames"] - 1, max(3, samples), dtype=int)
    frames = []
    try:
        for frame_id in indices:
            capture.set(cv2.CAP_PROP_POS_FRAMES, int(frame_id))
            ok, frame = capture.read()
            if ok:
                frames.append(frame)
    finally:
        capture.release()
    if len(frames) < 3:
        raise ValueError("not enough frames to build the global median")
    value = np.median(np.stack(frames), axis=0).astype(np.uint8)
    temporary = cache.with_suffix(".tmp.npy")
    np.save(temporary, value)
    os.replace(temporary, cache)
    return value


def read_frames(video: Path, start: int, end: int) -> np.ndarray:
    capture = cv2.VideoCapture(str(video))
    if not capture.isOpened():
        raise ValueError(f"cannot open video: {video}")
    capture.set(cv2.CAP_PROP_POS_FRAMES, start)
    frames = []
    try:
        for _ in range(start, end):
            ok, frame = capture.read()
            if not ok:
                break
            frames.append(frame)
    finally:
        capture.release()
    if len(frames) != end - start:
        raise ValueError(f"decoded {len(frames)} frames, expected {end - start} at {start}:{end}")
    return np.asarray(frames)


def predict_frames_with_global_median(
    pipeline,
    frames: np.ndarray,
    frame_ids: np.ndarray,
    *,
    eval_mode: str,
    batch_size: int,
    width: int,
    height: int,
    fps: float,
    median: np.ndarray,
):
    """Run a frame block while keeping one background median for the whole video.

    BadmintonTrackNet normally calculates the background from each supplied
    frame block.  Calling its window primitives here keeps chunked inference
    reproducible without requiring a local patch to the third-party package.
    """
    step = pipeline.seq_len if eval_mode == "nonoverlap" else 1
    pad = eval_mode == "nonoverlap" or len(frames) < pipeline.seq_len
    windows = iter_windows(frame_ids, frames, pipeline.seq_len, step, pad=pad)
    background = median if pipeline.bg_mode else None
    trajectory = pipeline._predict_windows(
        windows, eval_mode, batch_size, width, height, fps, background
    )
    return (
        pipeline._inpaint(trajectory, eval_mode, batch_size)
        if pipeline.inpaintnet
        else trajectory
    )


def completed_chunks(chunks_dir: Path) -> dict[int, Path]:
    result = {}
    for path in chunks_dir.glob("chunk-*.csv"):
        try:
            result[int(path.stem.split("-")[-1])] = path
        except ValueError:
            continue
    return result


def rebuild_partial(chunks_dir: Path, output: Path) -> pd.DataFrame:
    paths = sorted(completed_chunks(chunks_dir).values())
    if not paths:
        data = pd.DataFrame(columns=["Frame", "Visibility", "X", "Y"])
    else:
        data = pd.concat((pd.read_csv(path) for path in paths), ignore_index=True)
        data = data.sort_values("Frame").drop_duplicates("Frame", keep="last")
    atomic_csv(output, data)
    return data


def locate_ffmpeg(explicit: Path | None, project_root: Path) -> Path:
    if explicit:
        return explicit.resolve()
    bundled = project_root / ".tools" / "ffmpeg" / "ffmpeg-9.0.1-full_build-shared" / "bin" / "ffmpeg.exe"
    if bundled.exists():
        return bundled
    raise FileNotFoundError("ffmpeg was not supplied and the project-local executable was not found")


def create_clip(ffmpeg: Path, video: Path, output: Path, start: float, end: float) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    command = [
        str(ffmpeg), "-hide_banner", "-loglevel", "error", "-y", "-nostdin",
        "-ss", f"{max(0.0, start - 0.5):.3f}", "-to", f"{end + 0.8:.3f}",
        "-i", str(video), "-map", "0:v:0", "-an", "-c:v", "libx264",
        "-preset", "veryfast", "-crf", "18", "-movflags", "+faststart", str(output),
    ]
    subprocess.run(command, check=True)


def write_rally_index(output_dir: Path, feed: list[dict]) -> None:
    columns = [
        "rally", "start_frame", "end_frame", "start_seconds", "end_seconds",
        "auto_hit_count", "manual_hit_count", "effective_hit_count", "review_status",
        "observed_hits", "inferred_gap_hits", "confidence",
    ]
    rows = [
        {
            "rally": entry["rally"],
            "start_frame": entry["startFrame"],
            "end_frame": entry["endFrame"],
            "start_seconds": entry["startSeconds"],
            "end_seconds": entry["endSeconds"],
            "auto_hit_count": entry["hitCount"],
            "manual_hit_count": entry.get("manualHitCount"),
            "effective_hit_count": (
                entry["manualHitCount"]
                if entry.get("manualHitCount") is not None
                else entry["hitCount"]
            ),
            "review_status": entry.get("reviewStatus", "unreviewed"),
            "observed_hits": entry["observedHits"],
            "inferred_gap_hits": entry["inferredGapHits"],
            "confidence": entry["confidence"],
        }
        for entry in feed
    ]
    atomic_csv(output_dir / "rally-index.csv", pd.DataFrame(rows, columns=columns))


def update_rallies(
    *,
    video: Path,
    partial_csv: Path,
    output_dir: Path,
    analyzer: Path,
    ffmpeg: Path,
    final: bool,
    clips_enabled: bool,
) -> list[dict]:
    analysis_dir = output_dir / "analysis"
    analysis_dir.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            sys.executable, str(analyzer), "--video", str(video), "--trajectory",
            str(partial_csv), "--output-dir", str(analysis_dir),
        ],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    rallies_path = analysis_dir / "rallies-provisional.csv"
    if not rallies_path.exists() or not rallies_path.stat().st_size:
        return []
    rallies = pd.read_csv(rallies_path)
    candidates = rallies if final else rallies.iloc[:-1]
    feed_path = output_dir / "rally-feed.json"
    feed = json.loads(feed_path.read_text(encoding="utf-8")) if feed_path.exists() else []
    emitted_starts = {int(item["startFrame"]) for item in feed}
    for row in candidates.itertuples():
        if int(row.start_frame) in emitted_starts:
            continue
        sequence = len(feed) + 1
        clip_name = f"rally-{sequence:04d}-{int(row.start_frame)}-{int(row.end_frame)}.mp4"
        clip_path = output_dir / "rallies" / clip_name
        if clips_enabled:
            create_clip(
                ffmpeg, video, clip_path, float(row.start_seconds), float(row.end_seconds)
            )
        item = {
            "rally": sequence,
            "startFrame": int(row.start_frame),
            "endFrame": int(row.end_frame),
            "startSeconds": float(row.start_seconds),
            "endSeconds": float(row.end_seconds),
            "hitCount": int(row.hit_count),
            "manualHitCount": None,
            "effectiveHitCount": int(row.hit_count),
            "reviewStatus": "unreviewed",
            "observedHits": int(row.observed_hits),
            "inferredGapHits": int(row.inferred_gap_hits),
            "confidence": str(row.confidence),
            "clip": str(clip_path.resolve()) if clips_enabled else None,
        }
        feed.append(item)
        emitted_starts.add(int(row.start_frame))
        atomic_json(feed_path, feed)
        print(
            f"RALLY_READY rally={sequence} hits={item['hitCount']} "
            f"time={item['startSeconds']:.2f}-{item['endSeconds']:.2f}s "
            f"clip={item['clip']}",
            flush=True,
        )
    write_rally_index(output_dir, feed)
    return feed


def main() -> None:
    args = parse_args()
    if args.chunk_seconds <= 0 or args.overlap_seconds < 0 or args.batch_size <= 0:
        raise ValueError("chunk seconds and batch size must be positive; overlap cannot be negative")
    if args.max_rallies is not None and args.max_rallies <= 0:
        raise ValueError("max rallies must be positive")
    args.video = args.video.resolve()
    args.tracknet_file = args.tracknet_file.resolve()
    args.output_dir = args.output_dir.resolve()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    progress_path = args.output_dir / "progress.json"

    def pause_handler(_signum, _frame) -> None:
        progress = (
            json.loads(progress_path.read_text(encoding="utf-8"))
            if progress_path.exists()
            else {}
        )
        progress.update({"status": "paused", "updatedAtUnix": time.time()})
        atomic_json(progress_path, progress)
        print("PAUSED progress was saved", flush=True)
        raise KeyboardInterrupt

    signal.signal(signal.SIGINT, pause_handler)
    chunks_dir = args.output_dir / "chunks"
    chunks_dir.mkdir(parents=True, exist_ok=True)
    project_root = Path(__file__).resolve().parent.parent
    analyzer = (args.analyzer or project_root / "validation" / "analyze_tracknet_rallies.py").resolve()
    ffmpeg = locate_ffmpeg(args.ffmpeg, project_root)

    metadata = inspect_video(args.video)
    run_fingerprint = fingerprint(args.video, args.tracknet_file, metadata, args)
    fingerprint_path = args.output_dir / "run-fingerprint.json"
    if fingerprint_path.exists():
        existing = json.loads(fingerprint_path.read_text(encoding="utf-8"))
        if existing != run_fingerprint:
            raise ValueError("output directory belongs to a different input or chunk configuration")
    else:
        atomic_json(fingerprint_path, run_fingerprint)

    total_frames = metadata["frames"]
    target_frames = min(total_frames, args.max_frames or total_frames)
    core_frames = max(8, int(round(args.chunk_seconds * metadata["fps"])))
    overlap_frames = max(7, int(round(args.overlap_seconds * metadata["fps"])))
    chunk_count = int(math.ceil(target_frames / core_frames))
    finished = completed_chunks(chunks_dir)
    finished_at_start = set(finished)
    partial_csv = args.output_dir / "trajectory-partial.csv"
    if finished:
        rebuild_partial(chunks_dir, partial_csv)

    median = global_median(
        args.video, metadata, args.median_samples, args.output_dir / "background-median.npy"
    )
    pipeline_args = SimpleNamespace(
        device="cuda",
        tracknet_file=str(args.tracknet_file),
        inpaintnet_file="",
    )
    pipeline = _load_pipeline(pipeline_args)
    started = time.monotonic()

    for chunk_index in range(chunk_count):
        core_start = chunk_index * core_frames
        core_end = min(target_frames, core_start + core_frames)
        chunk_path = chunks_dir / f"chunk-{chunk_index:05d}.csv"
        if chunk_path.exists():
            continue
        read_start = max(0, core_start - overlap_frames)
        read_end = min(total_frames, core_end + overlap_frames)
        if args.eval_mode == "nonoverlap":
            # Non-overlapping inference must use one global sequence grid.  If
            # each chunk starts its own grid, predictions change when the chunk
            # duration changes and seams can look like trajectory events.
            read_start -= read_start % pipeline.seq_len
        print(
            f"CHUNK_START chunk={chunk_index + 1}/{chunk_count} "
            f"core={core_start}:{core_end} context={read_start}:{read_end}",
            flush=True,
        )
        frames = read_frames(args.video, read_start, read_end)
        frame_ids = np.arange(read_start, read_end, dtype=np.int64)

        attempted = []
        trajectory = None
        for batch_size in dict.fromkeys([args.batch_size, max(1, args.batch_size // 2), 2, 1]):
            attempted.append(batch_size)
            try:
                trajectory = predict_frames_with_global_median(
                    pipeline,
                    frames,
                    frame_ids,
                    eval_mode=args.eval_mode,
                    batch_size=batch_size,
                    width=metadata["width"],
                    height=metadata["height"],
                    fps=metadata["fps"],
                    median=median,
                )
                break
            except torch.cuda.OutOfMemoryError:
                torch.cuda.empty_cache()
        if trajectory is None:
            raise RuntimeError(f"chunk {chunk_index} failed at batch sizes {attempted}")
        data = trajectory.to_dataframe()
        data = data[(data.Frame >= core_start) & (data.Frame < core_end)].copy()
        if len(data) != core_end - core_start:
            raise ValueError(
                f"chunk {chunk_index} produced {len(data)} core frames, expected {core_end - core_start}"
            )
        atomic_csv(chunk_path, data)
        del frames, trajectory, data
        torch.cuda.empty_cache()
        partial = rebuild_partial(chunks_dir, partial_csv)
        completed_frames = len(partial[partial.Frame < target_frames])
        elapsed = time.monotonic() - started
        newly_completed = sum(
            min(target_frames, (index + 1) * core_frames) - index * core_frames
            for index in completed_chunks(chunks_dir)
            if index < chunk_count and index not in finished_at_start
        )
        rate = newly_completed / elapsed if elapsed > 0 else 0.0
        remaining = max(0, target_frames - completed_frames)
        progress = {
            "status": "running" if completed_frames < target_frames else "complete",
            "framesCompleted": completed_frames,
            "framesTarget": target_frames,
            "framesInVideo": total_frames,
            "percent": round(100.0 * completed_frames / target_frames, 2),
            "currentChunk": chunk_index + 1,
            "chunkCount": chunk_count,
            "fps": metadata["fps"],
            "inferenceFramesPerSecondThisRun": round(rate, 3),
            "etaSeconds": round(remaining / rate, 1) if rate > 0 else None,
            "updatedAtUnix": time.time(),
        }
        atomic_json(progress_path, progress)
        print(
            f"PROGRESS frames={completed_frames}/{target_frames} "
            f"percent={progress['percent']:.2f} eta={progress['etaSeconds']}s",
            flush=True,
        )
        feed = update_rallies(
            video=args.video,
            partial_csv=partial_csv,
            output_dir=args.output_dir,
            analyzer=analyzer,
            ffmpeg=ffmpeg,
            final=completed_frames >= target_frames,
            clips_enabled=args.write_rally_clips,
        )
        if args.max_rallies is not None and len(feed) >= args.max_rallies:
            progress.update(
                {
                    "status": "sample_complete",
                    "ralliesReady": len(feed),
                    "stoppedAfterRallies": args.max_rallies,
                }
            )
            atomic_json(progress_path, progress)
            print(
                f"SAMPLE_COMPLETE rallies={len(feed)} frames={completed_frames}/{target_frames}",
                flush=True,
            )
            return

    final_data = rebuild_partial(chunks_dir, partial_csv)
    final = len(final_data[final_data.Frame < target_frames]) >= target_frames
    feed = update_rallies(
        video=args.video,
        partial_csv=partial_csv,
        output_dir=args.output_dir,
        analyzer=analyzer,
        ffmpeg=ffmpeg,
        final=final,
        clips_enabled=args.write_rally_clips,
    )
    progress = json.loads(progress_path.read_text(encoding="utf-8")) if progress_path.exists() else {}
    progress.update({"status": "complete" if final else "partial", "ralliesReady": len(feed)})
    atomic_json(progress_path, progress)
    print(f"DONE status={progress['status']} rallies={len(feed)} output={args.output_dir}", flush=True)


if __name__ == "__main__":
    main()
