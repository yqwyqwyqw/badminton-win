// Model input construction, mirroring
// tracknet/inference/pipeline.py: InferencePipeline._prepare_frames.
//
// Order matters and is validated against a frozen PyTorch tensor:
//   1. BGR -> RGB per pixel (frames[..., ::-1])
//   2. cv2.resize(INTER_LINEAR) to 512x288 (see cv_resize.h)
//   3. HWC -> CHW per frame
//   4. with bg_mode == "concat": the resized background frame goes in FRONT
//   5. concatenate all frames, then divide by 255 (float32)
// There is no mean/std normalisation anywhere in the reference implementation.

#pragma once

#include <cstdint>
#include <vector>

namespace badminton::inference {

// `frames` holds `frame_count` pointers to BGR (H,W,3) 8-bit frames, in window
// order. `background_bgr` is the global background median (BGR, H,W,3) or null
// when the model does not take a background channel.
//
// Returns (frame_count [+1]) * 3 * 288 * 512 floats, channels-first.
std::vector<float> BuildModelInput(const std::uint8_t *const *frames, int frame_count,
                                   const std::uint8_t *background_bgr, int frame_width,
                                   int frame_height);

}  // namespace badminton::inference
