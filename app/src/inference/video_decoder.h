// Video decoding through the project's ffmpeg/ffprobe executables.
//
// Why a subprocess instead of linking libav*: the shipped FFmpeg build is GPLv3
// (it contains libx264).  Keeping it as a separate process is mere aggregation, so
// the application itself is not forced under the GPL; linking libavcodec into the
// binary would change that.  See the change report for the licensing decision.
//
// Frames are requested by absolute frame index and delivered as BGR24, matching
// the pixel layout cv2.VideoCapture produced in the reference implementation.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace badminton::inference {

struct VideoMetadata {
    int width = 0;
    int height = 0;
    double fps = 0.0;
    std::int64_t frame_count = 0;
};

// Uses ffprobe.exe next to `ffmpeg_path`.
bool ProbeVideo(const std::string &ffmpeg_path, const std::string &video_path,
                VideoMetadata &metadata, std::string &error);

// Runs an executable with the given arguments and waits for it.  Used for the
// rally preview clips, which keep the same ffmpeg invocation as the reference
// implementation.  Output is discarded; `error` receives ffmpeg's stderr on
// failure.
bool RunCommand(const std::string &executable, const std::vector<std::string> &arguments,
                std::string &error);

// Decodes `count` frames starting at `start_frame` into `out` (count * width *
// height * 3 bytes, BGR24).  `fps` converts the frame index into the seek
// timestamp; ffmpeg resolves input seeking to the requested frame for
// constant-frame-rate sources.
bool ReadFramesBgr24(const std::string &ffmpeg_path, const std::string &video_path,
                     std::int64_t start_frame, std::int64_t count, int width, int height,
                     double fps, std::vector<std::uint8_t> &out, std::string &error);

}  // namespace badminton::inference
