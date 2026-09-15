// Heatmap decoding, mirroring
// tracknet/inference/pipeline.py: InferencePipeline._decode_heatmap.
//
//   binary = heatmap > 0.5
//   largest external contour -> boundingRect -> centre (x + w/2, y + h/2)
//   scale to proxy resolution, (0,0) when nothing is above the threshold
//
// OpenCV selects the contour with the largest `contourArea` (a polygon area) and
// reports `w = max_x - min_x + 1`. This implementation uses 8-connected
// components ranked by pixel count, which agrees on every fixture frame; the
// full-clip comparison is what guards that assumption.

#pragma once

#include <cstdint>

#include "tracknet_types.h"

namespace badminton::inference {

// `heatmap` is 288x512 row-major. Coordinates are returned in the coordinate
// space of `proxy_width` x `proxy_height`.
DecodedPoint DecodeHeatmap(const float *heatmap, double proxy_width, double proxy_height,
                           float threshold = kHeatmapThreshold);

}  // namespace badminton::inference
