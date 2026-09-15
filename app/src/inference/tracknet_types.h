// Shared types and constants for the C++ inference kernel.
//
// These values mirror the Python reference implementation
// (tracknet/data/io.py, tracknet/inference/pipeline.py) exactly; changing them
// silently changes the trajectory and invalidates the golden parity fixtures.

#pragma once

#include <cstdint>
#include <vector>

namespace badminton::inference {

// Model input/output geometry (TrackNet, 512x288, seq_len 8 with a concatenated
// background frame as channels 0..2).
inline constexpr int kModelWidth = 512;
inline constexpr int kModelHeight = 288;
inline constexpr int kRgbChannels = 3;

// Heatmap binarisation threshold (pipeline.py: threshold=0.5).
inline constexpr float kHeatmapThreshold = 0.5f;

// One decoded trajectory sample, in proxy-video pixel coordinates.
struct TrajectoryPoint {
    std::int64_t frame = 0;
    double x = 0.0;
    double y = 0.0;
    bool visible = false;
};

using Trajectory = std::vector<TrajectoryPoint>;

// Result of decoding a single heatmap.
struct DecodedPoint {
    double x = 0.0;
    double y = 0.0;
    bool visible = false;
};

}  // namespace badminton::inference
