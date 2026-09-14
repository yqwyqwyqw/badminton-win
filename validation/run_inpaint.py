"""Apply the official InpaintNet checkpoint to an existing raw TrackNet CSV.

The output intentionally retains raw detector fields so interpolated positions are
never confused with observations made by TrackNet.
"""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

import pandas as pd
import torch

from tracknet.data import Trajectory
from tracknet.data.io import get_model
from tracknet.inference.pipeline import InferencePipeline


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-csv", required=True, type=Path)
    parser.add_argument("--checkpoint", required=True, type=Path)
    parser.add_argument("--output-csv", required=True, type=Path)
    parser.add_argument("--width", required=True, type=int)
    parser.add_argument("--height", required=True, type=int)
    parser.add_argument("--fps", required=True, type=float)
    parser.add_argument("--batch-size", type=int, default=64)
    parser.add_argument("--eval-mode", choices=("nonoverlap", "average", "weight"), default="weight")
    parser.add_argument("--device", choices=("auto", "cpu", "cuda", "mps"), default="auto")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    raw = pd.read_csv(args.input_csv).sort_values("Frame").reset_index(drop=True)
    trajectory = Trajectory.from_dataframe(
        raw, width=args.width, height=args.height, fps=args.fps
    )

    device = "cuda" if args.device == "auto" and torch.cuda.is_available() else args.device
    if device == "auto":
        device = "cpu"
    checkpoint = torch.load(args.checkpoint, map_location=device)
    model = get_model("InpaintNet")
    model.load_state_dict(checkpoint["model"])
    seq_len = int(checkpoint["param_dict"]["seq_len"])
    pipeline = InferencePipeline(
        torch.nn.Identity(),
        seq_len=seq_len,
        inpaintnet=model,
        inpaint_seq_len=seq_len,
        device=device,
    )

    started = time.perf_counter()
    repaired = pipeline.inpaint_trajectory(
        trajectory, eval_mode=args.eval_mode, batch_size=args.batch_size
    ).to_dataframe()
    elapsed = time.perf_counter() - started

    output = repaired.rename(
        columns={"Visibility": "FinalVisibility", "X": "FinalX", "Y": "FinalY"}
    )
    output["RawVisibility"] = raw["Visibility"].astype(int)
    output["RawX"] = raw["X"]
    output["RawY"] = raw["Y"]
    output["Inpainted"] = (
        (output["RawVisibility"] == 0) & (output["FinalVisibility"] == 1)
    ).astype(int)
    output = output[
        [
            "Frame",
            "RawVisibility",
            "RawX",
            "RawY",
            "FinalVisibility",
            "FinalX",
            "FinalY",
            "Inpainted",
        ]
    ]
    args.output_csv.parent.mkdir(parents=True, exist_ok=True)
    output.to_csv(args.output_csv, index=False)

    stats = {
        "device": device,
        "sequence_length": seq_len,
        "elapsed_seconds": round(elapsed, 3),
        "raw_visible_frames": int(output["RawVisibility"].sum()),
        "final_visible_frames": int(output["FinalVisibility"].sum()),
        "inpainted_frames": int(output["Inpainted"].sum()),
        "total_frames": len(output),
    }
    args.output_csv.with_suffix(".json").write_text(
        json.dumps(stats, indent=2), encoding="utf-8"
    )
    print(json.dumps(stats, ensure_ascii=False))


if __name__ == "__main__":
    main()
