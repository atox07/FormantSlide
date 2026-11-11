cmake_minimum_required(VERSION 3.24)
project(FormantSlideNative VERSION 0.5.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "FormantSlide native target requires Windows + MSVC (GitHub Actions windows-2022).")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/SDK/fp_plugclass.h")
    message(FATAL_ERROR "SDK/ missing in the repository root. Keep the SDK/ folder from your existing branch.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)

# Identical JUCE version to the working v0.2 VST3 editor.
FetchContent_Declare(juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.10
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(juce)

FetchContent_Declare(signalsmith_stretch
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
    GIT_TAG main
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(signalsmith_stretch)

add_subdirectory(SDK)
add_library(FormantSlideNative SHARED
    Source/FormantSlideNative.h Source/FormantSlideNative.cpp
    Source/WaveLoader.h Source/WaveLoader.cpp
    Source/MidiFile.h Source/MidiFile.cpp
    Source/WinEditor.h Source/WinEditor.cpp)

target_include_directories(FormantSlideNative PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/SDK"
    "${signalsmith_stretch_SOURCE_DIR}")

target_compile_definitions(FormantSlideNative PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(FormantSlideNative PRIVATE
    fl_sdk signalsmith-stretch
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    user32 gdi32 comdlg32 shell32 ole32)

# FL Studio native generator naming convention. Do not install it in VST3.
set_target_properties(FormantSlideNative PROPERTIES
    OUTPUT_NAME "FormantSlide_x64"
    PREFIX "")
