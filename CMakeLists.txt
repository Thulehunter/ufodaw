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

juce_add_gui_app(MiniDAW PRODUCT_NAME "MiniDAW"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon.png")

target_sources(MiniDAW PRIVATE
    Source/Main.cpp
    Source/Engine.cpp
    Source/Theme.cpp
    Source/ExternalMidi.cpp
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

# ASIO drivers need Steinberg's ASIO SDK, which can't be bundled with the sources. Either unzip it into
# third_party/ (e.g. third_party/asiosdk_2.3.3_2019-06-14), or point ASIO_SDK_DIR (cmake -D or environment
# variable) at the folder that contains common/iasiodrv.h. See the README.
set(ASIO_SDK_DIR "$ENV{ASIO_SDK_DIR}" CACHE PATH "Steinberg ASIO SDK folder (contains common/iasiodrv.h); switches ASIO on")
if(NOT ASIO_SDK_DIR)
    file(GLOB _asio_candidates LIST_DIRECTORIES true
         "${CMAKE_SOURCE_DIR}/third_party/asiosdk*" "${CMAKE_SOURCE_DIR}/third_party/ASIOSDK*")
    foreach(_dir IN LISTS _asio_candidates)
        if(EXISTS "${_dir}/common/iasiodrv.h")
            set(ASIO_SDK_DIR "${_dir}")
        endif()
    endforeach()
endif()

if(ASIO_SDK_DIR AND EXISTS "${ASIO_SDK_DIR}/common/iasiodrv.h")
    message(STATUS "ASIO support: ON (SDK in ${ASIO_SDK_DIR})")
    target_compile_definitions(MiniDAW PRIVATE JUCE_ASIO=1)
    target_include_directories(MiniDAW PRIVATE "${ASIO_SDK_DIR}/common")
else()
    message(STATUS "ASIO support: off (set ASIO_SDK_DIR to the ASIO SDK folder to switch it on)")
endif()

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
