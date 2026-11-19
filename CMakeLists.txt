cmake_minimum_required(VERSION 3.24)
project(FormantSlideNative VERSION 0.9.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "FormantSlide native target requires Windows + MSVC.")
endif()
set(FL_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/SDK" CACHE PATH "Private, local FL Studio SDK directory")
if(NOT EXISTS "${FL_SDK_DIR}/fp_plugclass.h")
    message(FATAL_ERROR "FL Studio SDK missing. Set FL_SDK_DIR to your private local SDK directory.")
endif()

set(CMAKE_CXX_STANDARD 20)
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
    GIT_TAG 222093b4cc13ddb4d07c826bc3c1559326091731
    GIT_SUBMODULES "")
FetchContent_MakeAvailable(signalsmith_stretch)

add_subdirectory("${FL_SDK_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/fl_sdk")
# Legacy SDK hint tables assign literals to char*. Scope compatibility to its .cpp files.
target_compile_options(fl_sdk PRIVATE /Zc:strictStrings-)

add_library(FormantSlideNative SHARED
    Source/FormantSlideNative.h Source/FormantSlideNative.cpp
    Source/SlideVoice.h Source/SlideVoice.cpp
    Source/WaveLoader.h Source/WaveLoader.cpp
    Source/MidiFile.h Source/MidiFile.cpp
    Source/WinEditor.h Source/WinEditor.cpp)

add_executable(FormantSlideAudioCheck EXCLUDE_FROM_ALL
    Tests/SlideAudio.cpp Source/SlideVoice.cpp Source/WaveLoader.cpp)
target_include_directories(FormantSlideAudioCheck PRIVATE Source)
target_link_libraries(FormantSlideAudioCheck PRIVATE signalsmith-stretch)
target_compile_options(FormantSlideAudioCheck PRIVATE /bigobj)
target_compile_options(FormantSlideNative PRIVATE /bigobj)

target_include_directories(FormantSlideNative PRIVATE "${FL_SDK_DIR}")

target_compile_definitions(FormantSlideNative PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=1 JUCE_REPORT_APP_USAGE=1
    JUCE_PLUGINHOST_VST=0 JUCE_PLUGINHOST_VST3=0 JUCE_PLUGINHOST_AU=0
    JUCE_PLUGINHOST_LV2=0 JUCE_PLUGINHOST_ARA=0 JUCE_USE_LAME_AUDIO_FORMAT=0)

target_link_libraries(FormantSlideNative PRIVATE
    fl_sdk signalsmith-stretch
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    user32 gdi32 comdlg32 shell32 ole32)

# FL Studio native generator naming convention. Do not install it in VST3.
set_target_properties(FormantSlideNative PROPERTIES
    OUTPUT_NAME "FormantSlide_x64"
    PREFIX "")

# Licence selection is governed by the JUCE 8 EULA, not by preprocessor flags.
# Starter is free only while its eligibility conditions are met.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_LICENSES.txt" project_notices)
file(READ "${signalsmith_stretch_SOURCE_DIR}/LICENSE.txt" stretch_license)
get_target_property(linear_source signalsmith-linear SOURCE_DIR)
file(READ "${linear_source}/LICENSE.txt" linear_license)
set(notices "${CMAKE_CURRENT_BINARY_DIR}/THIRD_PARTY_LICENSES.txt")
file(WRITE "${notices}" "${project_notices}\nSignalsmith Stretch\n${stretch_license}\nSignalsmith Linear\n${linear_license}\n")
add_custom_command(TARGET FormantSlideNative POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${notices}"
        "$<TARGET_FILE_DIR:FormantSlideNative>/THIRD_PARTY_LICENSES.txt"
    VERBATIM)
install(TARGETS FormantSlideNative RUNTIME DESTINATION . LIBRARY DESTINATION .)
install(FILES "${notices}" DESTINATION .)
