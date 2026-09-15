#include "job_runner.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <sstream>

#include "path_utf8.h"

namespace badminton::inference {
namespace {

namespace fs = std::filesystem;

// numpy.linspace(0, frames - 1, samples).astype(int) truncates towards zero.
std::vector<std::int64_t> SampleIndices(std::int64_t frames, int samples) {
    std::vector<std::int64_t> indices;
    const int count = std::max(3, samples);
    if (frames <= 0) {
        return indices;
    }
    if (frames == 1) {
        indices.push_back(0);
        return indices;
    }
    const double step = static_cast<double>(frames - 1) / static_cast<double>(count - 1);
    for (int index = 0; index < count; ++index) {
        indices.push_back(static_cast<std::int64_t>(static_cast<double>(index) * step));
    }
    return indices;
}

// np.median over the samples, per pixel and channel.
void MedianOfFrames(const std::vector<std::vector<std::uint8_t>> &frames, std::size_t pixels,
                    std::vector<std::uint8_t> &median) {
    median.assign(pixels * 3, 0);
    const std::size_t count = frames.size();
    std::vector<std::uint8_t> column(count);
    for (std::size_t index = 0; index < pixels * 3; ++index) {
        for (std::size_t sample = 0; sample < count; ++sample) {
            column[sample] = frames[sample][index];
        }
        const std::size_t middle = count / 2;
        std::nth_element(column.begin(), column.begin() + static_cast<std::ptrdiff_t>(middle),
                         column.end());
        median[index] = column[middle];
    }
}

void WriteChunkCsv(const fs::path &path, const Trajectory &points) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << "Frame,Visibility,X,Y\n";
    for (const TrajectoryPoint &point : points) {
        stream << point.frame << ',' << (point.visible ? 1 : 0) << ',' << point.x << ',' << point.y
               << '\n';
    }
}

// Reads a chunk CSV written by an earlier run (resume support).
bool ReadChunkCsv(const fs::path &path, Trajectory &points) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    std::string line;
    if (!std::getline(stream, line)) {
        return false;
    }
    points.clear();
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        std::istringstream parser(line);
        std::string frame_text, visibility_text, x_text, y_text;
        if (!std::getline(parser, frame_text, ',') || !std::getline(parser, visibility_text, ',') ||
            !std::getline(parser, x_text, ',') || !std::getline(parser, y_text, ',')) {
            return false;
        }
        TrajectoryPoint point;
        point.frame = std::stoll(frame_text);
        point.visible = std::stoi(visibility_text) != 0;
        point.x = std::stod(x_text);
        point.y = std::stod(y_text);
        points.push_back(point);
    }
    return !points.empty();
}

std::string FormatDouble(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.6f", value);
    return buffer;
}

}  // namespace

std::filesystem::path ChunkCsvPath(const std::string &output_dir, int chunk_index) {
    char name[64];
    std::snprintf(name, sizeof(name), "chunk-%05d.csv", chunk_index);
    return Utf8Path(output_dir) / "chunks" / name;
}

std::filesystem::path TrajectoryCsvPath(const std::string &output_dir) {
    return Utf8Path(output_dir) / "trajectory-partial.csv";
}

