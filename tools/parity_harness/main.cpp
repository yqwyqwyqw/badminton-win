// Golden parity harness for the C++ inference kernel (M2 steps 1-3).
//
// Verifies the ported modules against artifacts produced by the Python product
// path, so a regression cannot hide behind "looks right":
//
//   T1 preprocess  fixture frames + median -> BuildModelInput   == preprocessed.f32.bin
//   T2 postprocess golden heatmaps         -> DecodeHeatmap     == decoded-golden.txt
//   T3 end-to-end  fixture frames + median -> preprocess + ONNX -> decode
//                                                              == decoded-golden.txt
//
// Dev-only tool; the product kernel lives in app/src/inference/.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "postprocess.h"
#include "preprocess.h"
#include "rally_state_machine.h"
#include "tracknet_engine.h"

using namespace badminton::inference;

namespace {

constexpr int kModelPixels = kModelWidth * kModelHeight;
constexpr int kInChannels = 27;   // (seq_len 8 + background) * 3
constexpr int kOutFrames = 8;

struct Fixture {
    int windows = 0;
    int frames_per_window = 0;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> frames;      // windows * frames_per_window * H * W * 3
    std::vector<std::uint8_t> median;      // H * W * 3
    std::vector<float> preprocessed;       // windows * 27 * 288 * 512
    std::vector<float> heatmaps;           // windows * 8 * 288 * 512
    struct GoldenPoint {
        double x = 0.0;
        double y = 0.0;
        int visible = 0;
    };
    std::vector<GoldenPoint> decoded;      // windows * 8
};

std::vector<std::uint8_t> ReadBytes(const std::string &path, std::size_t expected) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        throw std::runtime_error("cannot open " + path);
    }
    const std::streamsize size = stream.tellg();
    stream.seekg(0, std::ios::beg);
    if (expected != 0 && static_cast<std::size_t>(size) != expected) {
        throw std::runtime_error(path + ": unexpected size");
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char *>(data.data()), size);
    return data;
}

std::vector<float> ReadFloats(const std::string &path) {
    const std::vector<std::uint8_t> bytes = ReadBytes(path, 0);
    if (bytes.size() % sizeof(float) != 0) {
        throw std::runtime_error(path + ": not a float32 file");
    }
    std::vector<float> data(bytes.size() / sizeof(float));
    std::memcpy(data.data(), bytes.data(), bytes.size());
    return data;
}

Fixture LoadFixture(const std::string &dir, int windows, int width, int height,
                    int frames_per_window) {
    Fixture fixture;
    fixture.windows = windows;
    fixture.frames_per_window = frames_per_window;
    fixture.width = width;
    fixture.height = height;
    const std::size_t frame_bytes = static_cast<std::size_t>(width) * height * 3;
    fixture.frames = ReadBytes(dir + "/frames.u8.bin",
                               frame_bytes * windows * frames_per_window);
    fixture.median = ReadBytes(dir + "/median.u8.bin", frame_bytes);
    fixture.preprocessed = ReadFloats(dir + "/preprocessed.f32.bin");
    fixture.heatmaps = ReadFloats(dir + "/heatmaps-ref.f32.bin");
    if (fixture.preprocessed.size() !=
        static_cast<std::size_t>(windows) * kInChannels * kModelPixels) {
        throw std::runtime_error("preprocessed fixture has an unexpected size");
    }
    if (fixture.heatmaps.size() !=
        static_cast<std::size_t>(windows) * kOutFrames * kModelPixels) {
        throw std::runtime_error("heatmap fixture has an unexpected size");
    }

    std::ifstream text(dir + "/decoded-golden.txt");
    if (!text) {
        throw std::runtime_error("cannot open decoded-golden.txt");
    }
    int window = 0, position = 0;
    double x = 0.0, y = 0.0;
    int visible = 0;
    while (text >> window >> position >> x >> y >> visible) {
        Fixture::GoldenPoint point;
        point.x = x;
        point.y = y;
        point.visible = visible;
        fixture.decoded.push_back(point);
    }
    if (fixture.decoded.size() != static_cast<std::size_t>(windows) * kOutFrames) {
        throw std::runtime_error("decoded-golden.txt has an unexpected row count");
    }
    return fixture;
}

struct DiffStats {
    double max_abs = 0.0;
    long differing = 0;
    long over_one_lsb = 0;
    std::size_t total = 0;
};

