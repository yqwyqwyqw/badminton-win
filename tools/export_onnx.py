"""Export TrackNet/InpaintNet to ONNX and prove numerical parity (M1).

Steps
  1. rebuild the module from the checkpoint's own `param_dict` (seq_len / bg_mode),
     using the vendored, unmodified model definition;
  2. export fp32 ONNX with a fixed 288x512 spatial shape and a dynamic batch axis;
  3. optionally convert to fp16 keeping fp32 IO (so the C++ side always feeds fp32);
  4. re-run ONNX Runtime on the frozen golden windows and compare against the
     PyTorch reference, both at heatmap level and after heatmap -> coordinate decode.

Dev-only tool.  Requires torch + onnx + onnxruntime (`.venv-validation`); never shipped.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import time
import warnings
from pathlib import Path

import cv2
import numpy as np
import onnx
import onnxruntime as ort
import torch

warnings.filterwarnings("ignore", category=UserWarning)   # fp16 truncation notices
warnings.filterwarnings("ignore", category=DeprecationWarning)  # legacy exporter notice

WIDTH = 512
HEIGHT = 288
THRESHOLD = 0.5


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tracknet-file", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--project-root", type=Path, default=None)
    parser.add_argument("--golden-dir", type=Path, default=None,
                        help="directory holding input-windows.npz / heatmaps-pytorch.npz")
    parser.add_argument("--opset", type=int, default=17)
    parser.add_argument("--skip-fp16", action="store_true")
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--parity-batch", type=int, default=4,
                        help="windows per ORT run during parity checks (CPU memory bound)")
    return parser.parse_args()


def ensure_vendor_on_path(project_root: Path) -> Path:
    vendor = (project_root / ".tools" / "vendor" / "BadmintonTrackNet").resolve()
    if not str(vendor) in sys.path:
        sys.path.insert(0, str(vendor))
    return vendor


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def decode_heatmap(heatmap: np.ndarray, width: int, height: int,
                   threshold: float = THRESHOLD) -> tuple[float, float]:
    """Exact copy of tracknet.inference.pipeline.InferencePipeline._decode_heatmap."""
    binary = (heatmap > threshold).astype(np.uint8)
    contours, _ = cv2.findContours(binary, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    if not contours:
        return 0.0, 0.0
    x, y, w, h = cv2.boundingRect(max(contours, key=cv2.contourArea))
    return (x + w / 2) * width / WIDTH, (y + h / 2) * height / HEIGHT


def make_session(path: Path, threads: int,
                 level: ort.GraphOptimizationLevel = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
                 ) -> ort.InferenceSession:
    options = ort.SessionOptions()
    options.intra_op_num_threads = threads
    options.graph_optimization_level = level
    return ort.InferenceSession(str(path), options, providers=["CPUExecutionProvider"])


def build_model(tracknet_file: Path):
    from tracknet.data.io import get_model

    checkpoint = torch.load(tracknet_file, map_location="cpu", weights_only=False)
    params = checkpoint["param_dict"]
    model = get_model("TrackNet", params["seq_len"], params.get("bg_mode", ""))
    model.load_state_dict(checkpoint["model"])
    model.eval()
    return model, params


def export_fp32(model, path: Path, opset: int) -> None:
    dummy = torch.zeros(1, 27, 288, 512, dtype=torch.float32)
    torch.onnx.export(
        model,
        dummy,
        str(path),
        input_names=["frames"],
        output_names=["heatmaps"],
        opset_version=opset,
        do_constant_folding=True,
        dynamic_axes={"frames": {0: "N"}, "heatmaps": {0: "N"}},
    )


def export_fp16(fp32_path: Path, fp16_path: Path, converter: str = "onnxconverter") -> None:
    """Convert an fp32 graph to fp16 (IO stays fp32 so callers always feed fp32)."""
    model = onnx.load(str(fp32_path))
    if converter == "onnxconverter":
        from onnxconverter_common import float16

        # `Resize` must keep float32 scales/roi per the ONNX spec, so it is blocked
        # from the conversion (the converter then inserts Cast nodes around it).
        converted = float16.convert_float_to_float16(
            model,
            keep_io_types=True,
            disable_shape_infer=True,
            op_block_list=["Resize"],
        )
    else:
        from onnxruntime.transformers import float16 as ort_float16

        converted = ort_float16.convert_float_to_float16(
            model, keep_io_types=True, disable_shape_infer=True)
    onnx.save(converted, str(fp16_path))


def try_load(path: Path) -> tuple[bool, str]:
    """Return (loadable, error) for an ONNX file, using ORT's own graph loader."""
    try:
        make_session(path, 1)
        return True, ""
    except Exception as error:  # noqa: BLE001 - surfaced to the caller verbatim
        return False, f"{type(error).__name__}: {error}".split("\n")[0][:300]


