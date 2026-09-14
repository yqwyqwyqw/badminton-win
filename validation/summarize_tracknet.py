"""Summarize raw TrackNet output and render a review overlay."""

from __future__ import annotations

import argparse
import json
import math
import time
from collections import deque
from pathlib import Path

import cv2
import numpy as np
import pandas as pd


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--video", required=True)
    parser.add_argument("--csv", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--history", type=int, default=12)
    return parser.parse_args()


def false_runs(values: np.ndarray) -> list[tuple[int, int, int]]:
    padded = np.r_[False, ~values, False].astype(np.int8)
    changes = np.diff(padded)
    starts = np.flatnonzero(changes == 1)
    ends = np.flatnonzero(changes == -1) - 1
    return [(int(start), int(end), int(end - start + 1)) for start, end in zip(starts, ends)]


def draw_frame(
    frame: np.ndarray,
    frame_id: int,
    fps: float,
    coordinate: tuple[int, int] | None,
    history: deque[tuple[int, int]],
    gap_length: int,
) -> np.ndarray:
    output = frame.copy()
    if coordinate is not None:
        history.appendleft(coordinate)
        for index, point in enumerate(history):
            radius = max(2, 7 - index // 2)
            cv2.circle(output, point, radius, (0, 255, 255), 2)
        status = "DETECTED"
        color = (0, 220, 0)
    else:
        status = f"NO DETECTION ({gap_length} frames)"
        color = (0, 0, 255)
    cv2.rectangle(output, (8, 8), (540, 72), (0, 0, 0), -1)
    cv2.putText(
        output,
        f"frame {frame_id}  time {frame_id / fps:.3f}s",
        (18, 34),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.65,
        (255, 255, 255),
        2,
        cv2.LINE_AA,
    )
    cv2.putText(
        output,
        status,
        (18, 62),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.65,
        color,
        2,
        cv2.LINE_AA,
    )
    return output


def main() -> int:
    args = parse_args()
    video_path = Path(args.video).resolve()
    csv_path = Path(args.csv).resolve()
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    table = pd.read_csv(csv_path)
    required = {"Frame", "Visibility", "X", "Y"}
    missing = required.difference(table.columns)
    if missing:
        raise ValueError(f"CSV is missing columns: {sorted(missing)}")
    table = table.sort_values("Frame").reset_index(drop=True)
    visibility = table["Visibility"].astype(bool).to_numpy()
    frames = table["Frame"].astype(int).to_numpy()
    x_values = table["X"].astype(float).to_numpy()
    y_values = table["Y"].astype(float).to_numpy()

    capture = cv2.VideoCapture(str(video_path))
    if not capture.isOpened():
        raise ValueError(f"Cannot open {video_path}")
    width = int(capture.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(capture.get(cv2.CAP_PROP_FRAME_HEIGHT))
    fps = float(capture.get(cv2.CAP_PROP_FPS))
    source_frame_count = int(capture.get(cv2.CAP_PROP_FRAME_COUNT))
    if fps <= 0 or width <= 0 or height <= 0:
        capture.release()
        raise ValueError("Invalid video metadata")

    runs = false_runs(visibility)
    visible_indices = np.flatnonzero(visibility)
    first_visible = int(visible_indices[0]) if len(visible_indices) else None
    last_visible = int(visible_indices[-1]) if len(visible_indices) else None
    inner_runs = [
        run
        for run in runs
        if first_visible is not None and run[0] >= first_visible and run[1] <= last_visible
    ]
    longest_runs = sorted(inner_runs, key=lambda item: item[2], reverse=True)[:20]

    diagonal = math.hypot(width, height)
    adjacent_jumps = []
    for index in range(1, len(table)):
        if not (visibility[index - 1] and visibility[index]):
            continue
        if frames[index] != frames[index - 1] + 1:
            continue
        distance = math.hypot(x_values[index] - x_values[index - 1], y_values[index] - y_values[index - 1])
        if distance > diagonal * 0.15:
            adjacent_jumps.append(
                {
                    "frame": int(frames[index]),
                    "distancePixels": round(distance, 2),
                    "distanceFractionOfDiagonal": round(distance / diagonal, 4),
                }
            )

    gap_lengths = np.zeros(len(table), dtype=np.int32)
    for start, end, length in runs:
        gap_lengths[start : end + 1] = length

    overlay_path = output_dir / "tracknet-raw-overlay.mp4"
    writer = cv2.VideoWriter(
        str(overlay_path), cv2.VideoWriter_fourcc(*"mp4v"), fps, (width, height)
    )
    if not writer.isOpened():
        capture.release()
        raise ValueError(f"Cannot open output writer: {overlay_path}")

    coordinates = {
        int(frame): (int(round(x)), int(round(y)))
        for frame, visible, x, y in zip(frames, visibility, x_values, y_values)
        if visible
    }
    history: deque[tuple[int, int]] = deque(maxlen=max(1, args.history))
    sample_ids = set(np.linspace(0, max(0, source_frame_count - 1), 12, dtype=int).tolist())
    sample_ids.update((start + end) // 2 for start, end, _ in longest_runs[:8])
    samples: list[tuple[int, np.ndarray]] = []

    started = time.perf_counter()
    frame_id = 0
    while True:
        ok, frame = capture.read()
        if not ok:
            break
        coordinate = coordinates.get(frame_id)
        rendered = draw_frame(
            frame,
            frame_id,
            fps,
            coordinate,
            history,
            int(gap_lengths[frame_id]) if frame_id < len(gap_lengths) else 0,
        )
        writer.write(rendered)
        if frame_id in sample_ids:
            samples.append((frame_id, rendered.copy()))
        frame_id += 1
    writer.release()
    capture.release()
    render_seconds = time.perf_counter() - started

    if samples:
        thumb_width = 480
        thumb_height = int(round(thumb_width * height / width))
        tiles = []
        for sample_id, sample in samples:
            tile = cv2.resize(sample, (thumb_width, thumb_height))
            cv2.putText(
                tile,
                f"#{sample_id} {sample_id / fps:.2f}s",
                (10, thumb_height - 12),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.6,
                (255, 255, 255),
                2,
                cv2.LINE_AA,
            )
            tiles.append(tile)
        columns = 3
        rows = math.ceil(len(tiles) / columns)
        blank = np.zeros_like(tiles[0])
        while len(tiles) < rows * columns:
            tiles.append(blank.copy())
        contact_rows = [np.hstack(tiles[row * columns : (row + 1) * columns]) for row in range(rows)]
        cv2.imwrite(str(output_dir / "tracknet-contact-sheet.jpg"), np.vstack(contact_rows))

    total_rows = len(table)
    visible_count = int(visibility.sum())
    longest_gap = longest_runs[0][2] if longest_runs else 0
    report = {
        "schemaVersion": 1,
        "groundTruthAvailable": False,
        "verdict": "MANUAL_REVIEW_REQUIRED",
        "source": {
            "video": str(video_path),
            "csv": str(csv_path),
            "width": width,
            "height": height,
            "fps": fps,
            "videoFrameCount": source_frame_count,
            "predictionRows": total_rows,
        },
        "rawTrackNet": {
            "visibleFrames": visible_count,
            "visiblePercent": round(100.0 * visible_count / total_rows, 2) if total_rows else 0.0,
            "firstVisibleFrame": first_visible,
            "lastVisibleFrame": last_visible,
            "longestInnerGapFrames": longest_gap,
            "longestInnerGapSeconds": round(longest_gap / fps, 3),
            "innerGapsAtLeastHalfSecond": sum(length >= fps * 0.5 for _, _, length in inner_runs),
            "innerGapsAtLeastOneSecond": sum(length >= fps for _, _, length in inner_runs),
            "longestInnerGaps": [
                {
                    "startFrame": start,
                    "endFrame": end,
                    "frames": length,
                    "startSeconds": round(start / fps, 3),
                    "endSeconds": round(end / fps, 3),
                    "seconds": round(length / fps, 3),
                }
                for start, end, length in longest_runs
            ],
            "adjacentJumpsOver15PercentDiagonal": adjacent_jumps,
        },
        "artifacts": {
            "overlayVideo": str(overlay_path),
            "contactSheet": str(output_dir / "tracknet-contact-sheet.jpg"),
        },
        "renderSeconds": round(render_seconds, 3),
        "notes": [
            "Visible percent is not recall without frame-level ground truth.",
            "The overlay must be reviewed for false positives and legitimate occlusion/out-of-frame gaps.",
            "The P1 requirement is recall >= 90% and longest unexplained gap < 0.5 seconds.",
        ],
    }
    (output_dir / "tracknet-summary.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8"
    )

    gap_rows = "\n".join(
        f"| {item['startSeconds']:.3f} | {item['endSeconds']:.3f} | {item['frames']} | {item['seconds']:.3f} |"
        for item in report["rawTrackNet"]["longestInnerGaps"][:10]
    )
    markdown = f"""# Raw TrackNet validation summary

- Verdict: **MANUAL_REVIEW_REQUIRED**
- Prediction rows: {total_rows}
- Visible frames: {visible_count} ({report['rawTrackNet']['visiblePercent']}%)
- Longest inner no-detection gap: {longest_gap} frames ({report['rawTrackNet']['longestInnerGapSeconds']} seconds)
- Gaps at least 0.5 seconds: {report['rawTrackNet']['innerGapsAtLeastHalfSecond']}
- Gaps at least 1.0 second: {report['rawTrackNet']['innerGapsAtLeastOneSecond']}
- Adjacent jumps over 15% of frame diagonal: {len(adjacent_jumps)}
- Overlay render time: {render_seconds:.3f} seconds

## Longest inner gaps

| Start (s) | End (s) | Frames | Duration (s) |
|---:|---:|---:|---:|
{gap_rows}

Visible percent is not recall because no frame-level ground truth exists yet. Review the overlay to decide which gaps are legitimate occlusion/out-of-frame intervals and which are model misses.
"""
    (output_dir / "tracknet-summary.md").write_text(markdown, encoding="utf-8")
    print(json.dumps(report["rawTrackNet"], indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
