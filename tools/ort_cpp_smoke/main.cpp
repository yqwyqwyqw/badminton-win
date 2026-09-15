// M2 step 1 smoke test: run the exported TrackNet ONNX model from C++ on the
// DirectML execution provider and compare against the frozen PyTorch golden
// heatmaps.  Proves the C++ runtime + EP integration and its numerics before the
// preprocessing / postprocessing / state-machine port begins.
//
// Usage:
//   ort_cpp_smoke <model.onnx> <golden-bin-dir> [dml|cpu] [batch]

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::vector<float> read_floats(const std::string &path, size_t expected, bool &ok) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        std::printf("ERROR cannot open %s\n", path.c_str());
        ok = false;
        return {};
    }
    const std::streamsize bytes = stream.tellg();
    stream.seekg(0, std::ios::beg);
    if (static_cast<size_t>(bytes) != expected * sizeof(float)) {
        std::printf("ERROR %s is %lld bytes, expected %zu\n", path.c_str(),
                    static_cast<long long>(bytes), expected * sizeof(float));
        ok = false;
        return {};
    }
    std::vector<float> data(expected);
    stream.read(reinterpret_cast<char *>(data.data()), bytes);
    ok = true;
    return data;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::printf("usage: ort_cpp_smoke <model.onnx> <golden-bin-dir> [dml|cpu] [batch]\n");
        return 2;
    }
    const std::string model_path = argv[1];
    const std::string data_dir = argv[2];
    const std::string provider = argc > 3 ? argv[3] : "dml";
    const int requested_batch = argc > 4 ? std::atoi(argv[4]) : 8;

    int n = 0, c_in = 0, c_out = 0, height = 0, width = 0;
    {
        std::ifstream dims(data_dir + "/dims.txt");
        if (!dims || !(dims >> n >> c_in >> c_out >> height >> width)) {
            std::printf("ERROR cannot read %s/dims.txt\n", data_dir.c_str());
            return 2;
        }
    }
    std::printf("golden: windows=%d in=%dx%dx%d out=%dx%dx%d\n", n, c_in, height, width,
                c_out, height, width);

    bool ok = true;
    const size_t in_per_window = static_cast<size_t>(c_in) * height * width;
    const size_t out_per_window = static_cast<size_t>(c_out) * height * width;
    std::vector<float> inputs = read_floats(data_dir + "/inputs.f32.bin", n * in_per_window, ok);
    std::vector<float> reference =
        read_floats(data_dir + "/reference.f32.bin", n * out_per_window, ok);
    if (!ok) {
        return 2;
    }

    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "ort_cpp_smoke");
    Ort::SessionOptions options;
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    options.SetIntraOpNumThreads(8);
    try {
        if (provider == "dml") {
            options.AppendExecutionProvider("DML", {});
        }
        // "cpu" needs no registration: CPU is the implicit fallback provider, and
        // the DirectML build does not accept the name "CPU" here.
    } catch (const Ort::Exception &error) {
        std::printf("ERROR cannot enable %s: %s\n", provider.c_str(), error.what());
        return 3;
    }

    Ort::Session session(env, std::wstring(model_path.begin(), model_path.end()).c_str(), options);
    Ort::AllocatorWithDefaultOptions allocator;
    const std::string input_name = session.GetInputNameAllocated(0, allocator).get();
    const std::string output_name = session.GetOutputNameAllocated(0, allocator).get();
    std::printf("model: input=%s output=%s provider=%s batch=%d\n", input_name.c_str(),
                output_name.c_str(), provider.c_str(), requested_batch);

    const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::vector<float> produced(static_cast<size_t>(n) * out_per_window);
    const int batch = std::max(1, std::min(requested_batch, n));

    const char *in_names[] = {input_name.c_str()};
    const char *out_names[] = {output_name.c_str()};
    const auto run_pass = [&](bool timed) {
        for (int start = 0; start < n; start += batch) {
            const int count = std::min(batch, n - start);
            const std::array<int64_t, 4> in_shape{count, c_in, height, width};
            Ort::Value in_tensor = Ort::Value::CreateTensor<float>(
                memory, inputs.data() + static_cast<size_t>(start) * in_per_window,
                static_cast<size_t>(count) * in_per_window, in_shape.data(), in_shape.size());
            auto outputs =
                session.Run(Ort::RunOptions{nullptr}, in_names, &in_tensor, 1, out_names, 1);
            if (timed) {
                const float *data = outputs[0].GetTensorData<float>();
                std::copy(data, data + static_cast<size_t>(count) * out_per_window,
                          produced.begin() + static_cast<size_t>(start) * out_per_window);
            }
        }
    };

    // First pass warms up the EP (DML compiles kernels on first use), second pass
    // is the steady-state measurement the product will actually see.
    run_pass(false);
    const auto started = std::chrono::steady_clock::now();
    run_pass(true);
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started;

    double max_diff = 0.0, sum_diff = 0.0;
    size_t over_001 = 0, over_005 = 0;
    for (size_t index = 0; index < produced.size(); ++index) {
        const double diff = std::fabs(static_cast<double>(produced[index]) - reference[index]);
        max_diff = std::max(max_diff, diff);
        sum_diff += diff;
        if (diff > 0.01) ++over_001;
        if (diff > 0.05) ++over_005;
    }
    const double mean_diff = sum_diff / static_cast<double>(produced.size());
    const double seconds = elapsed.count();
    const double fps = seconds > 0 ? (static_cast<double>(n) / seconds) : 0.0;

    std::printf("parity: max|d|=%.6e mean|d|=%.6e over0.01=%zu over0.05=%zu of %zu\n", max_diff,
                mean_diff, over_001, over_005, produced.size());
    std::printf("timing: %.3f s for %d windows -> %.2f fps (%.4f x realtime at 50fps)\n", seconds,
                n, fps, fps / 50.0);
    const bool pass = max_diff <= 2e-2;
    std::printf("verdict: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
