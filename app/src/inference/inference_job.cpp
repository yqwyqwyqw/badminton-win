#include "inference_job.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QTextStream>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "job_runner.h"
#include "path_utf8.h"
#include "video_decoder.h"

namespace {

namespace fs = std::filesystem;

// The kernel helpers live in badminton::inference; bring them into the file's
// anonymous namespace so the unqualified calls below resolve.
using badminton::inference::Utf8Path;
using badminton::inference::Utf8String;

std::string ToUtf8(const QString &text)
{
    const QByteArray bytes = text.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

QString FromUtf8(const std::string &text)
{
    return QString::fromUtf8(text.c_str(), static_cast<int>(text.size()));
}

// Minimal JSON string escaping (paths contain backslashes on Windows).
std::string EscapeJson(const std::string &text)
{
    std::string escaped;
    escaped.reserve(text.size() + 8);
    for (char character : text) {
        switch (character) {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default: escaped += character; break;
        }
    }
    return escaped;
}

std::string Fixed(double value, int digits = 3)
{
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(digits);
    stream << value;
    return stream.str();
}

// Atomic-ish write: the UI polls these files, so never leave a partial file.
//
// Replacing the destination is not always allowed on Windows: while the UI holds
// the file open for reading (QFile does not pass FILE_SHARE_DELETE) MoveFileEx
// fails.  Retry briefly and only then overwrite in place -- otherwise a progress
// update is lost for good and the UI keeps showing a stale percentage (that is
// how the run appeared to stop at 99.54% while the analysis had finished).
void WriteTextFile(const fs::path &path, const std::string &text)
{
    fs::create_directories(path.parent_path());
    // Append on the native path: going through .string() would re-encode with the
    // ANSI code page and corrupt non-ASCII directories.
    fs::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream << text;
        stream.flush();
    }
    std::error_code error;
    for (int attempt = 0; attempt < 20; ++attempt) {
        error.clear();
        fs::rename(temporary, path, error);
        if (!error) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    // Last resort: overwrite in place.  A reader may briefly see a truncated file;
    // the QML side then falls back to its previous value instead of getting stuck.
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << text;
    }
    std::error_code ignored;
    fs::remove(temporary, ignored);
}

std::string FormatSeconds(double value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    return buffer;
}

}  // namespace

InferenceJob::InferenceJob(QObject *parent) : QObject(parent) {}

