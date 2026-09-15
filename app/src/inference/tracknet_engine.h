// ONNX Runtime session wrapper plus the window/ensemble logic of
// tracknet/inference/pipeline.py (InferencePipeline._predict_windows and
// ensemble_predictions).
//
// Decoding a whole chunk into a trajectory is the unit the job runner consumes;
// this class deliberately owns no file I/O so it stays testable against the
// frozen golden fixtures.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "tracknet_types.h"

namespace badminton::inference {

class TrackNetEngine {
public:
    enum class Provider { kDirectML, kCpu };

    struct Options {
        std::string model_path;
        Provider provider = Provider::kDirectML;
        int intra_op_threads = 8;
        bool warmup = true;
    };

    // Throws std::runtime_error when the model cannot be loaded or the requested
    // execution provider is unavailable (the product fails fast by design: there
    // is no CPU slow path in the UI).
    explicit TrackNetEngine(const Options &options);
    ~TrackNetEngine();

    TrackNetEngine(const TrackNetEngine &) = delete;
    TrackNetEngine &operator=(const TrackNetEngine &) = delete;

    int sequence_length() const;
    bool uses_background() const;
    const std::string &input_name() const;
    const std::string &output_name() const;

    // Runs one batch of windows. `frames[i]` is the frame buffer of window i
    // (`sequence_length` pointers to BGR HxWx3 frames). Heatmaps are appended as
    // window-major (window, position, 288, 512) floats.
    void PredictWindows(const std::vector<const std::uint8_t *const *> &window_frames, int batch_size,
                        const std::uint8_t *background_bgr, int frame_width, int frame_height,
                        std::vector<float> &heatmaps);

    // Full chunk -> trajectory, mirroring predict_frames_with_global_median:
    // windowing, per-frame weighted accumulation and heatmap decoding.
    Trajectory PredictChunk(const std::uint8_t *frames, int frame_count,
                            std::int64_t first_frame_id, const std::uint8_t *background_bgr,
                            int frame_width, int frame_height, double fps, int batch_size);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace badminton::inference
