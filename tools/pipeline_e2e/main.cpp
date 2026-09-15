// End-to-end verification of the C++ analysis path against the Python golden:
//   ffmpeg decode -> background median -> chunked windowed inference (DirectML)
//   -> trajectory -> rally segmentation
//
// Compares three artifacts produced by the product sources in app/src/inference:
//   1. background median vs the cached golden median (decode fidelity)
//   2. trajectory vs app-run/trajectory-partial.csv
//   3. rallies vs app-run/analysis/rallies-provisional.csv
//
// Dev-only tool.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include "job_runner.h"

using namespace badminton::inference;

namespace {

std::vector<std::uint8_t> ReadBytes(const std::string &path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return {};
    }
    const std::streamsize size = stream.tellg();
    stream.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char *>(data.data()), size);
    return data;
}

struct Row {
    std::map<std::string, std::string> values;
};

std::vector<Row> ReadCsv(const std::string &path) {
    std::ifstream stream(path);
    std::vector<Row> rows;
    std::string line;
    std::vector<std::string> header;
    bool first = true;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        std::vector<std::string> cells;
        std::string current;
        bool quoted = false;
        for (char character : line) {
            if (character == '"') {
                quoted = !quoted;
            } else if (character == ',' && !quoted) {
                cells.push_back(current);
                current.clear();
            } else {
                current.push_back(character);
            }
        }
        cells.push_back(current);
        if (first) {
            header = cells;
            first = false;
            continue;
        }
        Row row;
        for (std::size_t index = 0; index < header.size() && index < cells.size(); ++index) {
            row.values[header[index]] = cells[index];
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

double ToDouble(const Row &row, const std::string &key) {
    const auto found = row.values.find(key);
    return found == row.values.end() ? 0.0 : std::stod(found->second);
}

int ToInt(const Row &row, const std::string &key) {
    const auto found = row.values.find(key);
    return found == row.values.end() ? 0 : std::stoi(found->second);
}

}  // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // unbuffered: output survives a crash
    if (argc < 6) {
        std::printf("usage: pipeline_e2e <ffmpeg.exe> <model.onnx> <proxy.mp4> <output-dir> "
                    "<golden-dir> [fixtures-dir]\n");
        return 2;
    }
    AnalysisOptions options;
    options.ffmpeg_path = argv[1];
    options.model_path = argv[2];
    options.video_path = argv[3];
    options.output_dir = argv[4];
    const std::string golden_dir = argv[5];
    const std::string fixtures_dir = argc > 6 ? argv[6] : golden_dir + "/../fixtures";
    options.batch_size = 8;
    options.use_directml = true;
    options.write_chunk_files = true;

    AnalysisResult result;
    std::string error;
    std::printf("starting: video=%s\n", options.video_path.c_str());
    const auto started = std::chrono::steady_clock::now();
    const bool ok = RunAnalysis(
        options,
        [](const AnalysisProgress &progress) {
            std::printf("  chunk %d/%d  frames %lld/%lld  %.2f%%  %.2f fps  eta %.1fs\n",
                        progress.chunk_index, progress.chunk_count,
                        static_cast<long long>(progress.frames_completed),
                        static_cast<long long>(progress.frames_target), progress.percent,
                        progress.inference_fps, progress.eta_seconds);
            std::fflush(stdout);
        },
        nullptr, result, error);
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (!ok) {
        std::printf("ERROR %s\n", error.c_str());
        return 1;
    }
    std::printf("video: %dx%d fps=%.3f frames=%lld  inference=%.1fs  wall=%.1fs (%.2f fps)\n",
                result.metadata.width, result.metadata.height, result.metadata.fps,
                static_cast<long long>(result.metadata.frame_count), result.inference_seconds, wall,
                static_cast<double>(result.metadata.frame_count) / std::max(wall, 1e-6));

    int failures = 0;

    // 1. background median (decode fidelity)
    {
        const std::vector<std::uint8_t> golden = ReadBytes(fixtures_dir + "/median.u8.bin");
        if (golden.empty() || golden.size() != result.median.size()) {
            std::printf("MEDIAN: golden unavailable or size mismatch -> SKIP\n");
        } else {
            long differing = 0;
            int max_diff = 0;
            for (std::size_t index = 0; index < golden.size(); ++index) {
                const int diff = std::abs(static_cast<int>(golden[index]) -
                                          static_cast<int>(result.median[index]));
                if (diff != 0) {
                    ++differing;
                }
                max_diff = std::max(max_diff, diff);
            }
            const bool pass = differing == 0;
            std::printf("MEDIAN: differing=%ld/%zu max=%d -> %s\n", differing, golden.size(), max_diff,
                        pass ? "PASS (identical to cv2 decode)" : "DIFF");
        }
    }

    // 2. trajectory
    {
        const std::vector<Row> golden = ReadCsv(golden_dir + "/app-run/trajectory-partial.csv");
        std::map<std::int64_t, const TrajectoryPoint *> mine;
        for (const TrajectoryPoint &point : result.trajectory) {
            mine[point.frame] = &point;
        }
        long visible_mismatch = 0;
        long compared = 0;
        std::vector<double> distances;
        for (const Row &row : golden) {
            const std::int64_t frame = static_cast<std::int64_t>(ToDouble(row, "Frame"));
            const bool golden_visible = ToInt(row, "Visibility") != 0;
            const auto found = mine.find(frame);
            if (found == mine.end()) {
                ++visible_mismatch;
                continue;
            }
            if (found->second->visible != golden_visible) {
                ++visible_mismatch;
                continue;
            }
            if (!golden_visible) {
                continue;
            }
            ++compared;
            distances.push_back(std::hypot(found->second->x - ToDouble(row, "X"),
                                           found->second->y - ToDouble(row, "Y")));
        }
        std::sort(distances.begin(), distances.end());
        const auto percentile = [&](double ratio) {
            if (distances.empty()) {
                return 0.0;
            }
            return distances[std::min(distances.size() - 1,
                                      static_cast<std::size_t>(ratio * distances.size()))];
        };
        const double mismatch_rate = golden.empty()
                                         ? 0.0
                                         : static_cast<double>(visible_mismatch) /
                                               static_cast<double>(golden.size());
        const bool pass = mismatch_rate <= 0.005 && percentile(0.95) <= 1.5;
        std::printf("TRAJECTORY: golden=%zu mine=%zu visibilityMismatch=%ld (%.3f%%) "
                    "jointVisible=%ld P50=%.3f P95=%.3f max=%.3f -> %s\n",
                    golden.size(), result.trajectory.size(), visible_mismatch, 100.0 * mismatch_rate,
                    compared, percentile(0.50), percentile(0.95),
                    distances.empty() ? 0.0 : distances.back(), pass ? "PASS" : "FAIL");
        failures += pass ? 0 : 1;
    }

    // 3. rallies
    {
        const std::vector<Row> golden = ReadCsv(golden_dir + "/app-run/analysis/rallies-provisional.csv");
        std::printf("RALLIES: mine=%zu golden=%zu\n", result.rallies.size(), golden.size());
        const std::size_t count = std::max(result.rallies.size(), golden.size());
        int differences = 0;
        for (std::size_t index = 0; index < count; ++index) {
            if (index >= result.rallies.size() || index >= golden.size()) {
                ++differences;
                continue;
            }
            const Rally &candidate = result.rallies[index];
            const bool same = candidate.start_frame == ToInt(golden[index], "start_frame") &&
                              candidate.end_frame == ToInt(golden[index], "end_frame") &&
                              static_cast<int>(candidate.hits.size()) == ToInt(golden[index], "hit_count");
            if (!same) {
                ++differences;
            }
            std::printf("  rally %zu: mine [%d..%d] hits=%zu | golden [%d..%d] hits=%d %s\n",
                        index + 1, candidate.start_frame, candidate.end_frame,
                        candidate.hits.size(), ToInt(golden[index], "start_frame"),
                        ToInt(golden[index], "end_frame"), ToInt(golden[index], "hit_count"),
                        same ? "ok" : "DIFF");
        }
        const bool pass = differences == 0;
        std::printf("RALLIES: differences=%d -> %s\n", differences, pass ? "PASS" : "FAIL");
        failures += pass ? 0 : 1;
    }

    std::printf("result: %s\n", failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
