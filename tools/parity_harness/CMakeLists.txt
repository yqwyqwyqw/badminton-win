cmake_minimum_required(VERSION 3.21)

# Golden parity harness for the C++ inference kernel (M2 steps 1-3).
# Compiles the *product* sources from app/src/inference/ so the tests exercise the
# same code the application will use.  Dev-only target.
project(parity_harness LANGUAGES CXX)

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
    "${REPO_ROOT}/app/src/inference/tracknet_engine.cpp")

add_executable(parity_harness main.cpp ${KERNEL_SOURCES})
target_include_directories(parity_harness PRIVATE
    "${ORT_INCLUDE_DIR}"
    "${REPO_ROOT}/app/src/inference")
target_link_libraries(parity_harness PRIVATE "${ORT_LIBRARY}")

add_custom_command(TARGET parity_harness POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORT_ROOT}/runtimes/win-x64/native/onnxruntime.dll"
        "${ORT_ROOT}/runtimes/win-x64/native/onnxruntime_providers_shared.dll"
        "$<TARGET_FILE_DIR:parity_harness>")
if(DML_ROOT)
    add_custom_command(TARGET parity_harness POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DML_ROOT}/bin/x64-win/DirectML.dll"
            "$<TARGET_FILE_DIR:parity_harness>")
endif()
