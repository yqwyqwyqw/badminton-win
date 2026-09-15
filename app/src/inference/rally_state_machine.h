// Rally segmentation and hit counting, ported 1:1 from
// validation/analyze_tracknet_rallies.py (the provisional heuristic the product
// ships today).  Constants, smoothing, merging and the out-of-frame rules are
// copied deliberately: this layer decides the rally boundaries, which is the
// delivery-critical output, so it must not "improve" while being ported.
//
// Out-of-frame handling lives here (see the plan's D2 note):
//   * a disappearance near the top edge is treated as an out-of-frame clear and
//     does NOT end the rally;
//   * an interior disappearance that restarts (>= split-gap, both endpoints below
//     the top band), or any gap >= 3.0 s, or a low-speed restart, ends the rally.

#pragma once

#include <string>
#include <vector>

#include "tracknet_types.h"

namespace badminton::inference {

struct RallyParameters {
    double split_gap_seconds = 0.35;
    double top_edge_fraction = 0.22;
    int min_rally_hits = 2;
    double min_rally_seconds = 0.8;
    double smooth_seconds = 0.22;
    double turn_window_seconds = 0.24;
    double merge_seconds = 0.32;
    double min_turn_diagonal_fraction = 0.02;
};

struct HitEvent {
    int frame = 0;
    double seconds = 0.0;
    double x = 0.0;
    double y = 0.0;
    bool observed = false;
    std::string rule;
    double confidence = 0.0;
};

struct Rally {
    int index = 0;
    int start_frame = 0;
    int end_frame = 0;
    double start_seconds = 0.0;
    double end_seconds = 0.0;
    int observed_hits = 0;
    int inferred_gap_hits = 0;
    std::vector<HitEvent> hits;
};

// `trajectory` must be sorted by frame (the engine guarantees this).
std::vector<Rally> AnalyseRallies(const Trajectory &trajectory, double fps, int width, int height,
                                  const RallyParameters &parameters = RallyParameters{});

}  // namespace badminton::inference
