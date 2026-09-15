#include "tracknet_engine.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "path_utf8.h"
#include "postprocess.h"
#include "preprocess.h"
#include "windowing.h"

namespace badminton::inference {
namespace {

std::string Narrow(const char *text) {
    return text != nullptr ? std::string(text) : std::string();
}

}  // namespace

struct TrackNetEngine::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "badminton-tracknet"};
    Ort::Session session{nullptr};
    std::string input_name;
    std::string output_name;
    int sequence_length = 8;
    bool uses_background = true;
    int channels = 27;
    int intra_op_threads = 8;
    Provider provider = Provider::kDirectML;
    std::vector<float> batch_input;   // reused across calls
    std::vector<float> batch_output;
};

TrackNetEngine::TrackNetEngine(const Options &options) : impl_(std::make_unique<Impl>()) {
    if (options.model_path.empty()) {
        throw std::runtime_error("TrackNetEngine: model path is empty");
    }
    impl_->provider = options.provider;
    impl_->intra_op_threads = options.intra_op_threads;

    Ort::SessionOptions session_options;
    session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    session_options.SetIntraOpNumThreads(std::max(1, options.intra_op_threads));
    if (options.provider == Provider::kDirectML) {
        try {
            session_options.AppendExecutionProvider("DML", {});
        } catch (const Ort::Exception &error) {
            throw std::runtime_error(
                std::string("DirectML execution provider is unavailable: ") + error.what() +
                " - this build requires a DirectX 12 GPU with a recent driver");
        }
    }
    // CPU needs no registration: it is the implicit fallback provider.

    try {
        // The model may live next to the executable, so the path can be non-ASCII.
        const std::wstring model_path = Utf8ToWide(options.model_path);
        impl_->session = Ort::Session(impl_->env, model_path.c_str(), session_options);
    } catch (const Ort::Exception &error) {
        throw std::runtime_error(std::string("cannot load ONNX model: ") + error.what());
    }

    Ort::AllocatorWithDefaultOptions allocator;
    impl_->input_name = Narrow(impl_->session.GetInputNameAllocated(0, allocator).get());
    impl_->output_name = Narrow(impl_->session.GetOutputNameAllocated(0, allocator).get());

    const auto input_shape = impl_->session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    if (input_shape.size() != 4 || input_shape[1] <= 0) {
        throw std::runtime_error("unexpected model input shape (expected N x C x 288 x 512)");
    }
    impl_->channels = static_cast<int>(input_shape[1]);
    if (impl_->channels % kRgbChannels != 0) {
        throw std::runtime_error("model input channels are not a multiple of 3");
    }
    const int planes = impl_->channels / kRgbChannels;
    // The concatenated-background layout is (seq_len + 1) planes, the plain one
    // is seq_len planes; models.json records which one the export produced.
    impl_->uses_background = true;
    impl_->sequence_length = planes - 1;
}

TrackNetEngine::~TrackNetEngine() = default;

int TrackNetEngine::sequence_length() const { return impl_->sequence_length; }
bool TrackNetEngine::uses_background() const { return impl_->uses_background; }
const std::string &TrackNetEngine::input_name() const { return impl_->input_name; }
const std::string &TrackNetEngine::output_name() const { return impl_->output_name; }

void TrackNetEngine::PredictWindows(const std::vector<const std::uint8_t *const *> &window_frames,
                                    int batch_size, const std::uint8_t *background_bgr,
                                    int frame_width, int frame_height,
                                    std::vector<float> &heatmaps) {
    const int count = static_cast<int>(window_frames.size());
    if (count == 0) {
        return;
    }
    if (count > batch_size) {
        throw std::runtime_error("PredictWindows: batch larger than the requested batch size");
    }
    const std::size_t pixels = static_cast<std::size_t>(kModelWidth) * kModelHeight;
    const std::size_t per_window_in = static_cast<std::size_t>(impl_->channels) * pixels;
    const std::size_t per_window_out = static_cast<std::size_t>(impl_->sequence_length) * pixels;

    impl_->batch_input.assign(static_cast<std::size_t>(count) * per_window_in, 0.0f);
    for (int window = 0; window < count; ++window) {
        std::vector<float> input = BuildModelInput(window_frames[window], impl_->sequence_length,
                                                   background_bgr, frame_width, frame_height);
        if (input.size() != per_window_in) {
            throw std::runtime_error("preprocessed window has an unexpected element count");
        }
        std::copy(input.begin(), input.end(),
                  impl_->batch_input.begin() + static_cast<std::size_t>(window) * per_window_in);
    }

    const std::array<std::int64_t, 4> input_shape{count, impl_->channels, kModelHeight, kModelWidth};
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value tensor = Ort::Value::CreateTensor<float>(memory, impl_->batch_input.data(),
                                                        impl_->batch_input.size(),
                                                        input_shape.data(), input_shape.size());
    const char *input_names[] = {impl_->input_name.c_str()};
    const char *output_names[] = {impl_->output_name.c_str()};
    auto outputs = impl_->session.Run(Ort::RunOptions{nullptr}, input_names, &tensor, 1,
                                      output_names, 1);
    const float *data = outputs[0].GetTensorData<float>();
    heatmaps.assign(data, data + static_cast<std::size_t>(count) * per_window_out);
}

