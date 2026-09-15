"""Build the parity fixtures the C++ kernel is verified against (M2 step 1-3).

Everything here is derived from artifacts the PyTorch run already produced, so the
C++ side is compared against the *product* path rather than a re-implementation:

  frames.u8.bin        raw BGR frames of the sampled windows (what preprocess eats)
  windows.txt          "<global_window> <frame_id 0..7>" per dumped window
  median.u8.bin        global background median, BGR uint8 (H,W,3)
  preprocessed.f32.bin same windows after preprocessing (== value/255 of the golden
                       uint8 tensor), so a bitwise match is possible
  heatmaps-ref.f32.bin PyTorch heatmaps for the same windows
  decoded-golden.txt   "<window> <frame> <x> <y> <visible>" decoded by the Python
                       reference implementation, for the C++ decode to match

The window grid is rebuilt with the project's own runner primitives, so shapes and
frame ids match the run that produced the golden set.

Dev-only tool.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import sys
from pathlib import Path

import cv2
import numpy as np


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--golden-dir", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--project-root", type=Path, default=None)
    parser.add_argument("--video", type=Path, default=None,
                        help="override; defaults to the video recorded in meta.json")
    parser.add_argument("--windows", type=int, default=8,
                        help="how many sampled windows to dump (8 = one product batch)")
    return parser.parse_args()


def ensure_vendor_on_path(project_root: Path) -> None:
    vendor = (project_root / ".tools" / "vendor" / "BadmintonTrackNet").resolve()
    if str(vendor) not in sys.path:
        sys.path.insert(0, str(vendor))


def load_runner(path: Path):
    spec = importlib.util.spec_from_file_location("chunked_runner", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def decode_heatmap(heatmap: np.ndarray, width: int, height: int,
                   threshold: float = 0.5) -> tuple[float, float]:
    binary = (heatmap > threshold).astype(np.uint8)
    contours, _ = cv2.findContours(binary, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    if not contours:
        return 0.0, 0.0
    x, y, w, h = cv2.boundingRect(max(contours, key=cv2.contourArea))
    return (x + w / 2) * width / 512, (y + h / 2) * height / 288


def main() -> None:
    args = parse_args()
    project_root = (args.project_root or Path(__file__).resolve().parent.parent).resolve()
    ensure_vendor_on_path(project_root)
    runner = load_runner(project_root / "validation" / "run_chunked_tracknet.py")

    from tracknet.data.video import iter_windows

    meta = json.loads((args.golden_dir / "meta.json").read_text(encoding="utf-8"))
    video = args.video or Path(meta["video"]["path"])
    width, height = int(meta["video"]["width"]), int(meta["video"]["height"])
    fps = float(meta["video"]["fps"])
    total_frames = int(meta["video"]["frames"])
    analysis = meta["analysis"]
    eval_mode = analysis["evalMode"]
    batch_size = int(analysis["batchSize"])
    core_frames = int(analysis["coreFramesPerChunk"])
    overlap_frames = int(analysis["overlapFrames"])
    analyzed_frames = int(analysis["analyzedFrames"])
    kept_batches = list(meta["samples"]["windowIndex"])

    metadata = runner.inspect_video(video)
    inputs = np.load(args.golden_dir / "input-windows.npz")["inputs"]
    heatmaps = np.load(args.golden_dir / "heatmaps-pytorch.npz")["heatmaps"]
    available = inputs.shape[0]
    wanted = min(args.windows, available)
    seq_len = inputs.shape[1] and 8  # checkpoint seq_len; validated below by shape

    args.out_dir.mkdir(parents=True, exist_ok=True)

    median = np.load(args.golden_dir / "background-median.npy").astype(np.uint8)
    (args.out_dir / "median.u8.bin").write_bytes(np.ascontiguousarray(median).tobytes())

    frames_path = args.out_dir / "frames.u8.bin"
    index_lines: list[str] = []
    dumped = 0
    frame_bytes = width * height * 3

    session = runner._load_pipeline(type("A", (), {
        "device": "cpu", "tracknet_file": str(Path(meta["model"]["path"])),
        "inpaintnet_file": ""})())
    seq_len = int(session.seq_len)
    if inputs.shape[1] != seq_len * 3 + 3:
        raise ValueError(f"golden channels {inputs.shape[1]} != (seq_len+1)*3 for seq_len={seq_len}")
    chunk_count = int(np.ceil(analyzed_frames / core_frames))

    with frames_path.open("wb") as sink:
        global_window = 0
        batch_index = 0
        for chunk_index in range(chunk_count):
            core_start = chunk_index * core_frames
            core_end = min(analyzed_frames, core_start + core_frames)
            read_start = max(0, core_start - overlap_frames)
            read_end = min(total_frames, core_end + overlap_frames)
            if eval_mode == "nonoverlap":
                read_start -= read_start % seq_len
            frames = runner.read_frames(video, read_start, read_end)
            frame_ids = np.arange(read_start, read_end, dtype=np.int64)
            step = seq_len if eval_mode == "nonoverlap" else 1
            pad = eval_mode == "nonoverlap" or len(frames) < seq_len
            pending: list = []
            for window in iter_windows(frame_ids, frames, seq_len, step, pad=pad):
                pending.append(window)
                if len(pending) < batch_size:
                    continue
                if batch_index in kept_batches and dumped < wanted:
                    for item in pending:
                        sink.write(np.ascontiguousarray(item.frames, dtype=np.uint8).tobytes())
                        index_lines.append(
                            f"{global_window} " + " ".join(str(int(i)) for i in item.frame_ids))
                        dumped += 1
                        global_window += 1
                    if dumped >= wanted:
                        break
                elif batch_index in kept_batches:
                    global_window += len(pending)
                else:
                    global_window += len(pending)
                batch_index += 1
                pending = []
            if dumped >= wanted:
                break
        if dumped < wanted:
            raise ValueError(f"only dumped {dumped} windows, wanted {wanted}")

    (args.out_dir / "windows.txt").write_text("\n".join(index_lines) + "\n", encoding="ascii")

    preprocessed = inputs[:wanted].astype(np.float32) / 255.0
    (args.out_dir / "preprocessed.f32.bin").write_bytes(
        np.ascontiguousarray(preprocessed).tobytes())
    reference = heatmaps[:wanted].astype(np.float32)
    (args.out_dir / "heatmaps-ref.f32.bin").write_bytes(np.ascontiguousarray(reference).tobytes())

    lines: list[str] = []
    for window in range(wanted):
        for position in range(seq_len):
            x, y = decode_heatmap(reference[window, position], width, height)
            lines.append(f"{window} {position} {x:.6f} {y:.6f} {1 if (x or y) else 0}")
    (args.out_dir / "decoded-golden.txt").write_text("\n".join(lines) + "\n", encoding="ascii")

    print(f"video={video.name} {width}x{height} fps={fps:.3f} seq_len={seq_len} "
          f"batch={batch_size} kept_batches={kept_batches}")
    print(f"windows dumped       {dumped}")
    print(f"frames.u8.bin        {frames_path.stat().st_size / 2**20:.1f} MiB")
    print(f"median.u8.bin        {(args.out_dir / 'median.u8.bin').stat().st_size / 2**20:.2f} MiB")
    print(f"preprocessed.f32.bin {preprocessed.nbytes / 2**20:.1f} MiB")
    print(f"heatmaps-ref.f32.bin {reference.nbytes / 2**20:.1f} MiB")
    print(f"decoded-golden.txt   {len(lines)} rows")
    print(f"golden frame bytes   {frame_bytes} per frame")


if __name__ == "__main__":
    main()
