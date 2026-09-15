// Window enumeration, mirroring tracknet/data/video.py: iter_windows.
//
//   for start in range(0, len(ids), step):
//       end = min(start + seq_len, len(ids)); valid_length = end - start
//       if valid_length < seq_len and not pad: break
//       pad by repeating the LAST frame (ids and pixels)
//
// Windows reference the caller's frame buffer by index, so a window is cheap and
// no pixels are copied.

#pragma once

#include <cstdint>
#include <vector>

namespace badminton::inference {

struct Window {
    std::vector<int> frame_indices;   // index into the frame buffer, padded by repeating the last
    std::vector<std::int64_t> frame_ids;
    int valid_length = 0;             // number of positions that are real (not padding)
};

std::vector<Window> BuildWindows(const std::vector<std::int64_t> &frame_ids, int seq_len, int step,
                                 bool pad);

}  // namespace badminton::inference
