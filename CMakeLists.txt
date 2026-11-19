cmake_minimum_required(VERSION 3.22)

project(TCSpectrumAnalyzer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()

# JUCE: use a local checkout if JUCE_DIR is given, otherwise download it.
set(JUCE_DIR "" CACHE PATH "Path to a local JUCE checkout")
if(JUCE_DIR)
    add_subdirectory(${JUCE_DIR} JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.17-security
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_plugin(TCSpectrumAnalyzer
    COMPANY_NAME "TC"
    COMPANY_WEBSITE "https://github.com/Hymmoth/TC-Spectrum-Analyzer"
    BUNDLE_ID "com.tc.spectrumanalyzer"
    PLUGIN_MANUFACTURER_CODE Tcsa
    PLUGIN_CODE Tcsa
    FORMATS VST3
    PRODUCT_NAME "TC Spectrum Analyzer"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    VST3_CATEGORIES "Fx Analyzer"
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(TCSpectrumAnalyzer PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/Analyzer.cpp
    Source/SpectrumView.cpp
    Source/MeterView.cpp)

target_compile_definitions(TCSpectrumAnalyzer PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(TCSpectrumAnalyzer
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
