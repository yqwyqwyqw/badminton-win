cmake_minimum_required(VERSION 3.21)

# End-to-end verification of the C++ analysis path against the Python golden.
# Compiles the product kernel from app/src/inference/.  Dev-only target.
project(pipeline_e2e LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ORT_ROOT "" CACHE PATH "ONNX Runtime C++ SDK root (NuGet Microsoft.ML.OnnxRuntime.DirectML)")
set(DML_ROOT "" CACHE PATH "DirectML redist root (NuGet Microsoft.AI.DirectML)")
set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "repository root")

find_path(ORT_INCLUDE_DIR onnxruntime_cxx_api.h HINTS "${ORT_ROOT}/build/native/include" REQUIRED)
find_library(ORT_LIBRARY onnxruntime
    HINTS "${ORT_ROOT}/runtimes/win-x64/native" "${ORT_ROOT}/build/native/x64" REQUIRED)

set(KERNEL_SOURCES
    "${REPO_ROOT}/app/src/inference/cv_resize.cpp"
    "${REPO_ROOT}/app/src/inference/preprocess.cpp"
    "${REPO_ROOT}/app/src/inference/postprocess.cpp"
    "${REPO_ROOT}/app/src/inference/windowing.cpp"
    "${REPO_ROOT}/app/src/inference/rally_state_machine.cpp"
    "${REPO_ROOT}/app/src/inference/video_decoder.cpp"
    "${REPO_ROOT}/app/src/inference/tracknet_engine.cpp"
    "${REPO_ROOT}/app/src/inference/job_runner.cpp")

add_executable(pipeline_e2e main.cpp ${KERNEL_SOURCES})
target_include_directories(pipeline_e2e PRIVATE
    "${ORT_INCLUDE_DIR}"
    "${REPO_ROOT}/app/src/inference")
target_link_libraries(pipeline_e2e PRIVATE "${ORT_LIBRARY}")

add_custom_command(TARGET pipeline_e2e POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORT_ROOT}/runtimes/win-x64/native/onnxruntime.dll"
        "${ORT_ROOT}/runtimes/win-x64/native/onnxruntime_providers_shared.dll"
        "$<TARGET_FILE_DIR:pipeline_e2e>")
if(DML_ROOT)
    add_custom_command(TARGET pipeline_e2e POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DML_ROOT}/bin/x64-win/DirectML.dll"
            "$<TARGET_FILE_DIR:pipeline_e2e>")
endif()