InferenceJob::~InferenceJob()
{
    cancel();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

void InferenceJob::start(const Request &request)
{
    if (m_running.load()) {
        return;
    }
    if (m_worker.joinable()) {
        m_worker.join();
    }
    m_cancel.store(false);
    m_running.store(true);
    m_outputDir = request.outputDir;
    m_emitted.clear();
    m_worker = std::thread([this, request]() { run(request); });
}

void InferenceJob::cancel()
{
    m_cancel.store(true);
}

void InferenceJob::run(Request request)
{
    badminton::inference::AnalysisOptions options;
    options.ffmpeg_path = ToUtf8(request.ffmpegPath);
    options.model_path = ToUtf8(request.modelPath);
    options.video_path = ToUtf8(request.videoPath);
    options.output_dir = ToUtf8(request.outputDir);
    options.batch_size = request.batchSize;
    options.chunk_seconds = request.chunkSeconds;
    options.overlap_seconds = request.overlapSeconds;
    options.median_samples = request.medianSamples;
    options.use_directml = request.useDirectML;
    options.intra_op_threads = request.intraOpThreads;
    options.max_frames = static_cast<std::int64_t>(request.maxFrames);
    options.write_chunk_files = true;

    m_outputDirUtf8 = options.output_dir;
    m_ffmpegPath = options.ffmpeg_path;
    m_videoPath = options.video_path;
    m_maxRallies = request.maxRallies;
    m_writeClips = request.writeRallyClips;

    // Publish rallies incrementally: the UI promises "每识别完一个完整回合，就会
    // 立即出现在右侧", which the reference implementation satisfied by re-running
    // the rally heuristic after every chunk.
    options.on_trajectory = [this](const badminton::inference::Trajectory &trajectory,
                                   const badminton::inference::VideoMetadata &metadata, bool final) {
        publishRallies(trajectory, metadata, final);
    };

    badminton::inference::AnalysisResult result;
    std::string error;
    const bool ok = badminton::inference::RunAnalysis(
        options,
        [this](const badminton::inference::AnalysisProgress &progress) {
            writeProgress(progress);
            QMetaObject::invokeMethod(this, [this]() { emit progressed(); }, Qt::QueuedConnection);
        },
        [this]() { return m_cancel.load(); }, result, error);

    if (!ok) {
        m_running.store(false);
        const QString message = FromUtf8(error);
        QMetaObject::invokeMethod(
            this, [this, message]() { emit finished(false, message); }, Qt::QueuedConnection);
        return;
    }

    // Final pass so the trailing rally is published too.
    const bool was_cancelled = m_cancel.load();
    if (!was_cancelled) {
        publishRallies(result.trajectory, result.metadata, true);
    }
    const QString message = QStringLiteral("已识别 %1 个回合").arg(m_emitted.size());
    m_running.store(false);
    QMetaObject::invokeMethod(
        this, [this, message]() { emit finished(true, message); }, Qt::QueuedConnection);
}

void InferenceJob::writeProgress(const badminton::inference::AnalysisProgress &progress) const
{
    const fs::path path = Utf8Path(ToUtf8(m_outputDir)) / "progress.json";
    std::ostringstream stream;
    stream << "{\n"
           << "  \"status\": \"" << progress.status << "\",\n"
           << "  \"framesCompleted\": " << progress.frames_completed << ",\n"
           << "  \"framesTarget\": " << progress.frames_target << ",\n"
           << "  \"framesInVideo\": " << progress.frames_in_video << ",\n"
           << "  \"percent\": " << Fixed(progress.percent, 2) << ",\n"
           << "  \"currentChunk\": " << progress.chunk_index << ",\n"
           << "  \"chunkCount\": " << progress.chunk_count << ",\n"
           << "  \"fps\": " << Fixed(progress.fps, 3) << ",\n"
           << "  \"inferenceFramesPerSecondThisRun\": " << Fixed(progress.inference_fps, 3) << ",\n"
           << "  \"etaSeconds\": " << (progress.eta_seconds > 0.0 ? Fixed(progress.eta_seconds, 1) : "null")
           << ",\n"
           << "  \"ralliesReady\": " << m_emitted.size() << ",\n"
           << "  \"updatedAtUnix\": "
           << Fixed(static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::system_clock::now().time_since_epoch())
                                            .count()) /
                        1000.0,
                    3)
           << "\n}\n";
    WriteTextFile(path, stream.str());
}

std::string InferenceJob::makeClip(const badminton::inference::Rally &rally, std::size_t index) const
{
    const fs::path output = Utf8Path(m_outputDirUtf8);
    fs::create_directories(output / "rallies");
    char name[96];
    std::snprintf(name, sizeof(name), "rally-%04zu-%d-%d.mp4", index + 1, rally.start_frame,
                  rally.end_frame);
    const fs::path clip = output / "rallies" / name;
    std::error_code ignored;
    fs::remove(clip, ignored);
    // Same ffmpeg invocation as the reference implementation.
    const std::vector<std::string> arguments = {
        "-hide_banner", "-loglevel", "error", "-y", "-nostdin",
        "-ss",          FormatSeconds(std::max(0.0, rally.start_seconds - 0.5)),
        "-to",          FormatSeconds(rally.end_seconds + 0.8),
        "-i",           m_videoPath,
        "-map",         "0:v:0",
        "-an",
        "-c:v",         "libx264",
        "-preset",      "veryfast",
        "-crf",         "18",
        "-movflags",    "+faststart",
        Utf8String(clip)};
    std::string clip_error;
    if (badminton::inference::RunCommand(m_ffmpegPath, arguments, clip_error)) {
        return Utf8String(clip);
    }
    // A missing preview must not fail the analysis; the rally list stays usable.
    return {};
}

