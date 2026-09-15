// UTF-8 filesystem paths for the inference kernel.
//
// The kernel carries every path as a UTF-8 std::string (that is what the Qt
// layer hands over).  On Windows `std::filesystem::path`'s narrow constructor
// does NOT read those bytes as UTF-8: it decodes them with the active ANSI code
// page (936 on a Chinese system).  Feeding a UTF-8 path into `fs::path(...)`
// therefore silently produces a *different* path -- e.g. the product cache
// directory "羽毛球回合分析器" became the garbage directory
// "缇芥瘺鐞冨洖鍚堝垎鏋愬櫒" on disk, so the UI watched an empty directory while
// the inference kept writing somewhere else.
//
// Route every path through Utf8Path()/Utf8String() and the bytes are converted
// explicitly, independently of the process code page.

#pragma once

#include <filesystem>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace badminton::inference {

// UTF-8 -> native path.
inline std::filesystem::path Utf8Path(const std::string &utf8)
{
#ifdef _WIN32
    if (utf8.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                         nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
    return std::filesystem::path(wide);
#else
    return std::filesystem::path(utf8);
#endif
}

// Native path -> UTF-8 (the inverse; use it whenever a path leaves the kernel,
// for example inside a JSON file the QML layer reads back).
inline std::string Utf8String(const std::filesystem::path &path)
{
#ifdef _WIN32
    const std::wstring wide = path.native();
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), size,
                        nullptr, nullptr);
    return utf8;
#else
    return path.string();
#endif
}

// UTF-8 -> wide, for APIs that insist on wchar_t (ONNX Runtime on Windows).
inline std::wstring Utf8ToWide(const std::string &utf8)
{
#ifdef _WIN32
    return Utf8Path(utf8).native();
#else
    return std::wstring(utf8.begin(), utf8.end());
#endif
}

}  // namespace badminton::inference
