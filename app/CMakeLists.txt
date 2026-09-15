cmake_minimum_required(VERSION 3.21)

project(BadmintonAnalyzer VERSION 0.0.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.5 REQUIRED COMPONENTS Multimedia Quick QuickControls2)

qt_standard_project_setup(REQUIRES 6.5)
qt_policy(SET QTP0004 NEW)

qt_add_executable(badminton-analyzer
    src/main.cpp
    src/TrialAnalysisController.cpp
    src/TrialAnalysisController.h
    src/VideoImportController.cpp
    src/VideoImportController.h
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
        qml/components/TrialRallyPanel.qml
        qml/components/VideoImportPanel.qml
        qml/components/StepNavigation.qml
        qml/components/AnalysisStatusCard.qml
        qml/components/VideoPreview.qml
        qml/components/TimelinePlaceholder.qml
        qml/components/RallyListPanel.qml
)

target_link_libraries(badminton-analyzer PRIVATE
    Qt6::Multimedia
    Qt6::Quick
    Qt6::QuickControls2
)

set_target_properties(badminton-analyzer PROPERTIES
    WIN32_EXECUTABLE TRUE
)

install(TARGETS badminton-analyzer
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)
