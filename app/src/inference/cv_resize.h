// Separable bilinear resize used to build the model input.
//
// Fidelity note (validated, see the change report): this does NOT reproduce
// cv::resize(INTER_LINEAR) bit-for-bit on OpenCV 5.x.  The classic two-pass
// 11-bit fixed-point path (which older OpenCV used) is off by one LSB on ~31% of
// pixels, while a separable float interpolation with a single rounding at the end
// is off by at most one LSB on ~8% of pixels - closer than OpenCV's own
// INTER_LINEAR_EXACT is to its INTER_LINEAR (9.4%).  Bit-exactness was therefore
// traded for the actual acceptance criterion: identical decoded trajectory and
// identical rally segmentation (see tools/parity_harness and the M2 report).
//
// The sampling positions, edge clamping and rounding mode do match OpenCV:
//   fx = (dst + 0.5) * (src / dst) - 0.5, clamped to [0, src - 2] with frac = 0
//   at the borders, and round-half-to-even on the final value.

#pragma once

#include <cstdint>

namespace badminton::inference {

// Resizes an interleaved 8-bit image. `src` and `dst` must not overlap.
void ResizeBilinearLikeOpenCv(const std::uint8_t *src, int src_width, int src_height,
                              int channels, std::uint8_t *dst, int dst_width, int dst_height);

}  // namespace badminton::inference