void InferenceJob::publishRallies(const badminton::inference::Trajectory &trajectory,
                                  const badminton::inference::VideoMetadata &metadata, bool final)
{
    std::vector<badminton::inference::Rally> rallies = badminton::inference::AnalyseRallies(
        trajectory, metadata.fps, metadata.width, metadata.height);
    // Same rule as the reference: a rally is only published once a later one
    // exists, because the trailing one may still grow.
    if (!final && !rallies.empty()) {
        rallies.pop_back();
    }

    bool added = false;
    for (const badminton::inference::Rally &rally : rallies) {
        const bool known = std::any_of(m_emitted.begin(), m_emitted.end(),
                                       [&](const EmittedRally &item) {
                                           return item.rally.start_frame == rally.start_frame;
                                       });
        if (known) {
            continue;
        }
        EmittedRally item;
        item.rally = rally;
        item.clip = m_writeClips ? makeClip(rally, m_emitted.size()) : std::string();
        m_emitted.push_back(std::move(item));
        added = true;
        if (m_maxRallies > 0 && static_cast<int>(m_emitted.size()) >= m_maxRallies) {
            // Trial mode: stop the analysis once enough rallies were found.
            m_cancel.store(true);
            break;
        }
    }
    if (added || final) {
        writeFeedFiles(metadata);
        QMetaObject::invokeMethod(this, [this]() { emit progressed(); }, Qt::QueuedConnection);
    }
}

