"""Estimate a coarse playing-court ROI from a static-camera video.

This is a validation prototype. It finds the dominant floor surface around the
lower image centre on a temporal median, then derives a generous tracking
envelope. A production version should add court-line keypoints and manual corner
adjustment for ambiguous multi-court scenes.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import cv2
import numpy as np


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--video", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--samples", type=int, default=31)
    return parser.parse_args()


def sample_median(video: Path, count: int, size: tuple[int, int]) -> np.ndarray:
    capture = cv2.VideoCapture(str(video))
    if not capture.isOpened():
        raise ValueError(f"cannot open {video}")
    frame_count = int(capture.get(cv2.CAP_PROP_FRAME_COUNT))
    indices = np.linspace(0, max(0, frame_count - 1), max(3, count), dtype=int)
    frames = []
    for index in indices:
        capture.set(cv2.CAP_PROP_POS_FRAMES, int(index))
        ok, frame = capture.read()
        if ok:
            frames.append(cv2.resize(frame, size, interpolation=cv2.INTER_AREA))
    capture.release()
    if len(frames) < 3:
        raise ValueError("not enough decodable frames")
    return np.median(np.stack(frames), axis=0).astype(np.uint8)


def main() -> None:
    args = parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    width, height = 320, 180
    median = sample_median(args.video, args.samples, (width, height))

    lab = cv2.cvtColor(cv2.GaussianBlur(median, (5, 5), 0), cv2.COLOR_BGR2LAB)
    pixels = lab.reshape(-1, 3).astype(np.float32)
    criteria = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 40, 0.2)
    _, labels, centers = cv2.kmeans(
        pixels, 7, None, criteria, 5, cv2.KMEANS_PP_CENTERS
    )
    labels = labels.reshape(height, width)

    centre = labels[int(height * 0.55) : int(height * 0.90), int(width * 0.25) : int(width * 0.75)]
    floor_label = int(np.bincount(centre.ravel()).argmax())
    mask = (labels == floor_label).astype(np.uint8) * 255
    mask[: int(height * 0.28)] = 0
    mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, np.ones((11, 11), np.uint8))
    mask = cv2.morphologyEx(mask, cv2.MORPH_OPEN, np.ones((5, 5), np.uint8))

    count, component_labels, stats, _ = cv2.connectedComponentsWithStats(mask)
    if count <= 1:
        raise ValueError("could not isolate a floor component")
    central_box = np.zeros_like(mask)
    central_box[int(height * 0.45) :, int(width * 0.2) : int(width * 0.8)] = 1
    scored = []
    for component in range(1, count):
        component_mask = component_labels == component
        area = int(stats[component, cv2.CC_STAT_AREA])
        overlap = int(np.logical_and(component_mask, central_box).sum())
        scored.append((area + 3 * overlap, component))
    selected = max(scored)[1]
    floor_mask = (component_labels == selected).astype(np.uint8) * 255
    contours, _ = cv2.findContours(floor_mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    contour = max(contours, key=cv2.contourArea)
    hull = cv2.convexHull(contour)
    epsilon = 0.018 * cv2.arcLength(hull, True)
    polygon = cv2.approxPolyDP(hull, epsilon, True).reshape(-1, 2)
    x, y, w, h = cv2.boundingRect(hull)

    margin = int(round(width * 0.08))
    left = max(0, x - margin)
    right = min(width - 1, x + w - 1 + margin)
    tracking_polygon = np.array([[left, 0], [right, 0], [right, height - 1], [left, height - 1]])

    overlay = median.copy()
    cv2.polylines(overlay, [polygon], True, (0, 255, 0), 2)
    cv2.polylines(overlay, [tracking_polygon], True, (255, 255, 0), 2)
    cv2.putText(overlay, "floor ROI", (10, height - 30), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 1)
    cv2.putText(overlay, "tracking envelope", (10, height - 10), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 0), 1)
    preview_path = args.output_dir / "court-roi-preview.png"
    preview = cv2.resize(overlay, (1280, 720))
    encoded, payload = cv2.imencode(".png", preview)
    if not encoded:
        raise ValueError(f"could not encode ROI preview: {preview_path}")
    payload.tofile(preview_path)

    def normalized(points: np.ndarray) -> list[list[float]]:
        return [[round(float(px) / width, 5), round(float(py) / height, 5)] for px, py in points]

    report = {
        "schemaVersion": 1,
        "method": "temporal-median-dominant-floor-cluster",
        "floorClusterLab": [round(float(value), 2) for value in centers[floor_label]],
        "floorPolygonNormalized": normalized(polygon),
        "trackingEnvelopeNormalized": normalized(tracking_polygon),
        "notes": [
            "Coordinates are normalized and are not tied to a fixed resolution.",
            "The tracking envelope extends above the floor so high clears and top-edge exits remain valid.",
            "Multi-court ambiguity still requires court-line keypoints or a user-adjustable four-corner confirmation.",
        ],
    }
    (args.output_dir / "court-roi.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=False))


if __name__ == "__main__":
    main()