DiffStats CompareVectors(const std::vector<float> &a, const std::vector<float> &b) {
    DiffStats stats;
    stats.total = std::min(a.size(), b.size());
    // uint8/255.0f in float32 is not exact, so a one-LSB difference can measure a
    // hair above 1/255; allow 0.1% slack in the "more than one LSB" counter.
    const double one_lsb = (1.0 / 255.0) * 1.001;
    for (std::size_t index = 0; index < stats.total; ++index) {
        const double diff = std::fabs(static_cast<double>(a[index]) - static_cast<double>(b[index]));
        if (diff > 0.0) {
            ++stats.differing;
        }
        if (diff > one_lsb) {
            ++stats.over_one_lsb;
        }
        stats.max_abs = std::max(stats.max_abs, diff);
    }
    return stats;
}

int TestPreprocess(const Fixture &fixture) {
    const std::size_t frame_bytes =
        static_cast<std::size_t>(fixture.width) * fixture.height * 3;
    const std::size_t per_window = static_cast<std::size_t>(kInChannels) * kModelPixels;
    std::vector<float> mine(fixture.preprocessed.size());
    for (int window = 0; window < fixture.windows; ++window) {
        std::vector<const std::uint8_t *> pointers;
        for (int position = 0; position < fixture.frames_per_window; ++position) {
            const std::size_t offset =
                (static_cast<std::size_t>(window) * fixture.frames_per_window + position) * frame_bytes;
            pointers.push_back(fixture.frames.data() + offset);
        }
        const std::vector<float> built =
            BuildModelInput(pointers.data(), fixture.frames_per_window, fixture.median.data(),
                            fixture.width, fixture.height);
        std::copy(built.begin(), built.end(), mine.begin() + static_cast<std::size_t>(window) * per_window);
    }
    const DiffStats stats = CompareVectors(mine, fixture.preprocessed);
    // Gate: never more than one 8-bit LSB of difference.  cv::resize's 8-bit path
    // carries its own quantisation bias (its float path matches this kernel
    // bit-for-bit), so exact equality is not the right criterion here.
    const double one_lsb = 1.0 / 255.0;
    const bool pass = stats.over_one_lsb == 0 && stats.max_abs <= one_lsb * 1.001;
    std::printf("T1 preprocess : max|d|=%.3e (%.4f LSB) differing=%ld (%.3f%%) over_1lsb=%ld "
                "-> %s\n",
                stats.max_abs, stats.max_abs / one_lsb, stats.differing,
                100.0 * static_cast<double>(stats.differing) / static_cast<double>(stats.total),
                stats.over_one_lsb, pass ? "PASS (<=1 LSB)" : "FAIL");
    return pass ? 0 : 1;
}

int TestPostprocess(const Fixture &fixture, int width, int height) {
    long mismatched = 0;
    double max_distance = 0.0;
    for (int window = 0; window < fixture.windows; ++window) {
        for (int position = 0; position < kOutFrames; ++position) {
            const std::size_t offset =
                (static_cast<std::size_t>(window) * kOutFrames + position) * kModelPixels;
            const DecodedPoint mine =
                DecodeHeatmap(fixture.heatmaps.data() + offset, width, height);
            const Fixture::GoldenPoint &golden =
                fixture.decoded[static_cast<std::size_t>(window) * kOutFrames + position];
            const bool visible_matches = (mine.visible ? 1 : 0) == golden.visible;
            const double distance = std::hypot(mine.x - golden.x, mine.y - golden.y);
            if (!visible_matches || distance > 0.0) {
                ++mismatched;
                max_distance = std::max(max_distance, std::max(distance, visible_matches ? 0.0 : 1.0));
            }
        }
    }
    const bool pass = mismatched == 0;
    std::printf("T2 postprocess: mismatched=%ld of %zu max|d|=%.3f px -> %s\n", mismatched,
                fixture.decoded.size(), max_distance, pass ? "PASS (exact)" : "FAIL");
    return pass ? 0 : 1;
}