class SizedUpsampleTrackNet(torch.nn.Module):
    """Export-only wrapper: identical maths, but Upsample becomes Resize(sizes).

    `nn.Upsample(scale_factor=2)` exports as Resize with float `scales`, which the
    fp16 converters turn into float16 and thereby make the graph invalid (the ONNX
    spec requires float32 scales).  Expressing the same 2x upsampling as
    `F.interpolate(size=<skip shape>)` emits an int64 `sizes` input instead, which
    survives fp16 conversion.  parity with the plain export is asserted by the
    caller, so this wrapper cannot silently change the model.
    """

    def __init__(self, net: torch.nn.Module):
        super().__init__()
        self.net = net

    def forward(self, x):  # pragma: no cover - exercised through the exporter
        net = self.net
        x1 = net.down_block_1(x)
        x = torch.nn.functional.max_pool2d(x1, (2, 2), (2, 2))
        x2 = net.down_block_2(x)
        x = torch.nn.functional.max_pool2d(x2, (2, 2), (2, 2))
        x3 = net.down_block_3(x)
        x = torch.nn.functional.max_pool2d(x3, (2, 2), (2, 2))
        x = net.bottleneck(x)
        x = torch.cat([torch.nn.functional.interpolate(x, size=x3.shape[-2:], mode="nearest"), x3], 1)
        x = net.up_block_1(x)
        x = torch.cat([torch.nn.functional.interpolate(x, size=x2.shape[-2:], mode="nearest"), x2], 1)
        x = net.up_block_2(x)
        x = torch.cat([torch.nn.functional.interpolate(x, size=x1.shape[-2:], mode="nearest"), x1], 1)
        x = net.up_block_3(x)
        return net.sigmoid(net.predictor(x))


def compare(golden_dir: Path, session: ort.InferenceSession, width: int, height: int,
            precision: str, batch: int = 4) -> dict:
    inputs = np.load(golden_dir / "input-windows.npz")["inputs"].astype(np.float32) / 255.0
    reference = np.load(golden_dir / "heatmaps-pytorch.npz")["heatmaps"].astype(np.float32)
    if inputs.shape[0] != reference.shape[0] or inputs.shape[-2:] != reference.shape[-2:]:
        raise ValueError(f"golden batch/spatial mismatch: {inputs.shape} vs {reference.shape}")

    # Batching matters: ORT's CPU EP materialises an im2col buffer per convolution,
    # so a 32-window batch needs tens of GB at 288x512.  The product uses batch 8.
    produced = np.empty_like(reference)
    started = time.monotonic()
    for start in range(0, inputs.shape[0], batch):
        stop = min(start + batch, inputs.shape[0])
        produced[start:stop] = session.run(
            ["heatmaps"], {"frames": inputs[start:stop]})[0].astype(np.float32)
    elapsed = time.monotonic() - started

    diff = np.abs(produced - reference)
    total = diff.size
    max_abs = float(diff.max())
    mean_abs = float(diff.mean())
    p999 = float(np.percentile(diff, 99.9))
    over_001 = float((diff > 0.01).sum()) / total
    over_005 = float((diff > 0.05).sum()) / total

    windows, seq_len = produced.shape[0], produced.shape[1]
    mismatched_visibility = 0
    distances: list[float] = []
    for window in range(windows):
        for frame in range(seq_len):
            ref_point = decode_heatmap(reference[window, frame], width, height)
            onnx_point = decode_heatmap(produced[window, frame], width, height)
            ref_visible = ref_point != (0.0, 0.0)
            onnx_visible = onnx_point != (0.0, 0.0)
            if ref_visible != onnx_visible:
                mismatched_visibility += 1
            elif ref_visible:
                distances.append(float(np.hypot(ref_point[0] - onnx_point[0],
                                                ref_point[1] - onnx_point[1])))

    decoded_total = windows * seq_len
    return {
        "precision": precision,
        "windows": int(windows),
        "frames": int(decoded_total),
        "heatmap": {
            "maxAbsDiff": max_abs,
            "meanAbsDiff": mean_abs,
            "p999AbsDiff": p999,
            "fractionOver0.01": over_001,
            "fractionOver0.05": over_005,
        },
        "decoded": {
            "visibilityMismatch": int(mismatched_visibility),
            "visibilityMismatchFraction": mismatched_visibility / max(decoded_total, 1),
            "jointVisibleFrames": len(distances),
            "coordP50Px": float(np.percentile(distances, 50)) if distances else None,
            "coordP95Px": float(np.percentile(distances, 95)) if distances else None,
            "coordMaxPx": float(np.max(distances)) if distances else None,
        },
        "inferenceSeconds": round(elapsed, 3),
        "inferenceFps": round(decoded_total / (elapsed * seq_len) if elapsed > 0 else 0.0, 3),
    }


