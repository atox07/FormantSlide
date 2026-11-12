cmake_minimum_required(VERSION 3.24)
project(FormantSlideNative VERSION 0.9.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "FormantSlide native target requires Windows + MSVC.")
endif()
set(FL_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/SDK" CACHE PATH "Private, local FL Studio SDK directory")
if(NOT EXISTS "${FL_SDK_DIR}/fp_plugclass.h")
    message(FATAL_ERROR "FL Studio SDK missing. Set FL_SDK_DIR to your private local SDK directory.")
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

# WORLD sources are vendored unmodified; see third_party/World/UPSTREAM.txt.
file(GLOB WORLD_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/World/src/*.cpp")
add_library(formantslide_world STATIC ${WORLD_SOURCES})
target_include_directories(formantslide_world PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/World/src")
target_compile_definitions(formantslide_world PRIVATE NOMINMAX _USE_MATH_DEFINES)

add_subdirectory("${FL_SDK_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/fl_sdk")
add_library(FormantSlideNative SHARED
    Source/FormantSlideNative.h Source/FormantSlideNative.cpp
    Source/WorldEngine.h Source/WorldEngine.cpp
    Source/WaveLoader.h Source/WaveLoader.cpp
    Source/MidiFile.h Source/MidiFile.cpp
    Source/WinEditor.h Source/WinEditor.cpp)

target_include_directories(FormantSlideNative PRIVATE
    "${FL_SDK_DIR}")

target_compile_definitions(FormantSlideNative PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=1 JUCE_REPORT_APP_USAGE=1
    JUCE_PLUGINHOST_VST=0 JUCE_PLUGINHOST_VST3=0 JUCE_PLUGINHOST_AU=0
    JUCE_PLUGINHOST_LV2=0 JUCE_PLUGINHOST_ARA=0 JUCE_USE_LAME_AUDIO_FORMAT=0)

target_link_libraries(FormantSlideNative PRIVATE
    fl_sdk formantslide_world
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    user32 gdi32 comdlg32 shell32 ole32)

# FL Studio native generator naming convention. Do not install it in VST3.
set_target_properties(FormantSlideNative PROPERTIES
    OUTPUT_NAME "FormantSlide_x64"
    PREFIX "")

# Licence selection is governed by the JUCE 8 EULA, not by preprocessor flags.
# Starter is free only while its eligibility conditions are met.
add_custom_command(TARGET FormantSlideNative POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_LICENSES.txt"
        "$<TARGET_FILE_DIR:FormantSlideNative>/THIRD_PARTY_LICENSES.txt"
    VERBATIM)
install(TARGETS FormantSlideNative RUNTIME DESTINATION . LIBRARY DESTINATION .)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_LICENSES.txt" DESTINATION .)