int TestEndToEnd(Fixture &fixture, const std::string &model_path, bool use_dml, int batch) {
    TrackNetEngine::Options options;
    options.model_path = model_path;
    options.provider = use_dml ? TrackNetEngine::Provider::kDirectML
                               : TrackNetEngine::Provider::kCpu;
    options.intra_op_threads = 8;
    TrackNetEngine engine(options);
    if (engine.sequence_length() != kOutFrames) {
        std::printf("T3 end-to-end : model seq_len=%d, fixture assumes %d -> SKIP\n",
                    engine.sequence_length(), kOutFrames);
        return 1;
    }

    const std::size_t frame_bytes =
        static_cast<std::size_t>(fixture.width) * fixture.height * 3;
    long visible_mismatch = 0;
    long coordinate_mismatch = 0;
    std::vector<double> distances;
    for (int start = 0; start < fixture.windows; start += batch) {
        const int stop = std::min(start + batch, fixture.windows);
        std::vector<std::vector<const std::uint8_t *>> storage;
        for (int window = start; window < stop; ++window) {
            std::vector<const std::uint8_t *> pointers;
            for (int position = 0; position < fixture.frames_per_window; ++position) {
                const std::size_t offset =
                    (static_cast<std::size_t>(window) * fixture.frames_per_window + position) *
                    frame_bytes;
                pointers.push_back(fixture.frames.data() + offset);
            }
            storage.push_back(std::move(pointers));
        }
        std::vector<const std::uint8_t *const *> pointers;
        for (const auto &item : storage) {
            pointers.push_back(item.data());
        }
        std::vector<float> heatmaps;
        engine.PredictWindows(pointers, batch, fixture.median.data(), fixture.width,
                              fixture.height, heatmaps);
        for (int window = start; window < stop; ++window) {
            for (int position = 0; position < kOutFrames; ++position) {
                const std::size_t local =
                    (static_cast<std::size_t>(window - start) * kOutFrames + position) * kModelPixels;
                const DecodedPoint mine =
                    DecodeHeatmap(heatmaps.data() + local, fixture.width, fixture.height);
                const Fixture::GoldenPoint &golden =
                    fixture.decoded[static_cast<std::size_t>(window) * kOutFrames + position];
                if ((mine.visible ? 1 : 0) != golden.visible) {
                    ++visible_mismatch;
                    continue;
                }
                if (golden.visible == 0) {
                    continue;
                }
                const double distance = std::hypot(mine.x - golden.x, mine.y - golden.y);
                distances.push_back(distance);
                if (distance > 1.5) {
                    ++coordinate_mismatch;
                }
            }
        }
    }
    std::sort(distances.begin(), distances.end());
    const auto percentile = [&](double ratio) {
        if (distances.empty()) {
            return 0.0;
        }
        const std::size_t index = std::min(distances.size() - 1,
                                           static_cast<std::size_t>(ratio * distances.size()));
        return distances[index];
    };
    const double visible_rate = static_cast<double>(visible_mismatch) /
                                static_cast<double>(fixture.decoded.size());
    // Product gates from the refactor plan: visibility agreement >= 99.5% and
    // coordinate P95 <= 1.5 px.
    const bool pass = visible_rate <= 0.005 && percentile(0.95) <= 1.5 && coordinate_mismatch == 0;
    std::printf("T3 end-to-end : visibilityMismatch=%ld/%zu (%.3f%%) coordP50=%.3f coordP95=%.3f "
                "coordMax=%.3f over1.5px=%ld -> %s\n",
                visible_mismatch, fixture.decoded.size(), 100.0 * visible_rate, percentile(0.50),
                percentile(0.95), distances.empty() ? 0.0 : distances.back(), coordinate_mismatch,
                pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

}  // namespace

namespace {

// Minimal CSV reader by header name (dev tool only).
struct Table {
    std::vector<std::string> header;
    std::vector<std::vector<std::string>> rows;

    int column(const std::string &name) const {
        for (std::size_t index = 0; index < header.size(); ++index) {
            if (header[index] == name) {
                return static_cast<int>(index);
            }
        }
        return -1;
    }
};

Table ReadCsv(const std::string &path) {
    std::ifstream stream(path);
    if (!stream) {
        throw std::runtime_error("cannot open " + path);
    }
    Table table;
    std::string line;
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
            table.header = cells;
            first = false;
        } else {
            table.rows.push_back(std::move(cells));
        }
    }
    return table;
}