Trajectory TrackNetEngine::PredictChunk(const std::uint8_t *frames, int frame_count,
                                        std::int64_t first_frame_id,
                                        const std::uint8_t *background_bgr, int frame_width,
                                        int frame_height, double fps, int batch_size) {
    (void)fps;
    const int sequence_length = impl_->sequence_length;
    const std::size_t pixels = static_cast<std::size_t>(kModelWidth) * kModelHeight;
    const std::size_t per_window_out = static_cast<std::size_t>(sequence_length) * pixels;

    std::vector<std::int64_t> frame_ids(static_cast<std::size_t>(frame_count));
    for (int index = 0; index < frame_count; ++index) {
        frame_ids[static_cast<std::size_t>(index)] = first_frame_id + index;
    }
    const std::vector<Window> windows =
        BuildWindows(frame_ids, sequence_length, sequence_length, /*pad=*/true);

    // Frame pointer helper: frame `index` lives at frames + index * frame_bytes.
    const std::size_t frame_bytes =
        static_cast<std::size_t>(frame_width) * frame_height * kRgbChannels;
    const auto frame_at = [&](int index) { return frames + static_cast<std::size_t>(index) * frame_bytes; };

    // The product runs the non-overlapping grid, where every frame belongs to
    // exactly one window, so predictions can be decoded straight away.  The
    // accumulating path below (kept for the overlapping modes the reference
    // implementation also supports) would hold one heatmap per frame in memory -
    // ~4.7 MB each, i.e. gigabytes for a 6 s chunk.
    const bool direct = (sequence_length > 0);

    std::map<std::int64_t, std::vector<float>> totals;
    std::map<std::int64_t, double> total_weights;
    std::vector<float> heatmaps;
    Trajectory trajectory;
    if (direct) {
        trajectory.reserve(static_cast<std::size_t>(frame_count));
    }

    const int effective_batch = std::max(1, batch_size);
    for (int start = 0; start < static_cast<int>(windows.size()); start += effective_batch) {
        const int stop = std::min(start + effective_batch, static_cast<int>(windows.size()));
        std::vector<const std::uint8_t *const *> pointers;
        std::vector<std::vector<const std::uint8_t *>> storage;
        storage.reserve(stop - start);
        for (int index = start; index < stop; ++index) {
            std::vector<const std::uint8_t *> window_pointers;
            window_pointers.reserve(sequence_length);
            for (int position = 0; position < sequence_length; ++position) {
                window_pointers.push_back(frame_at(windows[index].frame_indices[position]));
            }
            storage.push_back(std::move(window_pointers));
        }
        for (const auto &item : storage) {
            pointers.push_back(item.data());
        }
        PredictWindows(pointers, effective_batch, background_bgr, frame_width, frame_height, heatmaps);
        for (int index = start; index < stop; ++index) {
            const Window &window = windows[index];
            const float *window_output =
                heatmaps.data() + static_cast<std::size_t>(index - start) * per_window_out;
            for (int position = 0; position < window.valid_length; ++position) {
                const float *plane = window_output + static_cast<std::size_t>(position) * pixels;
                if (direct) {
                    const DecodedPoint decoded =
                        DecodeHeatmap(plane, static_cast<double>(frame_width),
                                      static_cast<double>(frame_height));
                    TrajectoryPoint point;
                    point.frame = window.frame_ids[position];
                    point.x = decoded.x;
                    point.y = decoded.y;
                    point.visible = decoded.visible;
                    trajectory.push_back(point);
                    continue;
                }
                const std::int64_t frame_id = window.frame_ids[position];
                auto &accumulator = totals[frame_id];
                if (accumulator.empty()) {
                    accumulator.assign(per_window_out, 0.0f);
                }
                for (std::size_t element = 0; element < per_window_out; ++element) {
                    accumulator[element] += plane[element];
                }
                total_weights[frame_id] += 1.0;
            }
        }
    }

    if (direct) {
        return trajectory;
    }

    trajectory.reserve(totals.size());
    for (auto &entry : totals) {
        const double weight = total_weights[entry.first] > 0.0 ? total_weights[entry.first] : 1.0;
        std::vector<float> averaged(entry.second.size());
        for (std::size_t element = 0; element < averaged.size(); ++element) {
            averaged[element] = static_cast<float>(entry.second[element] / weight);
        }
        const DecodedPoint decoded =
            DecodeHeatmap(averaged.data(), static_cast<double>(frame_width),
                          static_cast<double>(frame_height));
        TrajectoryPoint point;
        point.frame = entry.first;
        point.x = decoded.x;
        point.y = decoded.y;
        point.visible = decoded.visible;
        trajectory.push_back(point);
    }
    return trajectory;
}

}  // namespace badminton::inference
