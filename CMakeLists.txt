cmake_minimum_required(VERSION 3.22)

# Link the C++ runtime statically so MiniDAW.exe runs on a PC without the Visual C++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(MiniDAW VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(MiniDAW PRODUCT_NAME "MiniDAW")

target_sources(MiniDAW PRIVATE
    Source/Main.cpp
    Source/Engine.cpp
    Source/Synth.cpp
    Source/Fx.cpp
    Source/FxPanel.cpp
    Source/DrumKit.cpp
    Source/DrumSequencer.cpp
    Source/PluginHost.cpp
    Source/PluginPanel.cpp
    Source/PianoRoll.cpp
    Source/MainComponent.cpp)

target_compile_definitions(MiniDAW PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:MiniDAW,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:MiniDAW,JUCE_VERSION>")

target_link_libraries(MiniDAW
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_audio_devices
        juce::juce_audio_processors
        juce::juce_gui_basics
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
