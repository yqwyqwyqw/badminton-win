"""Render TrackNet-only rally and hit-count review video."""

from __future__ import annotations

import argparse
from collections import deque
from pathlib import Path

import cv2
import pandas as pd


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--video", required=True, type=Path)
    parser.add_argument("--trajectory", required=True, type=Path)
    parser.add_argument("--rallies", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--min-x", type=float, default=0.0)
    parser.add_argument("--history", type=int, default=10)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    trajectory = pd.read_csv(args.trajectory).sort_values("Frame")
    rallies = pd.read_csv(args.rallies).sort_values("rally")
    points = {
        int(row.Frame): (float(row.X), float(row.Y), bool(row.Visibility))
        for row in trajectory.itertuples()
    }
    definitions = []
    for row in rallies.itertuples():
        hits = [int(value) for value in str(row.hit_frames).split(";") if value]
        definitions.append(
            {
                "id": int(row.rally),
                "start": int(row.start_frame),
                "end": int(row.end_frame),
                "hits": hits,
            }
        )

    capture = cv2.VideoCapture(str(args.video))
    if not capture.isOpened():
        raise ValueError(f"cannot open {args.video}")
    width = int(capture.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(capture.get(cv2.CAP_PROP_FRAME_HEIGHT))
    fps = float(capture.get(cv2.CAP_PROP_FPS))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    writer = cv2.VideoWriter(
        str(args.output), cv2.VideoWriter_fourcc(*"mp4v"), fps, (width, height)
    )
    if not writer.isOpened():
        capture.release()
        raise ValueError(f"cannot create {args.output}")

    history: deque[tuple[int, int]] = deque(maxlen=max(1, args.history))
    frame_id = 0
    try:
        while True:
            ok, frame = capture.read()
            if not ok:
                break
            active = next(
                (item for item in definitions if item["start"] <= frame_id <= item["end"]),
                None,
            )
            point = points.get(frame_id)
            accepted = point is not None and point[2] and point[0] >= args.min_x
            if accepted:
                history.appendleft((int(round(point[0])), int(round(point[1]))))
            for index, item in enumerate(history):
                cv2.circle(frame, item, max(2, 7 - index // 2), (0, 255, 255), 2)

            if point is not None and point[2] and not accepted:
                rejected = (int(round(point[0])), int(round(point[1])))
                cv2.drawMarker(frame, rejected, (128, 128, 128), cv2.MARKER_TILTED_CROSS, 12, 2)

            if active is None:
                label = "NO MAIN-COURT RALLY"
                color = (120, 120, 120)
            else:
                completed = sum(hit <= frame_id for hit in active["hits"])
                label = f"RALLY {active['id']}  HITS {completed}/{len(active['hits'])}"
                color = (0, 220, 0)
                nearby = [hit for hit in active["hits"] if abs(hit - frame_id) <= 4]
                if nearby:
                    label += "  HIT"
                    color = (255, 100, 0)

            cv2.rectangle(frame, (8, 8), (600, 76), (0, 0, 0), -1)
            cv2.putText(
                frame,
                f"frame {frame_id}  {frame_id / fps:.3f}s",
                (18, 34),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.7,
                (255, 255, 255),
                2,
                cv2.LINE_AA,
            )
            cv2.putText(
                frame, label, (18, 65), cv2.FONT_HERSHEY_SIMPLEX, 0.8, color, 2, cv2.LINE_AA
            )
            writer.write(frame)
            frame_id += 1
    finally:
        writer.release()
        capture.release()

    print(f"rendered {frame_id} frames to {args.output}")


if __name__ == "__main__":
    main()