def verdict(parity: dict) -> tuple[bool, list[str]]:
    """Gate on decision-level agreement, not on raw single-pixel extremes.

    A handful of heatmap pixels can differ by a few 1e-3 because ONNX Runtime
    reorders/fuses the convolutions; what matters for the product is whether the
    decoded coordinates and the visibility decisions change.
    """
    notes: list[str] = []
    heat = parity["heatmap"]
    decoded = parity["decoded"]
    fp16 = parity["precision"] == "fp16"
    p999_limit = 5e-3 if fp16 else 1e-3
    max_limit = 2e-2 if fp16 else 1e-2
    if heat["p999AbsDiff"] > p999_limit:
        notes.append(f"heatmap p99.9 {heat['p999AbsDiff']:.2e} > {p999_limit:.0e}")
    if heat["maxAbsDiff"] > max_limit:
        notes.append(f"heatmap max {heat['maxAbsDiff']:.2e} > {max_limit:.0e}")
    if decoded["visibilityMismatchFraction"] > 0.005:
        notes.append(f"visibility mismatch {decoded['visibilityMismatchFraction']:.4%} > 0.5%")
    p95 = decoded["coordP95Px"]
    if p95 is None or p95 > 1.5:
        notes.append(f"coord P95 {p95} px > 1.5 px")
    return (not notes), notes


