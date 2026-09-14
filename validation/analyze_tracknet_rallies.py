"""Create provisional rally and hit counts from a raw TrackNet trajectory.

This is a validation heuristic, not a trained event classifier.  It keeps the
important distinction between visible TrackNet observations and events inferred
inside a short out-of-frame gap.
"""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np
import pandas as pd


@dataclass
class VideoInfo:
    fps: float
    width: int
    height: int
    frames: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--video", required=True, type=Path)
    parser.add_argument("--trajectory", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--split-gap-seconds", type=float, default=0.35)
    parser.add_argument("--top-edge-fraction", type=float, default=0.22)
    parser.add_argument("--min-rally-hits", type=int, default=2)
    parser.add_argument("--min-rally-seconds", type=float, default=0.8)
    parser.add_argument("--smooth-seconds", type=float, default=0.22)
    parser.add_argument("--turn-window-seconds", type=float, default=0.24)
    parser.add_argument("--merge-seconds", type=float, default=0.32)
    parser.add_argument("--min-turn-diagonal-fraction", type=float, default=0.02)
    return parser.parse_args()


def video_info(path: Path) -> VideoInfo:
    capture = cv2.VideoCapture(str(path))
    if not capture.isOpened():
        raise ValueError(f"cannot open video: {path}")
    info = VideoInfo(
        fps=float(capture.get(cv2.CAP_PROP_FPS)),
        width=int(capture.get(cv2.CAP_PROP_FRAME_WIDTH)),
        height=int(capture.get(cv2.CAP_PROP_FRAME_HEIGHT)),
        frames=int(capture.get(cv2.CAP_PROP_FRAME_COUNT)),
    )
    capture.release()
    if info.fps <= 0 or min(info.width, info.height) <= 0:
        raise ValueError(f"invalid video metadata: {path}")
    return info


def odd_frames(seconds: float, fps: float, minimum: int = 3) -> int:
    value = max(minimum, int(round(seconds * fps)))
    return value if value % 2 else value + 1


def smooth(values: np.ndarray, window: int) -> np.ndarray:
    if len(values) < 3 or window <= 1:
        return values.astype(float, copy=True)
    window = min(window, len(values) if len(values) % 2 else len(values) - 1)
    if window < 3:
        return values.astype(float, copy=True)
    pad = window // 2
    kernel = np.ones(window, dtype=float) / window
    padded = np.pad(values.astype(float), (pad, pad), mode="edge")
    return np.convolve(padded, kernel, mode="valid")


def invisible_gaps(frame: np.ndarray, visible: np.ndarray) -> list[tuple[int, int, int, int]]:
    """Return (previous visible position, next visible position, first/last gap positions)."""
    positions = np.flatnonzero(visible)
    result: list[tuple[int, int, int, int]] = []
    for left, right in zip(positions[:-1], positions[1:]):
        if right > left + 1:
            result.append((int(left), int(right), int(left + 1), int(right - 1)))
    return result


def rally_ranges(data: pd.DataFrame, info: VideoInfo, args: argparse.Namespace) -> list[tuple[int, int]]:
    visible = data["Visibility"].to_numpy(dtype=bool)
    positions = np.flatnonzero(visible)
    if not len(positions):
        return []
    split_after: list[int] = []
    minimum_gap = max(1, int(round(args.split_gap_seconds * info.fps)))
    restart_gap = max(2, int(round(0.10 * info.fps)))
    diagonal = float(np.hypot(info.width, info.height))

    def visible_speed(positions: np.ndarray) -> np.ndarray:
        if len(positions) < 2:
            return np.array([], dtype=float)
        points = data.iloc[positions][["X", "Y"]].to_numpy(dtype=float)
        frames = data.iloc[positions]["Frame"].to_numpy(dtype=float)
        delta_frames = np.maximum(1.0, np.diff(frames))
        return np.hypot(np.diff(points[:, 0]), np.diff(points[:, 1])) / delta_frames

    for left, right, first_gap, last_gap in invisible_gaps(
        data["Frame"].to_numpy(), visible
    ):
        gap_size = last_gap - first_gap + 1
        if gap_size < restart_gap:
            continue
        left_y = float(data.iloc[left].Y) / info.height
        right_y = float(data.iloc[right].Y) / info.height
        # A disappearance at the top is normally an out-of-frame clear and must
        # not end the rally.  An interior/lower disappearance followed by a new
        # interior trajectory is a landing/restart candidate even if close.
        interior_restart = (
            gap_size >= minimum_gap
            and left_y >= args.top_edge_fraction
            and right_y >= args.top_edge_fraction
        )
        very_long_gap = gap_size >= int(round(3.0 * info.fps))
        before_positions = positions[(positions <= left) & (positions >= left - int(0.30 * info.fps))]
        after_positions = positions[(positions >= right) & (positions <= right + int(0.45 * info.fps))]
        before_speed = visible_speed(before_positions)
        after_speed = visible_speed(after_positions)
        reset_distance = float(
            np.hypot(
                float(data.iloc[right].X) - float(data.iloc[left].X),
                float(data.iloc[right].Y) - float(data.iloc[left].Y),
            )
        )
        low_speed_restart = (
            left_y >= args.top_edge_fraction
            and right_y >= args.top_edge_fraction
            and len(before_speed) >= 2
            and len(after_speed) >= 2
            and float(np.median(before_speed[-3:])) <= 0.004 * diagonal
            and float(np.max(after_speed)) >= 0.01 * diagonal
            and reset_distance >= 0.03 * diagonal
        )
        if interior_restart or very_long_gap or low_speed_restart:
            split_after.append(left)

    ranges: list[tuple[int, int]] = []
    start = int(positions[0])
    for left in split_after:
        end = int(left)
        if end >= start:
            ranges.append((start, end))
        following = positions[positions > left]
        if len(following):
            start = int(following[0])
    end = int(positions[-1])
    if end >= start:
        ranges.append((start, end))
    return ranges


def axis_extrema(
    values: np.ndarray, window: int, prominence: float
) -> list[tuple[int, float]]:
    found: list[tuple[int, float]] = []
    for sign in (1.0, -1.0):
        signal = values * sign
        for index in range(window, len(signal) - window):
            before = signal[index - window : index]
            after = signal[index + 1 : index + window + 1]
            if signal[index] < before.max() or signal[index] <= after.max():
                continue
            strength = min(signal[index] - before.min(), signal[index] - after.min())
            if strength >= prominence:
                found.append((index, float(strength)))
    return found


def hit_candidates(
    segment: pd.DataFrame, info: VideoInfo, args: argparse.Namespace
) -> list[dict]:
    visibility = segment["Visibility"].to_numpy(dtype=bool)
    coordinate_mask = np.repeat(visibility[:, None], 2, axis=1)
    coordinates = segment[["X", "Y"]].where(coordinate_mask).interpolate(
        limit_direction="both"
    )
    if coordinates.isna().any().any():
        return []
    smooth_window = odd_frames(args.smooth_seconds, info.fps)
    turn_window = max(2, int(round(args.turn_window_seconds * info.fps)))
    diagonal = float(np.hypot(info.width, info.height))
    prominence = args.min_turn_diagonal_fraction * diagonal
    x = smooth(coordinates["X"].to_numpy(), smooth_window)
    y = smooth(coordinates["Y"].to_numpy(), smooth_window)
    raw = axis_extrema(x, turn_window, prominence)
    raw.extend(axis_extrema(y, turn_window, prominence))
    raw.sort()

    merge_frames = max(1, int(round(args.merge_seconds * info.fps)))
    merged: list[tuple[int, float]] = []
    for position, strength in raw:
        if not merged or position - merged[-1][0] >= merge_frames:
            merged.append((position, strength))
        elif strength > merged[-1][1]:
            merged[-1] = (position, strength)

    first_visible = int(np.flatnonzero(visibility)[0])
    events = [(first_visible, prominence, "serve_or_open_start")]
    events.extend((position, strength, "trajectory_turn") for position, strength in merged)
    events.sort()

    # Do not double-count a turn immediately after the initial serve/open boundary.
    deduplicated: list[tuple[int, float, str]] = []
    for event in events:
        if not deduplicated or event[0] - deduplicated[-1][0] >= merge_frames:
            deduplicated.append(event)
        elif event[1] > deduplicated[-1][1]:
            deduplicated[-1] = event

    result = []
    for position, strength, rule in deduplicated:
        row = segment.iloc[position]
        observed = bool(row.Visibility)
        result.append(
            {
                "frame": int(row.Frame),
                "seconds": round(float(row.Frame) / info.fps, 3),
                "x": round(float(coordinates.iloc[position].X), 2),
                "y": round(float(coordinates.iloc[position].Y), 2),
                "source": "observed" if observed else "inferred_gap",
                "rule": rule,
                "confidence": round(min(0.95, 0.55 + strength / diagonal), 3),
            }
        )
    return result


def main() -> None:
    args = parse_args()
    info = video_info(args.video)
    data = pd.read_csv(args.trajectory).sort_values("Frame").reset_index(drop=True)
    required = {"Frame", "Visibility", "X", "Y"}
    missing = required.difference(data.columns)
    if missing:
        raise ValueError(f"trajectory is missing columns: {sorted(missing)}")
    args.output_dir.mkdir(parents=True, exist_ok=True)

    rally_rows = []
    event_rows = []
    for start_position, end_position in rally_ranges(data, info, args):
        segment = data.iloc[start_position : end_position + 1].copy().reset_index(drop=True)
        hits = hit_candidates(segment, info, args)
        duration = (float(segment.iloc[-1].Frame) - float(segment.iloc[0].Frame)) / info.fps
        if len(hits) < args.min_rally_hits or duration < args.min_rally_seconds:
            continue
        rally_id = len(rally_rows) + 1
        for hit_id, hit in enumerate(hits, 1):
            event_rows.append({"rally": rally_id, "hit": hit_id, **hit})
        rally_rows.append(
            {
                "rally": rally_id,
                "start_frame": int(segment.iloc[0].Frame),
                "end_frame": int(segment.iloc[-1].Frame),
                "start_seconds": round(float(segment.iloc[0].Frame) / info.fps, 3),
                "end_seconds": round(float(segment.iloc[-1].Frame) / info.fps, 3),
                "hit_count": len(hits),
                "observed_hits": sum(hit["source"] == "observed" for hit in hits),
                "inferred_gap_hits": sum(hit["source"] == "inferred_gap" for hit in hits),
                "hit_frames": ";".join(str(hit["frame"]) for hit in hits),
                "hit_seconds": ";".join(f'{hit["seconds"]:.3f}' for hit in hits),
                "confidence": "provisional",
            }
        )

    rally_columns = [
        "rally", "start_frame", "end_frame", "start_seconds", "end_seconds",
        "hit_count", "observed_hits", "inferred_gap_hits", "hit_frames",
        "hit_seconds", "confidence",
    ]
    event_columns = [
        "rally", "hit", "frame", "seconds", "x", "y", "source", "rule",
        "confidence",
    ]
    rallies = pd.DataFrame(rally_rows, columns=rally_columns)
    events = pd.DataFrame(event_rows, columns=event_columns)
    rallies.to_csv(args.output_dir / "rallies-provisional.csv", index=False)
    events.to_csv(args.output_dir / "hits-provisional.csv", index=False)
    report = {
        "schemaVersion": 1,
        "status": "PROVISIONAL_REVIEW_REQUIRED",
        "audioUsed": False,
        "video": {
            "fps": info.fps,
            "width": info.width,
            "height": info.height,
            "frames": info.frames,
        },
        "trajectoryRows": len(data),
        "visibleFrames": int(data.Visibility.astype(bool).sum()),
        "rallyCount": len(rallies),
        "hitCounts": rallies.hit_count.astype(int).tolist() if len(rallies) else [],
        "limitations": [
            "TrackNet emits one shuttle position per frame; another active court can steal the global maximum before ROI filtering.",
            "Out-of-frame contacts are inferred from trajectory turns and are not direct observations.",
            "Counts require review until the court crop and event thresholds are validated on labelled long videos.",
        ],
        "parameters": {
            "splitGapSeconds": args.split_gap_seconds,
            "topEdgeFraction": args.top_edge_fraction,
            "smoothSeconds": args.smooth_seconds,
            "turnWindowSeconds": args.turn_window_seconds,
            "mergeSeconds": args.merge_seconds,
            "minTurnDiagonalFraction": args.min_turn_diagonal_fraction,
        },
    }
    (args.output_dir / "rally-analysis-summary.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
