#include "video_decoder.h"

#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

namespace badminton::inference {
namespace {

std::wstring Widen(const std::string &utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                                         nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(), size);
    return wide;
}

std::string Narrow(const std::wstring &wide) {
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), narrow.data(), size,
                        nullptr, nullptr);
    return narrow;
}

std::string Quote(const std::string &path) { return "\"" + path + "\""; }

std::string DirectoryOf(const std::string &path) {
    const std::size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

// Runs a command line, streaming stdout through `sink` (binary) and stderr into a
// temporary file.  Returns the exit code, or -1 when the process could not be
// started (with `error` filled in).  Streaming avoids holding two copies of large
// frame buffers in memory.
using ByteSink = std::function<void(const std::uint8_t *, std::size_t)>;

int RunProcess(const std::string &command, const ByteSink &sink, std::string &error) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &attributes, 1 << 20)) {
        error = "CreatePipe failed";
        return -1;
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    wchar_t temp_path[MAX_PATH] = {};
    wchar_t temp_file[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp_path);
    GetTempFileNameW(temp_path, L"bmt", 0, temp_file);
    HANDLE error_file = CreateFileW(temp_file, GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE null_input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &attributes, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_pipe;
    startup.hStdError = error_file;
    startup.hStdInput = null_input;

    PROCESS_INFORMATION process{};
    std::wstring wide_command = Widen(command);
    const BOOL started = CreateProcessW(nullptr, wide_command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    // Parent must close its copy of the write end, otherwise the read never ends.
    CloseHandle(write_pipe);
    CloseHandle(error_file);
    CloseHandle(null_input);

    if (!started) {
        CloseHandle(read_pipe);
        DeleteFileW(temp_file);
        error = "cannot start " + command.substr(0, command.find(' '));
        return -1;
    }

    std::uint8_t buffer[1 << 16];
    while (true) {
        DWORD available = 0;
        if (!PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr)) {
            break;
        }
        if (available == 0) {
            const DWORD wait = WaitForSingleObject(process.hProcess, 0);
            if (wait == WAIT_OBJECT_0) {
                // Drain anything left, then stop.
                DWORD read = 0;
                if (!ReadFile(read_pipe, buffer, sizeof(buffer), &read, nullptr) || read == 0) {
                    break;
                }
                sink(buffer, read);
                continue;
            }
            Sleep(1);
            continue;
        }
        DWORD read = 0;
        const DWORD wanted = std::min<DWORD>(available, sizeof(buffer));
        if (!ReadFile(read_pipe, buffer, wanted, &read, nullptr) || read == 0) {
            break;
        }
        sink(buffer, read);
    }

    WaitForSingleObject(process.hProcess, 120000);
    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(read_pipe);

    if (exit_code != 0) {
        std::string text;
        HANDLE file = CreateFileW(temp_file, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            char chunk[4096];
            DWORD read = 0;
            while (ReadFile(file, chunk, sizeof(chunk) - 1, &read, nullptr) && read > 0) {
                chunk[read] = '\0';
                text += chunk;
                if (text.size() > 4000) {
                    break;
                }
            }
            CloseHandle(file);
        }
        if (text.empty()) {
            text = "exit code " + std::to_string(exit_code);
        }
        error = text;
    }
    DeleteFileW(temp_file);
    return static_cast<int>(exit_code);
}

bool RunCaptureText(const std::string &command, std::string &text, std::string &error) {
    text.clear();
    const int code = RunProcess(
        command,
        [&text](const std::uint8_t *data, std::size_t size) {
            text.append(reinterpret_cast<const char *>(data), size);
        },
        error);
    return code == 0;
}

std::string Trim(const std::string &value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

}  // namespace

bool ProbeVideo(const std::string &ffmpeg_path, const std::string &video_path,
                VideoMetadata &metadata, std::string &error) {
    const std::string ffprobe = DirectoryOf(ffmpeg_path) + "ffprobe.exe";
    const std::string command =
        Quote(ffprobe) +
        " -v error -select_streams v:0 -show_entries "
        "stream=width,height,r_frame_rate,nb_frames -of default=noprint_wrappers=1 " +
        Quote(video_path);
    std::string output;
    if (!RunCaptureText(command, output, error)) {
        error = "ffprobe failed: " + error;
        return false;
    }
    std::string frame_rate;
    std::size_t position = 0;
    while (position < output.size()) {
        const std::size_t end = output.find('\n', position);
        const std::string line = Trim(output.substr(position, end - position));
        position = end == std::string::npos ? output.size() : end + 1;
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        if (key == "width") {
            metadata.width = std::atoi(value.c_str());
        } else if (key == "height") {
            metadata.height = std::atoi(value.c_str());
        } else if (key == "nb_frames") {
            metadata.frame_count = std::atoll(value.c_str());
        } else if (key == "r_frame_rate") {
            frame_rate = value;
        }
    }
    if (!frame_rate.empty()) {
        const std::size_t slash = frame_rate.find('/');
        if (slash != std::string::npos) {
            const double numerator = std::atof(frame_rate.substr(0, slash).c_str());
            const double denominator = std::atof(frame_rate.substr(slash + 1).c_str());
            if (denominator > 0.0) {
                metadata.fps = numerator / denominator;
            }
        } else {
            metadata.fps = std::atof(frame_rate.c_str());
        }
    }
    if (metadata.width <= 0 || metadata.height <= 0 || metadata.fps <= 0.0) {
        error = "invalid video metadata";
        return false;
    }
    return true;
}

bool RunCommand(const std::string &executable, const std::vector<std::string> &arguments,
                std::string &error) {
    std::string command = Quote(executable);
    for (const std::string &argument : arguments) {
        command += " ";
        // Values that may contain spaces or Chinese characters are quoted; plain
        // flags stay unquoted so ffmpeg parses them as options.
        const bool needs_quotes = argument.find(' ') != std::string::npos ||
                                  argument.find('\\') != std::string::npos ||
                                  argument.find('/') != std::string::npos;
        command += needs_quotes ? Quote(argument) : argument;
    }
    const int code = RunProcess(command, [](const std::uint8_t *, std::size_t) {}, error);
    if (code != 0) {
        error = "command failed: " + error;
        return false;
    }
    return true;
}

bool ReadFramesBgr24(const std::string &ffmpeg_path, const std::string &video_path,
                     std::int64_t start_frame, std::int64_t count, int width, int height,
                     double fps, std::vector<std::uint8_t> &out, std::string &error) {
    if (count <= 0) {
        out.clear();
        return true;
    }
    if (fps <= 0.0) {
        error = "fps must be positive to convert a frame index into a seek timestamp";
        return false;
    }
    char seek[64];
    std::snprintf(seek, sizeof(seek), "%.6f", static_cast<double>(start_frame) / fps);
    const std::string command = Quote(ffmpeg_path) +
                                " -hide_banner -loglevel error -nostdin -ss " + seek + " -i " +
                                Quote(video_path) + " -frames:v " + std::to_string(count) +
                                " -f rawvideo -pix_fmt bgr24 -";
    const std::size_t expected = static_cast<std::size_t>(count) * static_cast<std::size_t>(width) *
                                 static_cast<std::size_t>(height) * 3;
    out.assign(expected, 0);
    std::size_t written = 0;
    bool overflow = false;
    const int code = RunProcess(
        command,
        [&](const std::uint8_t *data, std::size_t size) {
            const std::size_t room = expected - std::min(written, expected);
            const std::size_t take = std::min(room, size);
            if (take > 0) {
                std::memcpy(out.data() + written, data, take);
                written += take;
            }
            if (take < size) {
                overflow = true;
            }
        },
        error);
    if (code != 0) {
        error = "ffmpeg decode failed: " + error;
        return false;
    }
    if (overflow || written < expected) {
        error = "ffmpeg produced " + std::to_string(written) + " bytes, expected " +
                std::to_string(expected);
        return false;
    }
    return true;
}

}  // namespace badminton::inference
