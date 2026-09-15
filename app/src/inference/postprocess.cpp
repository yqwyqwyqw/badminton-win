#include "postprocess.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace badminton::inference {
namespace {

struct Box {
    int min_x = std::numeric_limits<int>::max();
    int min_y = std::numeric_limits<int>::max();
    int max_x = -1;
    int max_y = -1;
    long pixels = 0;
};

}  // namespace

DecodedPoint DecodeHeatmap(const float *heatmap, double proxy_width, double proxy_height,
                           float threshold) {
    const int width = kModelWidth;
    const int height = kModelHeight;
    const std::size_t count = static_cast<std::size_t>(width) * height;

    std::vector<std::uint8_t> binary(count, 0);
    for (std::size_t index = 0; index < count; ++index) {
        binary[index] = heatmap[index] > threshold ? 1 : 0;
    }

    // 8-connected components, iterative flood fill (no recursion: 288x512 blobs
    // would blow the stack).
    std::vector<std::uint8_t> visited(count, 0);
    std::vector<int> stack;
    Box best;
    for (int start = 0; start < static_cast<int>(count); ++start) {
        if (binary[start] == 0 || visited[start] != 0) {
            continue;
        }
        Box box;
        visited[start] = 1;
        stack.clear();
        stack.push_back(start);
        while (!stack.empty()) {
            const int point = stack.back();
            stack.pop_back();
            const int px = point % width;
            const int py = point / width;
            box.min_x = std::min(box.min_x, px);
            box.min_y = std::min(box.min_y, py);
            box.max_x = std::max(box.max_x, px);
            box.max_y = std::max(box.max_y, py);
            ++box.pixels;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) {
                        continue;
                    }
                    const int nx = px + dx;
                    const int ny = py + dy;
                    if (nx < 0 || nx >= width || ny < 0 || ny >= height) {
                        continue;
                    }
                    const int neighbour = ny * width + nx;
                    if (binary[neighbour] != 0 && visited[neighbour] == 0) {
                        visited[neighbour] = 1;
                        stack.push_back(neighbour);
                    }
                }
            }
        }
        if (box.pixels > best.pixels) {
            best = box;
        }
    }

    DecodedPoint result;
    if (best.max_x < 0) {
        return result;  // nothing above the threshold -> invisible at (0,0)
    }
    const double box_width = best.max_x - best.min_x + 1;   // cv::boundingRect semantics
    const double box_height = best.max_y - best.min_y + 1;
    result.x = (best.min_x + box_width / 2.0) * proxy_width / kModelWidth;
    result.y = (best.min_y + box_height / 2.0) * proxy_height / kModelHeight;
    result.visible = (result.x != 0.0) || (result.y != 0.0);
    return result;
}

}  // namespace badminton::inference
