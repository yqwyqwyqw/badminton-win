cmake_minimum_required(VERSION 3.21)

# M2 step 1: C++ + ONNX Runtime (DirectML) smoke test against the frozen golden.
# Dev-only; the real inference kernel will live under app/src/inference/.
project(ort_cpp_smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ORT_ROOT "" CACHE PATH "ONNX Runtime C++ SDK root (NuGet Microsoft.ML.OnnxRuntime.DirectML)")
set(DML_ROOT "" CACHE PATH "DirectML redist root (NuGet Microsoft.AI.DirectML)")

find_path(ORT_INCLUDE_DIR onnxruntime_cxx_api.h
    HINTS "${ORT_ROOT}/build/native/include"
    REQUIRED)
find_library(ORT_LIBRARY onnxruntime
    HINTS "${ORT_ROOT}/runtimes/win-x64/native" "${ORT_ROOT}/build/native/x64"
    REQUIRED)

add_executable(ort_cpp_smoke main.cpp)
target_include_directories(ort_cpp_smoke PRIVATE "${ORT_INCLUDE_DIR}")
target_link_libraries(ort_cpp_smoke PRIVATE "${ORT_LIBRARY}")

# Ship the runtime next to the executable so the test runs from the build tree.
add_custom_command(TARGET ort_cpp_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORT_ROOT}/runtimes/win-x64/native/onnxruntime.dll"
        "${ORT_ROOT}/runtimes/win-x64/native/onnxruntime_providers_shared.dll"
        "$<TARGET_FILE_DIR:ort_cpp_smoke>")
if(DML_ROOT)
    add_custom_command(TARGET ort_cpp_smoke POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DML_ROOT}/bin/x64-win/DirectML.dll"
            "$<TARGET_FILE_DIR:ort_cpp_smoke>")
endif()
