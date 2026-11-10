cmake_minimum_required(VERSION 3.24)
project(FormantSlide VERSION 0.2.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)

FetchContent_Declare(juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.10
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(juce)

# MIT-licensed time/pitch engine (with formant compensation).
FetchContent_Declare(signalsmith_stretch
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
    GIT_TAG main
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(signalsmith_stretch)

juce_add_plugin(FormantSlide
    COMPANY_NAME "FormantSlide"
    BUNDLE_ID "com.formantslide.plugin"
    PLUGIN_MANUFACTURER_CODE FmSl
    PLUGIN_CODE FmS1
    FORMATS VST3
    PRODUCT_NAME "FormantSlide"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)
juce_generate_juce_header(FormantSlide)

target_sources(FormantSlide PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(FormantSlide PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_include_directories(FormantSlide PRIVATE ${signalsmith_stretch_SOURCE_DIR})
target_link_libraries(FormantSlide PRIVATE
    signalsmith-stretch
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
