#include "windowing.h"

#include <algorithm>

namespace badminton::inference {

std::vector<Window> BuildWindows(const std::vector<std::int64_t> &frame_ids, int seq_len, int step,
                                 bool pad) {
    std::vector<Window> windows;
    const int total = static_cast<int>(frame_ids.size());
    if (seq_len <= 0 || step <= 0 || total == 0) {
        return windows;
    }
    for (int start = 0; start < total; start += step) {
        const int end = std::min(start + seq_len, total);
        const int valid_length = end - start;
        if (valid_length < seq_len && !pad) {
            break;
        }
        Window window;
        window.valid_length = valid_length;
        window.frame_indices.reserve(seq_len);
        window.frame_ids.reserve(seq_len);
        for (int position = start; position < end; ++position) {
            window.frame_indices.push_back(position);
            window.frame_ids.push_back(frame_ids[static_cast<std::size_t>(position)]);
        }
        // Padding repeats the last real frame exactly like the reference.
        for (int position = valid_length; position < seq_len; ++position) {
            window.frame_indices.push_back(window.frame_indices.back());
            window.frame_ids.push_back(window.frame_ids.back());
        }
        windows.push_back(std::move(window));
        if (end == total) {
            break;
        }
    }
    return windows;
}

}  // namespace badminton::inference