def main() -> None:
    args = parse_args()
    project_root = (args.project_root or Path(__file__).resolve().parent.parent).resolve()
    ensure_vendor_on_path(project_root)
    args.tracknet_file = args.tracknet_file.resolve()
    args.output_dir = args.output_dir.resolve()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    model, params = build_model(args.tracknet_file)
    parameters = sum(p.numel() for p in model.parameters())
    print(f"[M1] model rebuilt: seq_len={params['seq_len']} bg_mode={params.get('bg_mode','')!r} "
          f"params={parameters:,} ({parameters * 4 / 2**20:.1f} MiB fp32)", flush=True)

    fp32_path = args.output_dir / "tracknet-8f-concat-288x512-fp32.onnx"
    started = time.monotonic()
    export_fp32(model, fp32_path, args.opset)
    onnx.checker.check_model(str(fp32_path))
    print(f"[M1] exported fp32: {fp32_path.name} {fp32_path.stat().st_size / 2**20:.1f} MiB "
          f"({time.monotonic() - started:.1f}s)", flush=True)

    fp16_path = None
    fp16_strategy = None
    sized_fp32_path = None
    if not args.skip_fp16:
        plain_fp16 = args.output_dir / "tracknet-8f-concat-288x512-fp16.onnx"
        for converter in ("onnxconverter", "ort"):
            try:
                export_fp16(fp32_path, plain_fp16, converter)
                onnx.checker.check_model(str(plain_fp16))
            except Exception as error:  # noqa: BLE001
                print(f"[M1] fp16 via {converter}: conversion failed: {error}", flush=True)
                continue
            loadable, detail = try_load(plain_fp16)
            if loadable:
                fp16_path, fp16_strategy = plain_fp16, f"plain/{converter}"
                print(f"[M1] exported fp16 ({converter}): {plain_fp16.name} "
                      f"{plain_fp16.stat().st_size / 2**20:.1f} MiB", flush=True)
                break
            print(f"[M1] fp16 via {converter}: graph rejected by ORT: {detail}", flush=True)
            plain_fp16.unlink(missing_ok=True)

    if not args.skip_fp16 and fp16_path is None:
        # Fall back to a graph whose Resize takes int64 `sizes` (see the wrapper docs).
        print("[M1] fp16 fallback: re-exporting with int64 Resize sizes", flush=True)
        sized_model = SizedUpsampleTrackNet(model).eval()
        probe = torch.randn(1, 27, 288, 512, dtype=torch.float32)
        with torch.inference_mode():
            difference = float((model(probe) - sized_model(probe)).abs().max())
        print(f"[M1] wrapper equivalence on random input: max|d|={difference:.3e}", flush=True)
        if difference <= 1e-6:
            sized_fp32_path = args.output_dir / "tracknet-8f-concat-288x512-sizes-fp32.onnx"
            export_fp32(sized_model, sized_fp32_path, args.opset)
            onnx.checker.check_model(str(sized_fp32_path))
            candidate = args.output_dir / "tracknet-8f-concat-288x512-sizes-fp16.onnx"
            for converter in ("onnxconverter", "ort"):
                try:
                    export_fp16(sized_fp32_path, candidate, converter)
                    onnx.checker.check_model(str(candidate))
                except Exception as error:  # noqa: BLE001
                    print(f"[M1] sizes fp16 via {converter}: conversion failed: {error}", flush=True)
                    continue
                loadable, detail = try_load(candidate)
                if loadable:
                    fp16_path, fp16_strategy = candidate, f"sizes/{converter}"
                    print(f"[M1] exported fp16 ({converter}, int64 sizes): {candidate.name} "
                          f"{candidate.stat().st_size / 2**20:.1f} MiB", flush=True)
                    break
                print(f"[M1] sizes fp16 via {converter}: rejected: {detail}", flush=True)
        else:
            print("[M1] wrapper is not equivalent; fp16 disabled", flush=True)

    if fp16_path is None:
        print("[M1] fp16 not available; fp32 is the adopted model "
              "(43.3 MiB is inside the size budget)", flush=True)
    else:
        # Never leave a stale, ORT-loadable-failed fp16 file behind: the plain
        # graph is invalid under fp16 and must not be picked up by mistake.
        plain = args.output_dir / "tracknet-8f-concat-288x512-fp16.onnx"
        if plain != fp16_path and plain.exists():
            plain.unlink()
            print(f"[M1] removed stale non-loadable {plain.name}", flush=True)

    manifest = {
        "tracknet": {
            "sourceCheckpoint": str(args.tracknet_file),
            "sourceSha256": sha256(args.tracknet_file),
            "seqLen": int(params["seq_len"]),
            "bgMode": params.get("bg_mode", ""),
            "parameters": parameters,
            "inputShape": ["N", 27, 288, 512],
            "outputShape": ["N", 8, 288, 512],
            "opset": args.opset,
            "files": [],
        },
        "parity": [],
        "goldenDir": str(args.golden_dir) if args.golden_dir else None,
        "verifiedAtUnix": time.time(),
    }

    runs: list[tuple[str, Path, ort.GraphOptimizationLevel, bool]] = [
        ("fp32", fp32_path, ort.GraphOptimizationLevel.ORT_ENABLE_ALL, True),
        # Diagnostic only: with optimisations off we can tell whether the small
        # fp32 deltas come from ORT's graph rewrites rather than from the export.
        ("fp32-noopt", fp32_path, ort.GraphOptimizationLevel.ORT_DISABLE_ALL, False),
    ]
    if sized_fp32_path is not None:
        runs.append(("fp32-sizes", sized_fp32_path,
                     ort.GraphOptimizationLevel.ORT_ENABLE_ALL, False))
    if fp16_path is not None:
        runs.append(("fp16", fp16_path, ort.GraphOptimizationLevel.ORT_ENABLE_ALL, True))

    golden_dir = args.golden_dir or (project_root / "validation-output" / "golden"
                                     / "VID20260828212318_1")
    if golden_dir.is_dir() and (golden_dir / "input-windows.npz").exists():
        meta = json.loads((golden_dir / "meta.json").read_text(encoding="utf-8"))
        width, height = int(meta["video"]["width"]), int(meta["video"]["height"])
        for label, path, level, gated in runs:
            session = make_session(path, args.threads, level)
            parity = compare(golden_dir, session, width, height, label, args.parity_batch)
            parity["graphOptimization"] = level.name
            parity["gated"] = gated
            ok, notes = verdict(parity)
            parity["pass"] = ok if gated else None
            parity["failures"] = notes
            manifest["parity"].append(parity)
            status = ("PASS" if ok else "FAIL " + "; ".join(notes)) if gated else "diagnostic"
            print(f"[M1] {label:<11} max|d|={parity['heatmap']['maxAbsDiff']:.3e} "
                  f"p99.9={parity['heatmap']['p999AbsDiff']:.3e} "
                  f"visMismatch={parity['decoded']['visibilityMismatch']}/{parity['frames']} "
                  f"coordP95={parity['decoded']['coordP95Px']} "
                  f"cpuFps={parity['inferenceFps']:.2f} -> {status}", flush=True)
    else:
        print(f"[M1] golden set not found at {golden_dir}; exported without parity check", flush=True)

    produced = [fp32_path]
    if sized_fp32_path is not None:
        produced.append(sized_fp32_path)
    if fp16_path is not None:
        produced.append(fp16_path)
    for path in produced:
        manifest["tracknet"]["files"].append({
            "name": path.name,
            "bytes": path.stat().st_size,
            "sha256": sha256(path),
        })
    manifest["adopted"] = {
        "fp32": fp32_path.name,
        "fp16": fp16_path.name if fp16_path else None,
        "fp16Strategy": fp16_strategy,
        "note": "fp16 keeps float32 inputs/outputs; the caller always feeds float32",
    }
    (args.output_dir / "models.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"[M1] manifest -> {args.output_dir / 'models.json'}", flush=True)


if __name__ == "__main__":
    main()
