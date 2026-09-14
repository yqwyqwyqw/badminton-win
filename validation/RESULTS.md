# Validation results

Test source: `F:\羽毛球视频\20260828\VID20260828212318_1.mp4`

The source video was only read. No files were written to its directory.

## A6 decode validation

- Result: `PASS_WITH_WARNING`
- Source: H.264 High, 3840x2160, 50 fps CFR, 56.46 seconds, 2,823 frames
- CPU decode and 720p scale: 220.24 fps, 4.40x real-time
- NVDEC/CUDA decode and CUDA scale: 542.36 fps, 10.85x real-time
- Decode errors: 0
- Warning: this source is 4K50, so the exact 4K60 target remains untested

## 720p proxy and timeline validation

- Result: `PASS_WITH_WARNING`
- Proxy: 1280x720 at 50 fps, 2,823 frames, 56.46 seconds
- Frame-count difference: 0
- Duration difference: 0 seconds
- Maximum per-frame timestamp drift: 0 seconds
- Random seek checks: 5/5 passed
- Proxy size: 5.72 MB
- Successful fallback encode: 5.288 seconds, 10.68x real-time

## Compatibility finding

NVDEC decoding works. NVENC encoding with FFmpeg 9.0.1 does not initialize because the build requires NVENC API 13.1 while the installed NVIDIA driver exposes 13.0. FFmpeg reports driver 610.00 or newer as its requirement. The validation tool therefore used CUDA decode/scale plus `libx264` software encoding.

The product must keep automatic software fallback. Before release, either pin an FFmpeg build compatible with the supported driver range or document a minimum NVIDIA driver version.

## P1 TrackNetV3 trajectory feasibility

- Result: `NOT_PASSED_ON_THIS_CLIP` (technical path runs; quality gate not met)
- Test input: 1280x720 at 50 fps, 2,823 frames
- Model path: official TrackNetV3 TrackNet checkpoint, weighted overlapping-window inference on CUDA
- Raw visible output: 1,984 / 2,823 frames (70.28% trajectory coverage)
- Longest raw inner gap: 104 frames / 2.08 seconds
- Raw inner gaps at least 0.5 seconds: 14; at least 1 second: 2
- Runtime: 242.94 seconds for 56.46 seconds of video, approximately 11.62 fps / 0.23x real-time
- InpaintNet added 212 inferred positions in 0.349 seconds; final visible positions: 2,196 / 2,823
- Raw and repaired visibility are stored separately because inferred coordinates are not detector observations

The high-quality CRF 10 proxy produced essentially the same result as the original CRF 23 validation proxy: 1,984 versus 1,983 visible frames, the same 2.08-second longest gap, and 1,976 jointly visible frames had a median coordinate difference of 0 pixels (95th percentile 1.25 pixels). Proxy compression is therefore not the main cause of the long gaps.

This one clip cannot produce a true frame-level recall value without frame-level ground-truth labels. Later user review confirmed that several contacts occur outside the frame and that the video was pre-trimmed, so raw trajectory coverage and long gaps must not be interpreted as TrackNet recall or as a direct model failure.

## Confirmed rally-count regression case

For `VID20260828212318_1.mp4`, user-reviewed ground truth is **4 rallies with 19, 4, 6, and 16 hits** (45 total). The provisional TrackNet-only downstream logic produced **3 rallies with 16, 4, and 18 hits** (38 total).

- Rally 1: three contacts were missed because they occurred outside the frame.
- Rally 2: hit count matched.
- Rallies 3 and 4: incorrectly merged because the landing and following serve were close in time and image coordinates.
- The merged final section also under-counted four contacts relative to the reviewed combined total of 22.

The principal failure is now classified as partial observability plus rally-state logic, not evidence that visible-frame TrackNet localization itself is unusable.

## Fast-mode long-video sample

The first five rallies from `序列 01.mp4` were processed with chunked TrackNet
`nonoverlap` inference and emitted before the rest of the source completed. User review confirmed
that all five rally time boundaries were correct. Automatic hit counts were **3, 7, 4, 6, 6**;
reviewed counts were **3, 4, 2, 4, 6**.

This establishes the product acceptance split: automatic rally segmentation is delivery-critical,
while hit counts are editable draft metadata. The software must retain the original automatic count
alongside an optional manual override and use the override as the effective displayed count.

## Engineering decisions from this run

- Keep TrackNetV3 + InpaintNet for short event windows, not whole-video first-pass scanning.
- First-pass validation must continue with a lightweight TrackNetV2/V4 checkpoint or an optimized ONNX implementation.
- Compute the background median after resizing samples to model resolution; the current reference implementation's full-resolution median creates an avoidable multi-gigabyte memory peak.
- Keep raw visibility, repaired coordinates, and `inpainted` provenance as separate fields in the product data model.
- A final P1 Go/No-Go still requires frame-level manual labels across the planned representative sample set; trajectory coverage must not be reported as recall.
