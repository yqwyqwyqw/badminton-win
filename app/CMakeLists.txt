cmake_minimum_required(VERSION 3.21)

project(BadmintonAnalyzer VERSION 0.0.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.5 REQUIRED COMPONENTS Multimedia Quick QuickControls2)

qt_standard_project_setup(REQUIRES 6.5)
qt_policy(SET QTP0004 NEW)

# ONNX Runtime (DirectML build: DML + CPU execution providers in one package).
# Dev setup: unzip the NuGet packages under .tools/onnxruntime-cpp/ (see README).
set(ORT_ROOT "" CACHE PATH "ONNX Runtime C++ SDK root (NuGet Microsoft.ML.OnnxRuntime.DirectML)")
set(DML_ROOT "" CACHE PATH "DirectML redist root (NuGet Microsoft.AI.DirectML)")

find_path(ORT_INCLUDE_DIR onnxruntime_cxx_api.h
    HINTS "${ORT_ROOT}/build/native/include" "${CMAKE_SOURCE_DIR}/../.tools/onnxruntime-cpp/ort-dml/build/native/include")
find_library(ORT_LIBRARY onnxruntime
    HINTS "${ORT_ROOT}/runtimes/win-x64/native"
          "${ORT_ROOT}/build/native/x64"
          "${CMAKE_SOURCE_DIR}/../.tools/onnxruntime-cpp/ort-dml/runtimes/win-x64/native")
if(NOT ORT_INCLUDE_DIR OR NOT ORT_LIBRARY)
    message(FATAL_ERROR
        "ONNX Runtime SDK not found. Pass -DORT_ROOT=<...>/ort-dml (and -DDML_ROOT=<...>/directml).")
endif()

qt_add_executable(badminton-analyzer
    src/main.cpp
    src/TrialAnalysisController.cpp
    src/TrialAnalysisController.h
    src/VideoImportController.cpp
    src/VideoImportController.h
    src/inference/cv_resize.cpp
    src/inference/preprocess.cpp
    src/inference/postprocess.cpp
    src/inference/windowing.cpp
    src/inference/rally_state_machine.cpp
    src/inference/video_decoder.cpp
    src/inference/tracknet_engine.cpp
    src/inference/job_runner.cpp
    src/inference/inference_job.cpp
    src/inference/inference_job.h
)

set_source_files_properties(qml/Theme.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(badminton-analyzer
    URI BadmintonAnalyzer
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/components/AppHeader.qml
        qml/components/CutTrialPanel.qml
        qml/components/RallyAssemblyPanel.qml
        qml/components/RallyLibraryPanel.qml
        qml/components/TrialRallyPanel.qml
        qml/components/VideoExportPanel.qml
        qml/components/VideoImportPanel.qml
        qml/components/StepNavigation.qml
        qml/components/AnalysisStatusCard.qml
        qml/components/VideoPreview.qml
        qml/components/TimelinePlaceholder.qml
        qml/components/RallyListPanel.qml
)

target_include_directories(badminton-analyzer PRIVATE
    "${ORT_INCLUDE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/inference"
)

target_link_libraries(badminton-analyzer PRIVATE
    Qt6::Multimedia
    Qt6::Quick
    Qt6::QuickControls2
    "${ORT_LIBRARY}"
)

# Ship the runtime next to the executable so the app runs from the build tree.
add_custom_command(TARGET badminton-analyzer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORT_ROOT}/runtimes/win-x64/native/onnxruntime.dll"
        "${ORT_ROOT}/runtimes/win-x64/native/onnxruntime_providers_shared.dll"
        "$<TARGET_FILE_DIR:badminton-analyzer>")
if(DML_ROOT)
    add_custom_command(TARGET badminton-analyzer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DML_ROOT}/bin/x64-win/DirectML.dll"
            "$<TARGET_FILE_DIR:badminton-analyzer>")
endif()

set_target_properties(badminton-analyzer PROPERTIES
    WIN32_EXECUTABLE TRUE
)

install(TARGETS badminton-analyzer
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)