bool RunAnalysis(const AnalysisOptions &options, const ProgressCallback &on_progress,
                 const CancelCallback &should_cancel, AnalysisResult &result, std::string &error) {
    if (!fs::exists(Utf8Path(options.ffmpeg_path))) {
        error = "ffmpeg not found: " + options.ffmpeg_path;
        return false;
    }
    if (!fs::exists(Utf8Path(options.model_path))) {
        error = "model not found: " + options.model_path;
        return false;
    }
    if (!ProbeVideo(options.ffmpeg_path, options.video_path, result.metadata, error)) {
        return false;
    }
    const VideoMetadata &metadata = result.metadata;
    std::fprintf(stderr, "[job] probe ok: %dx%d fps=%.3f frames=%lld\n", metadata.width,
                 metadata.height, metadata.fps, static_cast<long long>(metadata.frame_count));
    std::fflush(stderr);
    const std::int64_t total_frames = metadata.frame_count;
    const std::int64_t target_frames =
        options.max_frames > 0 ? std::min(total_frames, options.max_frames) : total_frames;
    const int core_frames = std::max(8, static_cast<int>(std::lround(options.chunk_seconds * metadata.fps)));
    const int overlap_frames =
        std::max(7, static_cast<int>(std::lround(options.overlap_seconds * metadata.fps)));

    TrackNetEngine::Options engine_options;
    engine_options.model_path = options.model_path;
    engine_options.provider =
        options.use_directml ? TrackNetEngine::Provider::kDirectML : TrackNetEngine::Provider::kCpu;
    engine_options.intra_op_threads = options.intra_op_threads;
    TrackNetEngine engine(engine_options);
    const int seq_len = engine.sequence_length();

    fs::create_directories(Utf8Path(options.output_dir));
    if (options.write_chunk_files) {
        fs::create_directories(Utf8Path(options.output_dir) / "chunks");
    }

    // ---- background median (41 sampled frames, BGR uint8) -------------------
    const std::vector<std::int64_t> samples = SampleIndices(total_frames, options.median_samples);
    const std::size_t frame_bytes = static_cast<std::size_t>(metadata.width) *
                                    static_cast<std::size_t>(metadata.height) * 3;
    std::vector<std::vector<std::uint8_t>> sampled;
    sampled.reserve(samples.size());
    for (std::int64_t index : samples) {
        std::vector<std::uint8_t> frame;
        std::fprintf(stderr, "[job] median sample frame=%lld\n", static_cast<long long>(index));
        std::fflush(stderr);
        if (!ReadFramesBgr24(options.ffmpeg_path, options.video_path, index, 1, metadata.width,
                             metadata.height, metadata.fps, frame, error)) {
            error = "median sample " + std::to_string(index) + ": " + error;
            return false;
        }
        sampled.push_back(std::move(frame));
    }
    MedianOfFrames(sampled, frame_bytes / 3, result.median);
    sampled.clear();

    // ---- chunked inference --------------------------------------------------
    const int chunk_count = static_cast<int>((target_frames + core_frames - 1) / core_frames);
    const auto started = std::chrono::steady_clock::now();
    std::int64_t frames_completed = 0;
    for (int chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        if (should_cancel && should_cancel()) {
            result.inference_seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            return true;
        }
        const std::int64_t core_start = static_cast<std::int64_t>(chunk_index) * core_frames;
        const std::int64_t core_end = std::min(target_frames, core_start + core_frames);
        std::int64_t read_start = std::max<std::int64_t>(0, core_start - overlap_frames);
        const std::int64_t read_end = std::min(total_frames, core_end + overlap_frames);
        read_start -= read_start % seq_len;  // keep one global window grid

        const fs::path chunk_path = ChunkCsvPath(options.output_dir, chunk_index);
        Trajectory kept;
        bool reused = false;
        if (options.write_chunk_files && fs::exists(chunk_path)) {
            // Resume: reuse the trajectory an earlier run already produced.  The
            // chunk still flows through the publish/progress code below -- skipping
            // it would freeze the UI's progress bar until the first fresh chunk.
            if (ReadChunkCsv(chunk_path, kept)) {
                reused = true;
            }
        }

        if (!reused) {
            std::vector<std::uint8_t> frames;
            std::fprintf(stderr, "[job] chunk %d decode frames %lld..%lld\n", chunk_index,
                         static_cast<long long>(read_start), static_cast<long long>(read_end));
            std::fflush(stderr);
            if (!ReadFramesBgr24(options.ffmpeg_path, options.video_path, read_start,
                                 read_end - read_start, metadata.width, metadata.height, metadata.fps,
                                 frames, error)) {
                error = "chunk " + std::to_string(chunk_index) + ": " + error;
                return false;
            }
            std::fprintf(stderr, "[job] chunk %d decoded %zu bytes, inferring\n", chunk_index,
                         frames.size());
            std::fflush(stderr);

            Trajectory chunk_trajectory =
                engine.PredictChunk(frames.data(), static_cast<int>(read_end - read_start), read_start,
                                    result.median.data(), metadata.width, metadata.height, metadata.fps,
                                    options.batch_size);
            frames.clear();
            frames.shrink_to_fit();

            kept.reserve(static_cast<std::size_t>(core_end - core_start));
            for (const TrajectoryPoint &point : chunk_trajectory) {
                if (point.frame >= core_start && point.frame < core_end) {
                    kept.push_back(point);
                }
            }
            std::sort(
                kept.begin(), kept.end(),
                [](const TrajectoryPoint &a, const TrajectoryPoint &b) { return a.frame < b.frame; });
            if (options.write_chunk_files) {
                WriteChunkCsv(chunk_path, kept);
            }
        }
        result.trajectory.insert(result.trajectory.end(), kept.begin(), kept.end());
        frames_completed += core_end - core_start;
        result.chunks_completed = chunk_index + 1;

        const bool is_final = frames_completed >= target_frames;
        if (options.on_trajectory) {
            // Hand the partial trajectory out before reporting progress so the
            // caller can publish newly closed rallies together with the update.
            options.on_trajectory(result.trajectory, metadata, is_final);
        }

        if (on_progress) {
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            AnalysisProgress progress;
            progress.status = frames_completed >= target_frames ? "complete" : "running";
            progress.chunk_index = chunk_index + 1;
            progress.chunk_count = chunk_count;
            progress.frames_completed = frames_completed;
            progress.frames_target = target_frames;
            progress.frames_in_video = total_frames;
            progress.percent = 100.0 * static_cast<double>(frames_completed) /
                               static_cast<double>(std::max<std::int64_t>(1, target_frames));
            progress.fps = metadata.fps;
            progress.inference_fps =
                elapsed > 0.0 ? static_cast<double>(frames_completed) / elapsed : 0.0;
            progress.eta_seconds = progress.inference_fps > 0.0
                                       ? static_cast<double>(target_frames - frames_completed) /
                                             progress.inference_fps
                                       : 0.0;
            on_progress(progress);
        }
    }

    result.inference_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::sort(result.trajectory.begin(), result.trajectory.end(),
              [](const TrajectoryPoint &a, const TrajectoryPoint &b) { return a.frame < b.frame; });

    if (options.write_chunk_files) {
        WriteChunkCsv(TrajectoryCsvPath(options.output_dir), result.trajectory);
    }

    result.rallies = AnalyseRallies(result.trajectory, metadata.fps, metadata.width, metadata.height);
    return true;
}

}  // namespace badminton::inference