void InferenceJob::writeFeedFiles(const badminton::inference::VideoMetadata &metadata) const
{
    const fs::path output = Utf8Path(m_outputDirUtf8);
    fs::create_directories(output / "analysis");

    // rally-feed.json (the contract the QML layer reads).
    std::ostringstream feed;
    feed << "[\n";
    for (std::size_t index = 0; index < m_emitted.size(); ++index) {
        const EmittedRally &item = m_emitted[index];
        feed << "  {\n"
             << "    \"rally\": " << (index + 1) << ",\n"
             << "    \"startFrame\": " << item.rally.start_frame << ",\n"
             << "    \"endFrame\": " << item.rally.end_frame << ",\n"
             << "    \"startSeconds\": " << Fixed(item.rally.start_seconds) << ",\n"
             << "    \"endSeconds\": " << Fixed(item.rally.end_seconds) << ",\n"
             << "    \"hitCount\": " << item.rally.hits.size() << ",\n"
             << "    \"manualHitCount\": null,\n"
             << "    \"effectiveHitCount\": " << item.rally.hits.size() << ",\n"
             << "    \"reviewStatus\": \"unreviewed\",\n"
             << "    \"observedHits\": " << item.rally.observed_hits << ",\n"
             << "    \"inferredGapHits\": " << item.rally.inferred_gap_hits << ",\n"
             << "    \"confidence\": \"provisional\",\n"
             << "    \"clip\": ";
        if (item.clip.empty()) {
            feed << "null";
        } else {
            feed << "\"" << EscapeJson(item.clip) << "\"";
        }
        feed << "\n  }" << (index + 1 < m_emitted.size() ? "," : "") << "\n";
    }
    feed << "]\n";
    WriteTextFile(output / "rally-feed.json", feed.str());

    // rally-index.csv
    {
        std::ostringstream csv;
        csv << "rally,start_frame,end_frame,start_seconds,end_seconds,auto_hit_count,"
               "manual_hit_count,effective_hit_count,review_status,observed_hits,"
               "inferred_gap_hits,confidence\n";
        for (std::size_t index = 0; index < m_emitted.size(); ++index) {
            const badminton::inference::Rally &rally = m_emitted[index].rally;
            csv << (index + 1) << ',' << rally.start_frame << ',' << rally.end_frame << ','
                << Fixed(rally.start_seconds) << ',' << Fixed(rally.end_seconds) << ','
                << rally.hits.size() << ",,,unreviewed," << rally.observed_hits << ','
                << rally.inferred_gap_hits << ",provisional\n";
        }
        WriteTextFile(output / "rally-index.csv", csv.str());
    }

    // analysis/rallies-provisional.csv + hits-provisional.csv + summary json
    {
        std::ostringstream csv;
        csv << "rally,start_frame,end_frame,start_seconds,end_seconds,hit_count,observed_hits,"
               "inferred_gap_hits,hit_frames,hit_seconds,confidence\n";
        for (std::size_t index = 0; index < m_emitted.size(); ++index) {
            const badminton::inference::Rally &rally = m_emitted[index].rally;
            csv << (index + 1) << ',' << rally.start_frame << ',' << rally.end_frame << ','
                << Fixed(rally.start_seconds) << ',' << Fixed(rally.end_seconds) << ','
                << rally.hits.size() << ',' << rally.observed_hits << ',' << rally.inferred_gap_hits
                << ",\"";
            for (std::size_t hit = 0; hit < rally.hits.size(); ++hit) {
                csv << (hit ? ";" : "") << rally.hits[hit].frame;
            }
            csv << "\",\"";
            for (std::size_t hit = 0; hit < rally.hits.size(); ++hit) {
                csv << (hit ? ";" : "") << FormatSeconds(rally.hits[hit].seconds);
            }
            csv << "\",provisional\n";
        }
        WriteTextFile(output / "analysis" / "rallies-provisional.csv", csv.str());

        std::ostringstream hits;
        hits << "rally,hit,frame,seconds,x,y,source,rule,confidence\n";
        for (std::size_t index = 0; index < m_emitted.size(); ++index) {
            const badminton::inference::Rally &rally = m_emitted[index].rally;
            for (std::size_t hit = 0; hit < rally.hits.size(); ++hit) {
                const auto &event = rally.hits[hit];
                hits << (index + 1) << ',' << (hit + 1) << ',' << event.frame << ','
                     << FormatSeconds(event.seconds) << ',' << Fixed(event.x, 2) << ','
                     << Fixed(event.y, 2) << ',' << (event.observed ? "observed" : "inferred_gap")
                     << ',' << event.rule << ',' << Fixed(event.confidence) << "\n";
            }
        }
        WriteTextFile(output / "analysis" / "hits-provisional.csv", hits.str());

        int totalHits = 0;
        for (const EmittedRally &item : m_emitted) {
            totalHits += static_cast<int>(item.rally.hits.size());
        }
        std::ostringstream summary;
        summary << "{\n  \"schemaVersion\": 1,\n  \"status\": \"PROVISIONAL_REVIEW_REQUIRED\",\n"
                   "  \"audioUsed\": false,\n  \"engine\": \"cpp-onnxruntime\",\n"
                   "  \"video\": {\n    \"fps\": "
                << Fixed(metadata.fps) << ",\n    \"width\": " << metadata.width
                << ",\n    \"height\": " << metadata.height
                << ",\n    \"frames\": " << metadata.frame_count << "\n  },\n"
                << "  \"rallyCount\": " << m_emitted.size() << ",\n  \"hitCount\": " << totalHits
                << ",\n  \"limitations\": [\n"
                   "    \"TrackNet emits one shuttle position per frame; another active court can "
                   "steal the global maximum before ROI filtering.\",\n"
                   "    \"Out-of-frame contacts are inferred from trajectory turns and are not "
                   "direct observations.\",\n"
                   "    \"Counts require review until the court crop and event thresholds are "
                   "validated on labelled long videos.\"\n  ]\n}\n";
        WriteTextFile(output / "analysis" / "rally-analysis-summary.json", summary.str());
    }
}
