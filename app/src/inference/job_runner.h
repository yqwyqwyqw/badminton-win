// Chunked analysis job: background median, windowed inference, trajectory
// assembly and rally segmentation.
//
// Mirrors validation/run_chunked_tracknet.py so the product keeps producing the
// same trajectory for the same proxy:
//   * chunk grid: core = round(chunk_seconds * fps), overlap = round(0.5 s)
//   * non-overlapping inference on the global seq_len grid
//     (read_start is aligned down to a multiple of seq_len)
//   * only the frames inside the core range are kept from each chunk
//   * background median sampled with np.linspace semantics and cached per output
//     directory
// Everything is expressed with an in-memory progress callback; file contracts
// (progress.json / rally-feed.json) are written by the same runner so the UI keeps
// working unchanged.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "rally_state_machine.h"
#include "tracknet_engine.h"
#include "video_decoder.h"

namespace badminton::inference {

struct AnalysisOptions {
    std::string ffmpeg_path;   // .../bin/ffmpeg.exe (ffprobe.exe is expected next to it)
    std::string model_path;    // ONNX model
    std::string video_path;    // the 720p proxy the model analyses
    std::string output_dir;    // <output>/tracknet in the product layout
    int batch_size = 8;
    double chunk_seconds = 6.0;
    double overlap_seconds = 0.5;
    int median_samples = 41;
    bool use_directml = true;
    int intra_op_threads = 8;
    std::int64_t max_frames = 0;   // 0 = whole video
    bool write_chunk_files = true; // chunks/*.csv + trajectory CSV (resume contract)
    bool warmup = true;
    // Set to publish rallies after each chunk (the UI depends on this).
    std::function<void(const Trajectory &, const VideoMetadata &, bool final)> on_trajectory;
};

struct AnalysisProgress {
    std::string status = "running";  // running | complete | paused
    int chunk_index = 0;
    int chunk_count = 0;
    std::int64_t frames_completed = 0;
    std::int64_t frames_target = 0;
    std::int64_t frames_in_video = 0;
    double percent = 0.0;
    double fps = 0.0;
    double inference_fps = 0.0;
    double eta_seconds = 0.0;
};

struct AnalysisResult {
    VideoMetadata metadata;
    Trajectory trajectory;
    std::vector<Rally> rallies;
    std::vector<std::uint8_t> median;  // height * width * 3, BGR
    double inference_seconds = 0.0;
    int chunks_completed = 0;
};

using ProgressCallback = std::function<void(const AnalysisProgress &)>;
using CancelCallback = std::function<bool()>;

// Called after every completed chunk with the trajectory accumulated so far
// (`final` is true once the whole analysed range is done).  The product uses this
// to publish rallies incrementally: the UI shows a rally as soon as it closes,
// matching the reference implementation's behaviour.
using TrajectoryCallback =
    std::function<void(const Trajectory &, const VideoMetadata &, bool final)>;

// Returns false and fills `error` when the analysis cannot run (missing ffmpeg,
// DirectML unavailable, decode failure...).  On cancellation it returns true with
// `result.trajectory` holding the chunks completed so far.
bool RunAnalysis(const AnalysisOptions &options, const ProgressCallback &on_progress,
                 const CancelCallback &should_cancel, AnalysisResult &result, std::string &error);

// Layout helpers shared with the product controller.
// The returned path is a native path: build it from a UTF-8 directory with
// Utf8Path() semantics, never by converting the directory through std::string.
std::filesystem::path ChunkCsvPath(const std::string &output_dir, int chunk_index);
std::filesystem::path TrajectoryCsvPath(const std::string &output_dir);

}  // namespace badminton::inference