// T4: trajectory CSV -> rally segmentation, compared against the golden
// rallies-provisional.csv produced by validation/analyze_tracknet_rallies.py.
int TestRallies(const std::string &trajectory_path, const std::string &rallies_path, double fps,
                int width, int height) {
    const Table trajectory_table = ReadCsv(trajectory_path);
    const int frame_column = trajectory_table.column("Frame");
    const int visibility_column = trajectory_table.column("Visibility");
    const int x_column = trajectory_table.column("X");
    const int y_column = trajectory_table.column("Y");
    if (frame_column < 0 || visibility_column < 0 || x_column < 0 || y_column < 0) {
        std::printf("T4 rallies   : trajectory CSV is missing Frame/Visibility/X/Y -> SKIP\n");
        return 1;
    }
    Trajectory trajectory;
    trajectory.reserve(trajectory_table.rows.size());
    for (const auto &row : trajectory_table.rows) {
        TrajectoryPoint point;
        point.frame = std::stoll(row[static_cast<std::size_t>(frame_column)]);
        point.visible = std::stoi(row[static_cast<std::size_t>(visibility_column)]) != 0;
        point.x = std::stod(row[static_cast<std::size_t>(x_column)]);
        point.y = std::stod(row[static_cast<std::size_t>(y_column)]);
        trajectory.push_back(point);
    }

    const std::vector<Rally> mine = AnalyseRallies(trajectory, fps, width, height);

    const Table golden_table = ReadCsv(rallies_path);
    struct GoldenRally {
        int start_frame = 0;
        int end_frame = 0;
        int hit_count = 0;
        int observed = 0;
        int inferred = 0;
    };
    std::vector<GoldenRally> golden;
    const int start_column = golden_table.column("start_frame");
    const int end_column = golden_table.column("end_frame");
    const int hit_column = golden_table.column("hit_count");
    const int observed_column = golden_table.column("observed_hits");
    const int inferred_column = golden_table.column("inferred_gap_hits");
    for (const auto &row : golden_table.rows) {
        GoldenRally rally;
        rally.start_frame = std::stoi(row[static_cast<std::size_t>(start_column)]);
        rally.end_frame = std::stoi(row[static_cast<std::size_t>(end_column)]);
        rally.hit_count = std::stoi(row[static_cast<std::size_t>(hit_column)]);
        rally.observed = std::stoi(row[static_cast<std::size_t>(observed_column)]);
        rally.inferred = std::stoi(row[static_cast<std::size_t>(inferred_column)]);
        golden.push_back(rally);
    }

    std::printf("T4 rallies   : frames=%zu fps=%.3f mine=%zu golden=%zu\n", trajectory.size(), fps,
                mine.size(), golden.size());
    int differences = 0;
    const std::size_t count = std::max(mine.size(), golden.size());
    for (std::size_t index = 0; index < count; ++index) {
        const bool has_mine = index < mine.size();
        const bool has_golden = index < golden.size();
        if (!has_mine || !has_golden) {
            ++differences;
            std::printf("               rally %zu: only in %s\n", index + 1,
                        has_mine ? "candidate" : "golden");
            continue;
        }
        const Rally &candidate = mine[index];
        const GoldenRally &reference = golden[index];
        const bool same = candidate.start_frame == reference.start_frame &&
                          candidate.end_frame == reference.end_frame &&
                          static_cast<int>(candidate.hits.size()) == reference.hit_count &&
                          candidate.observed_hits == reference.observed &&
                          candidate.inferred_gap_hits == reference.inferred;
        if (!same) {
            ++differences;
        }
        std::printf("               rally %zu: mine [%d..%d] hits=%zu (obs=%d inf=%d) | "
                    "golden [%d..%d] hits=%d (obs=%d inf=%d) %s\n",
                    index + 1, candidate.start_frame, candidate.end_frame, candidate.hits.size(),
                    candidate.observed_hits, candidate.inferred_gap_hits, reference.start_frame,
                    reference.end_frame, reference.hit_count, reference.observed, reference.inferred,
                    same ? "ok" : "DIFF");
    }
    const bool pass = differences == 0;
    std::printf("T4 rallies   : differences=%d -> %s\n", differences, pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::printf("usage: parity_harness <fixture-dir> <model.onnx> [dml|cpu] [batch] "
                    "[width height frames-per-window]\n");
        return 2;
    }
    const std::string dir = argv[1];
    const std::string model = argv[2];
    const bool use_dml = argc > 3 ? std::string(argv[3]) != "cpu" : true;
    const int batch = argc > 4 ? std::atoi(argv[4]) : 8;
    int width = argc > 6 ? std::atoi(argv[5]) : 1280;
    int height = argc > 6 ? std::atoi(argv[6]) : 720;
    const int frames_per_window = argc > 7 ? std::atoi(argv[7]) : 8;
    const int windows = 8;

    try {
        Fixture fixture = LoadFixture(dir, windows, width, height, frames_per_window);
        std::printf("fixture: windows=%d frames/window=%d %dx%d provider=%s batch=%d\n",
                    fixture.windows, fixture.frames_per_window, width, height,
                    use_dml ? "dml" : "cpu", batch);

        int failures = 0;
        failures += TestPreprocess(fixture);
        failures += TestPostprocess(fixture, width, height);
        failures += TestEndToEnd(fixture, model, use_dml, batch);
        if (argc > 9) {
            const double fps = argc > 10 ? std::atof(argv[10]) : 50.0;
            failures += TestRallies(argv[8], argv[9], fps, width, height);
        }
        std::printf("result: %s\n", failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
        return failures == 0 ? 0 : 1;
    } catch (const std::exception &error) {
        std::printf("ERROR %s\n", error.what());
        return 2;
    }
}
