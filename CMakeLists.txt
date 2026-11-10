cmake_minimum_required(VERSION 3.24)
project(FormantSlideNative VERSION 0.3.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "FL Studio Native SDK target requires Windows + MSVC. Use the Windows GitHub Actions workflow.")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/SDK/fp_plugclass.h")
    message(FATAL_ERROR "Missing SDK. Copy the SDK/ directory from your FL-Studio-SDK-CMake-Template-main ZIP to the repository root.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)

# The existing v0.2 also uses Signalsmith Stretch; this retains its formant engine.
FetchContent_Declare(signalsmith_stretch
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
    GIT_TAG main
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(signalsmith_stretch)

add_subdirectory(SDK)

add_library(FormantSlideNative SHARED
    Source/FormantSlideNative.h
    Source/FormantSlideNative.cpp
    Source/MidiFile.h
    Source/MidiFile.cpp
    Source/WaveLoader.h
    Source/WaveLoader.cpp
    Source/WinEditor.h
    Source/WinEditor.cpp)

target_include_directories(FormantSlideNative PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/SDK"
    "${signalsmith_stretch_SOURCE_DIR}")

target_compile_definitions(FormantSlideNative PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_link_libraries(FormantSlideNative PRIVATE
    fl_sdk
    signalsmith-stretch
    user32
    gdi32
    comdlg32
    shell32
    ole32)

set_target_properties(FormantSlideNative PROPERTIES
    OUTPUT_NAME "FormantSlide"
    PREFIX "")
