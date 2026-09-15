#include "rally_state_machine.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <vector>

namespace badminton::inference {
namespace {

// ---------------------------------------------------------------- utilities

int OddFrames(double seconds, double fps, int minimum = 3) {
    int value = std::max(minimum, static_cast<int>(std::lround(seconds * fps)));
    if (value % 2 == 0) {
        ++value;
    }
    return value;
}

// analysis.py: smooth() - edge-padded box filter, with the same guards.
std::vector<double> Smooth(const std::vector<double> &values, int window) {
    const int count = static_cast<int>(values.size());
    if (count < 3 || window <= 1) {
        return values;
    }
    int effective = std::min(window, count % 2 ? count : count - 1);
    if (effective < 3) {
        return values;
    }
    const int pad = effective / 2;
    std::vector<double> padded(static_cast<std::size_t>(count) + 2 * pad);
    for (int index = 0; index < static_cast<int>(padded.size()); ++index) {
        const int source = std::min(std::max(index - pad, 0), count - 1);
        padded[static_cast<std::size_t>(index)] = values[static_cast<std::size_t>(source)];
    }
    std::vector<double> result(static_cast<std::size_t>(count));
    const double weight = 1.0 / static_cast<double>(effective);
    for (int index = 0; index < count; ++index) {
        double sum = 0.0;
        for (int offset = 0; offset < effective; ++offset) {
            sum += padded[static_cast<std::size_t>(index + offset)];
        }
        result[static_cast<std::size_t>(index)] = sum * weight;
    }
    return result;
}

// numpy.median: average of the two middle values for an even count.
double Median(std::vector<double> values) {
    if (values.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    std::sort(values.begin(), values.end());
    const std::size_t count = values.size();
    if (count % 2 == 1) {
        return values[count / 2];
    }
    return (values[count / 2 - 1] + values[count / 2]) / 2.0;
}

// pandas .interpolate(limit_direction="both") on a linear index: interior gaps
// interpolate linearly, leading/trailing gaps take the nearest valid value.
std::vector<double> InterpolateBoth(const std::vector<double> &values,
                                    const std::vector<char> &valid) {
    const int count = static_cast<int>(values.size());
    std::vector<double> result(static_cast<std::size_t>(count),
                               std::numeric_limits<double>::quiet_NaN());
    std::vector<int> known;
    for (int index = 0; index < count; ++index) {
        if (valid[static_cast<std::size_t>(index)] != 0) {
            known.push_back(index);
            result[static_cast<std::size_t>(index)] = values[static_cast<std::size_t>(index)];
        }
    }
    if (known.empty()) {
        return result;
    }
    for (int index = 0; index < known.front(); ++index) {
        result[static_cast<std::size_t>(index)] = values[static_cast<std::size_t>(known.front())];
    }
    for (int index = known.back() + 1; index < count; ++index) {
        result[static_cast<std::size_t>(index)] = values[static_cast<std::size_t>(known.back())];
    }
    for (std::size_t pair = 0; pair + 1 < known.size(); ++pair) {
        const int left = known[pair];
        const int right = known[pair + 1];
        if (right <= left + 1) {
            continue;
        }
        const double left_value = values[static_cast<std::size_t>(left)];
        const double right_value = values[static_cast<std::size_t>(right)];
        const double step = (right_value - left_value) / static_cast<double>(right - left);
        for (int index = left + 1; index < right; ++index) {
            result[static_cast<std::size_t>(index)] =
                left_value + step * static_cast<double>(index - left);
        }
    }
    return result;
}

// analysis.py: axis_extrema()
std::vector<std::pair<int, double>> AxisExtrema(const std::vector<double> &values, int window,
                                                double prominence) {
    std::vector<std::pair<int, double>> found;
    const int count = static_cast<int>(values.size());
    for (double sign : {1.0, -1.0}) {
        std::vector<double> signal(static_cast<std::size_t>(count));
        for (int index = 0; index < count; ++index) {
            signal[static_cast<std::size_t>(index)] = values[static_cast<std::size_t>(index)] * sign;
        }
        for (int index = window; index < count - window; ++index) {
            double before_max = -std::numeric_limits<double>::infinity();
            double before_min = std::numeric_limits<double>::infinity();
            for (int offset = index - window; offset < index; ++offset) {
                before_max = std::max(before_max, signal[static_cast<std::size_t>(offset)]);
                before_min = std::min(before_min, signal[static_cast<std::size_t>(offset)]);
            }
            double after_max = -std::numeric_limits<double>::infinity();
            double after_min = std::numeric_limits<double>::infinity();
            for (int offset = index + 1; offset <= index + window; ++offset) {
                after_max = std::max(after_max, signal[static_cast<std::size_t>(offset)]);
                after_min = std::min(after_min, signal[static_cast<std::size_t>(offset)]);
            }
            if (signal[static_cast<std::size_t>(index)] < before_max ||
                signal[static_cast<std::size_t>(index)] <= after_max) {
                continue;
            }
            const double strength =
                std::min(signal[static_cast<std::size_t>(index)] - before_min,
                         signal[static_cast<std::size_t>(index)] - after_min);
            if (strength >= prominence) {
                found.emplace_back(index, strength);
            }
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

struct Gap {
    int left = 0;       // last visible row index before the gap
    int right = 0;      // first visible row index after the gap
    int first_gap = 0;  // left + 1
    int last_gap = 0;   // right - 1
};

std::vector<Gap> InvisibleGaps(const std::vector<char> &visible) {
    std::vector<int> positions;
    for (int index = 0; index < static_cast<int>(visible.size()); ++index) {
        if (visible[static_cast<std::size_t>(index)] != 0) {
            positions.push_back(index);
        }
    }
    std::vector<Gap> gaps;
    for (std::size_t index = 0; index + 1 < positions.size(); ++index) {
        const int left = positions[index];
        const int right = positions[index + 1];
        if (right > left + 1) {
            gaps.push_back(Gap{left, right, left + 1, right - 1});
        }
    }
    return gaps;
}

// analysis.py: visible_speed() over the given row positions.
std::vector<double> VisibleSpeed(const Trajectory &data, const std::vector<int> &positions) {
    std::vector<double> speeds;
    if (positions.size() < 2) {
        return speeds;
    }
    for (std::size_t index = 0; index + 1 < positions.size(); ++index) {
        const TrajectoryPoint &left = data[static_cast<std::size_t>(positions[index])];
        const TrajectoryPoint &right = data[static_cast<std::size_t>(positions[index + 1])];
        const double delta = std::max(1.0, static_cast<double>(right.frame - left.frame));
        speeds.push_back(std::hypot(right.x - left.x, right.y - left.y) / delta);
    }
    return speeds;
}

struct Range {
    int start = 0;
    int end = 0;
};

std::vector<Range> RallyRanges(const Trajectory &data, double fps, int width, int height,
                               const RallyParameters &parameters) {
    const int count = static_cast<int>(data.size());
    std::vector<char> visible(static_cast<std::size_t>(count));
    std::vector<int> positions;
    for (int index = 0; index < count; ++index) {
        visible[static_cast<std::size_t>(index)] = data[static_cast<std::size_t>(index)].visible ? 1 : 0;
        if (data[static_cast<std::size_t>(index)].visible) {
            positions.push_back(index);
        }
    }
    if (positions.empty()) {
        return {};
    }
    const int minimum_gap = std::max(1, static_cast<int>(std::lround(parameters.split_gap_seconds * fps)));
    const int restart_gap = std::max(2, static_cast<int>(std::lround(0.10 * fps)));
    const double diagonal = std::hypot(static_cast<double>(width), static_cast<double>(height));

    std::vector<int> split_after;
    for (const Gap &gap : InvisibleGaps(visible)) {
        const int gap_size = gap.last_gap - gap.first_gap + 1;
        if (gap_size < restart_gap) {
            continue;
        }
        const double left_y = data[static_cast<std::size_t>(gap.left)].y / static_cast<double>(height);
        const double right_y = data[static_cast<std::size_t>(gap.right)].y / static_cast<double>(height);

        // A disappearance at the top is normally an out-of-frame clear and must
        // not end the rally (analysis.py comment, ported verbatim).
        const bool interior_restart = gap_size >= minimum_gap &&
                                      left_y >= parameters.top_edge_fraction &&
                                      right_y >= parameters.top_edge_fraction;
        const bool very_long_gap = gap_size >= static_cast<int>(std::lround(3.0 * fps));

        std::vector<int> before_positions;
        for (int candidate : positions) {
            if (candidate <= gap.left && candidate >= gap.left - static_cast<int>(0.30 * fps)) {
                before_positions.push_back(candidate);
            }
        }
        std::vector<int> after_positions;
        for (int candidate : positions) {
            if (candidate >= gap.right && candidate <= gap.right + static_cast<int>(0.45 * fps)) {
                after_positions.push_back(candidate);
            }
        }
        const std::vector<double> before_speed = VisibleSpeed(data, before_positions);
        const std::vector<double> after_speed = VisibleSpeed(data, after_positions);
        const double reset_distance =
            std::hypot(data[static_cast<std::size_t>(gap.right)].x - data[static_cast<std::size_t>(gap.left)].x,
                       data[static_cast<std::size_t>(gap.right)].y - data[static_cast<std::size_t>(gap.left)].y);

        bool low_speed_restart = false;
        if (left_y >= parameters.top_edge_fraction && right_y >= parameters.top_edge_fraction &&
            before_speed.size() >= 2 && after_speed.size() >= 2) {
            std::vector<double> tail(before_speed.end() - std::min<std::size_t>(3, before_speed.size()),
                                     before_speed.end());
            const double before_median = Median(tail);
            double after_max = -std::numeric_limits<double>::infinity();
            for (double value : after_speed) {
                after_max = std::max(after_max, value);
            }
            low_speed_restart = before_median <= 0.004 * diagonal &&
                                after_max >= 0.01 * diagonal &&
                                reset_distance >= 0.03 * diagonal;
        }
        if (interior_restart || very_long_gap || low_speed_restart) {
            split_after.push_back(gap.left);
        }
    }

    std::vector<Range> ranges;
    int start = positions.front();
    for (int left : split_after) {
        const int end = left;
        if (end >= start) {
            ranges.push_back(Range{start, end});
        }
        for (int candidate : positions) {
            if (candidate > left) {
                start = candidate;
                break;
            }
        }
    }
    const int end = positions.back();
    if (end >= start) {
        ranges.push_back(Range{start, end});
    }
    return ranges;
}

std::vector<HitEvent> HitCandidates(const Trajectory &segment, double fps, int width, int height,
                                    const RallyParameters &parameters) {
    const int count = static_cast<int>(segment.size());
    std::vector<double> x_raw(static_cast<std::size_t>(count));
    std::vector<double> y_raw(static_cast<std::size_t>(count));
    std::vector<char> valid(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        const TrajectoryPoint &point = segment[static_cast<std::size_t>(index)];
        x_raw[static_cast<std::size_t>(index)] = point.x;
        y_raw[static_cast<std::size_t>(index)] = point.y;
        valid[static_cast<std::size_t>(index)] = point.visible ? 1 : 0;
    }
    const std::vector<double> x_filled = InterpolateBoth(x_raw, valid);
    const std::vector<double> y_filled = InterpolateBoth(y_raw, valid);
    for (int index = 0; index < count; ++index) {
        if (std::isnan(x_filled[static_cast<std::size_t>(index)]) ||
            std::isnan(y_filled[static_cast<std::size_t>(index)])) {
            return {};
        }
    }

    const int smooth_window = OddFrames(parameters.smooth_seconds, fps);
    const int turn_window = std::max(2, static_cast<int>(std::lround(parameters.turn_window_seconds * fps)));
    const double diagonal = std::hypot(static_cast<double>(width), static_cast<double>(height));
    const double prominence = parameters.min_turn_diagonal_fraction * diagonal;

    const std::vector<double> x = Smooth(x_filled, smooth_window);
    const std::vector<double> y = Smooth(y_filled, smooth_window);
    std::vector<std::pair<int, double>> raw = AxisExtrema(x, turn_window, prominence);
    const std::vector<std::pair<int, double>> y_extrema = AxisExtrema(y, turn_window, prominence);
    raw.insert(raw.end(), y_extrema.begin(), y_extrema.end());
    std::sort(raw.begin(), raw.end());

    const int merge_frames = std::max(1, static_cast<int>(std::lround(parameters.merge_seconds * fps)));
    std::vector<std::pair<int, double>> merged;
    for (const auto &item : raw) {
        if (merged.empty() || item.first - merged.back().first >= merge_frames) {
            merged.push_back(item);
        } else if (item.second > merged.back().second) {
            merged.back() = item;
        }
    }

    int first_visible = 0;
    while (first_visible < count && valid[static_cast<std::size_t>(first_visible)] == 0) {
        ++first_visible;
    }
    struct Event {
        int position;
        double strength;
        std::string rule;
    };
    std::vector<Event> events;
    events.push_back(Event{first_visible, prominence, "serve_or_open_start"});
    for (const auto &item : merged) {
        events.push_back(Event{item.first, item.second, "trajectory_turn"});
    }
    std::sort(events.begin(), events.end(), [](const Event &a, const Event &b) {
        if (a.position != b.position) {
            return a.position < b.position;
        }
        if (a.strength != b.strength) {
            return a.strength < b.strength;
        }
        return a.rule < b.rule;
    });
    std::vector<Event> deduplicated;
    for (const Event &event : events) {
        if (deduplicated.empty() || event.position - deduplicated.back().position >= merge_frames) {
            deduplicated.push_back(event);
        } else if (event.strength > deduplicated.back().strength) {
            deduplicated.back() = event;
        }
    }

    std::vector<HitEvent> hits;
    for (const Event &event : deduplicated) {
        const TrajectoryPoint &point = segment[static_cast<std::size_t>(event.position)];
        HitEvent hit;
        hit.frame = static_cast<int>(point.frame);
        hit.seconds = static_cast<double>(point.frame) / fps;
        hit.x = x_filled[static_cast<std::size_t>(event.position)];
        hit.y = y_filled[static_cast<std::size_t>(event.position)];
        hit.observed = point.visible;
        hit.rule = event.rule;
        hit.confidence = std::min(0.95, 0.55 + event.strength / diagonal);
        hits.push_back(hit);
    }
    return hits;
}

}  // namespace

std::vector<Rally> AnalyseRallies(const Trajectory &trajectory, double fps, int width, int height,
                                  const RallyParameters &parameters) {
    std::vector<Rally> rallies;
    if (trajectory.empty() || fps <= 0.0) {
        return rallies;
    }
    for (const Range &range : RallyRanges(trajectory, fps, width, height, parameters)) {
        Trajectory segment(trajectory.begin() + range.start, trajectory.begin() + range.end + 1);
        std::vector<HitEvent> hits = HitCandidates(segment, fps, width, height, parameters);
        const double duration =
            (static_cast<double>(segment.back().frame) - static_cast<double>(segment.front().frame)) / fps;
        if (static_cast<int>(hits.size()) < parameters.min_rally_hits ||
            duration < parameters.min_rally_seconds) {
            continue;
        }
        Rally rally;
        rally.index = static_cast<int>(rallies.size()) + 1;
        rally.start_frame = static_cast<int>(segment.front().frame);
        rally.end_frame = static_cast<int>(segment.back().frame);
        rally.start_seconds = static_cast<double>(segment.front().frame) / fps;
        rally.end_seconds = static_cast<double>(segment.back().frame) / fps;
        for (const HitEvent &hit : hits) {
            if (hit.observed) {
                ++rally.observed_hits;
            } else {
                ++rally.inferred_gap_hits;
            }
        }
        rally.hits = std::move(hits);
        rallies.push_back(std::move(rally));
    }
    return rallies;
}

}  // namespace badminton::inference
