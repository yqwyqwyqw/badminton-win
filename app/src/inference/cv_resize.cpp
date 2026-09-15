#include "cv_resize.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace badminton::inference {
namespace {

struct Axis {
    std::vector<int> offset;   // first source index per destination index
    std::vector<double> frac;  // weight of source[offset + 1]
};

// Sampling geometry identical to cv::resize: half-pixel centres, clamped to the
// valid two-tap window, with the fraction zeroed at the borders.
Axis BuildAxis(int src_size, int dst_size) {
    Axis axis;
    axis.offset.resize(dst_size);
    axis.frac.resize(dst_size);
    const double scale = static_cast<double>(src_size) / static_cast<double>(dst_size);
    for (int d = 0; d < dst_size; ++d) {
        double fx = (d + 0.5) * scale - 0.5;
        int sx = static_cast<int>(std::floor(fx));
        double fraction = fx - sx;
        if (sx < 0) {
            sx = 0;
            fraction = 0.0;
        }
        if (sx + 1 >= src_size) {
            sx = std::max(0, src_size - 2);
            fraction = 0.0;
        }
        axis.offset[d] = sx;
        axis.frac[d] = fraction;
    }
    return axis;
}

inline std::uint8_t RoundToByte(double value) {
    const double rounded = std::nearbyint(value);  // round half to even, like cvRound
    return static_cast<std::uint8_t>(std::clamp(rounded, 0.0, 255.0));
}

}  // namespace

void ResizeBilinearLikeOpenCv(const std::uint8_t *src, int src_width, int src_height,
                              int channels, std::uint8_t *dst, int dst_width, int dst_height) {
    if (src_width <= 0 || src_height <= 0 || dst_width <= 0 || dst_height <= 0 || channels <= 0) {
        return;
    }
    const Axis x = BuildAxis(src_width, dst_width);
    const Axis y = BuildAxis(src_height, dst_height);

    // Horizontal pass into float rows: no intermediate rounding, so the two
    // passes are mathematically one separable filter.
    std::vector<double> rows(static_cast<std::size_t>(src_height) * dst_width * channels);
    for (int row = 0; row < src_height; ++row) {
        const std::uint8_t *source_row = src + static_cast<std::size_t>(row) * src_width * channels;
        double *target_row = rows.data() + static_cast<std::size_t>(row) * dst_width * channels;
        for (int dx = 0; dx < dst_width; ++dx) {
            const int sx = x.offset[dx];
            const double fraction = x.frac[dx];
            const std::uint8_t *left = source_row + static_cast<std::size_t>(sx) * channels;
            const std::uint8_t *right = left + channels;
            for (int c = 0; c < channels; ++c) {
                target_row[static_cast<std::size_t>(dx) * channels + c] =
                    left[c] * (1.0 - fraction) + right[c] * fraction;
            }
        }
    }

    // Vertical pass, rounding once at the very end.
    for (int dy = 0; dy < dst_height; ++dy) {
        const int sy = y.offset[dy];
        const double fraction = y.frac[dy];
        const double *top = rows.data() + static_cast<std::size_t>(sy) * dst_width * channels;
        const double *bottom = top + static_cast<std::size_t>(dst_width) * channels;
        std::uint8_t *target = dst + static_cast<std::size_t>(dy) * dst_width * channels;
        for (int index = 0; index < dst_width * channels; ++index) {
            target[index] = RoundToByte(top[index] * (1.0 - fraction) + bottom[index] * fraction);
        }
    }
}

}  // namespace badminton::inference
