#include "preprocess.h"

#include <cstring>

#include "cv_resize.h"
#include "tracknet_types.h"

namespace badminton::inference {
namespace {

// BGR -> RGB into a freshly allocated buffer sized for the model input.
void BgrToRgbResized(const std::uint8_t *bgr, int width, int height, std::uint8_t *rgb_small) {
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    std::vector<std::uint8_t> rgb(pixels * kRgbChannels);
    for (std::size_t index = 0; index < pixels; ++index) {
        rgb[index * 3 + 0] = bgr[index * 3 + 2];
        rgb[index * 3 + 1] = bgr[index * 3 + 1];
        rgb[index * 3 + 2] = bgr[index * 3 + 0];
    }
    ResizeBilinearLikeOpenCv(rgb.data(), width, height, kRgbChannels, rgb_small, kModelWidth,
                             kModelHeight);
}

// Converts one resized HWC plane into the /255 float channel block.
void AppendChannels(const std::uint8_t *plane, float *destination) {
    const std::size_t pixels = static_cast<std::size_t>(kModelWidth) * kModelHeight;
    for (int channel = 0; channel < kRgbChannels; ++channel) {
        for (std::size_t index = 0; index < pixels; ++index) {
            destination[index] = static_cast<float>(plane[index * kRgbChannels + channel]) / 255.0f;
        }
        destination += pixels;
    }
}

}  // namespace

std::vector<float> BuildModelInput(const std::uint8_t *const *frames, int frame_count,
                                   const std::uint8_t *background_bgr, int frame_width,
                                   int frame_height) {
    const std::size_t pixels = static_cast<std::size_t>(kModelWidth) * kModelHeight;
    const int planes = frame_count + (background_bgr != nullptr ? 1 : 0);
    std::vector<float> input(static_cast<std::size_t>(planes) * kRgbChannels * pixels);

    std::vector<std::uint8_t> resized(pixels * kRgbChannels);
    std::size_t offset = 0;

    if (background_bgr != nullptr) {
        BgrToRgbResized(background_bgr, frame_width, frame_height, resized.data());
        AppendChannels(resized.data(), input.data() + offset);
        offset += pixels * kRgbChannels;
    }
    for (int index = 0; index < frame_count; ++index) {
        BgrToRgbResized(frames[index], frame_width, frame_height, resized.data());
        AppendChannels(resized.data(), input.data() + offset);
        offset += pixels * kRgbChannels;
    }
    return input;
}

}  // namespace badminton::inference
