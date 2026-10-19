cmake_minimum_required(VERSION 3.22)

# Link the C++ runtime statically so UFODAW.exe runs on a PC without the Visual C++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(UFODAW VERSION 0.9.3.1 LANGUAGES C CXX)

# /EHa: catch(...) also catches access violations, so a crash in a plugin's window (or another callback from the message loop) is caught by JUCE's
# loop and handled by UFODAWApplication::unhandledException instead of ending the program.
if(MSVC)
    string(REPLACE "/EHsc" "/EHa" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

# (JUCE's recommended flags add /EHsc themselves, after ours: make that /EHa as well, once JUCE is available)
function(ufodaw_use_async_exceptions)
    if(NOT MSVC)
        return()
    endif()
    get_target_property(opts juce_recommended_config_flags INTERFACE_COMPILE_OPTIONS)
    if(opts)
        string(REPLACE "/EHsc" "/EHa" opts "${opts}")
        set_property(TARGET juce_recommended_config_flags PROPERTY INTERFACE_COMPILE_OPTIONS "${opts}")
    endif()
endfunction()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(JUCE)

# Rubber Band: time-stretching and pitch-shifting of audio clips. Its single-file build (no separate library, built-in FFT)
# is compiled into the program. GPL: see the README.
FetchContent_Declare(rubberband
    GIT_REPOSITORY https://github.com/breakfastquay/rubberband.git
    GIT_TAG v4.0.0
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(rubberband)

# Shine: a small fixed-point MP3 encoder (LGPL), compiled into the program for "Export > MP3". (It has no CMake build of its own: its
# C sources are added below.)
FetchContent_Declare(shine
    GIT_REPOSITORY https://github.com/toots/shine.git
    GIT_TAG ab5e3526b64af1a2eaa43aa6f441a7312e013519
    GIT_SHALLOW OFF)
FetchContent_MakeAvailable(shine)

# Lua 5.4: the scripting language of UFODAW (small, fast, easy to keep in a sandbox). Compiled from source into a static library.
FetchContent_Declare(lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG v5.4.7
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(lua)
file(GLOB LUA_SOURCES "${lua_SOURCE_DIR}/*.c")
list(FILTER LUA_SOURCES EXCLUDE REGEX "/(lua|luac|onelua|ltests)\.c$")
add_library(lua_core STATIC ${LUA_SOURCES})
target_include_directories(lua_core PUBLIC "${lua_SOURCE_DIR}")
set_target_properties(lua_core PROPERTIES LINKER_LANGUAGE C)
set(SHINE_SOURCES
    "${shine_SOURCE_DIR}/src/lib/bitstream.c" "${shine_SOURCE_DIR}/src/lib/huffman.c" "${shine_SOURCE_DIR}/src/lib/l3bitstream.c"
    "${shine_SOURCE_DIR}/src/lib/l3loop.c" "${shine_SOURCE_DIR}/src/lib/l3mdct.c" "${shine_SOURCE_DIR}/src/lib/l3subband.c"
    "${shine_SOURCE_DIR}/src/lib/layer3.c" "${shine_SOURCE_DIR}/src/lib/reservoir.c" "${shine_SOURCE_DIR}/src/lib/tables.c")

juce_add_gui_app(UFODAW PRODUCT_NAME "UFODAW"
    VERSION "${PROJECT_VERSION}"
    BUNDLE_ID "io.github.thulehunter.ufodaw"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "UFODAW needs the audio input to record."
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon.png")

target_sources(UFODAW PRIVATE
    Source/Main.cpp
    Source/Engine.cpp
    Source/EngineLauncher.cpp
    Source/Scripting.cpp
    Source/Theme.cpp
    Source/ExternalMidi.cpp
    Source/Tuner.cpp
    Source/Synth.cpp
    Source/Fx.cpp
    Source/FxPanel.cpp
    Source/DrumKit.cpp
    Source/DrumSequencer.cpp
    Source/PluginHost.cpp
    Source/PluginPanel.cpp
    Source/PianoRoll.cpp
    Source/Stretch.cpp
    Source/Sampler.cpp
    Source/StarterPack.cpp
    Source/ProjectTemplates.cpp
    Source/AudioExport.cpp
    ${SHINE_SOURCES}
    Source/MainComponent.cpp
    "${rubberband_SOURCE_DIR}/single/RubberBandSingle.cpp")
target_include_directories(UFODAW PRIVATE "${rubberband_SOURCE_DIR}" "${shine_SOURCE_DIR}/src/lib")
if(MSVC)   # (the C sources of Shine use GCC's __attribute__: a forced include takes it away)
    set_source_files_properties(${SHINE_SOURCES} PROPERTIES COMPILE_FLAGS "/FI\"${CMAKE_SOURCE_DIR}/Source/ShineCompat.h\"")
endif()
# (Windows' min/max macros would break std::numeric_limits<...>::max() in Rubber Band)
set_source_files_properties("${rubberband_SOURCE_DIR}/single/RubberBandSingle.cpp" PROPERTIES COMPILE_DEFINITIONS "NOMINMAX")

target_compile_definitions(UFODAW PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_CATCH_UNHANDLED_EXCEPTIONS=1
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:UFODAW,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:UFODAW,JUCE_VERSION>")
if(APPLE)   # on a Mac the Audio Units that are installed can be hosted, next to VST3
    target_compile_definitions(UFODAW PRIVATE JUCE_PLUGINHOST_AU=1)
endif()

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
    target_compile_definitions(UFODAW PRIVATE JUCE_ASIO=1)
    target_include_directories(UFODAW PRIVATE "${ASIO_SDK_DIR}/common")
else()
    message(STATUS "ASIO support: off (set ASIO_SDK_DIR to the ASIO SDK folder to switch it on)")
endif()

ufodaw_use_async_exceptions()

target_link_libraries(UFODAW
    PRIVATE
        lua_core
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_audio_devices
        juce::juce_audio_processors
        juce::juce_gui_basics
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Automatic tests (cmake -DUFODAW_BUILD_TESTS=ON): EngineTest renders a project serial and with worker threads and checks the results are identical.
option(UFODAW_BUILD_TESTS "Build the engine tests" OFF)
if(UFODAW_BUILD_TESTS)
    juce_add_console_app(EngineTest PRODUCT_NAME "EngineTest")
    target_sources(EngineTest PRIVATE
        tests/engine/EngineTest.cpp
        Source/Engine.cpp Source/EngineLauncher.cpp Source/ExternalMidi.cpp Source/Tuner.cpp Source/Synth.cpp Source/Fx.cpp Source/DrumKit.cpp
        Source/Stretch.cpp Source/Sampler.cpp Source/StarterPack.cpp Source/ProjectTemplates.cpp Source/AudioExport.cpp
        ${SHINE_SOURCES}
        "${rubberband_SOURCE_DIR}/single/RubberBandSingle.cpp")
    target_include_directories(EngineTest PRIVATE Source "${rubberband_SOURCE_DIR}" "${shine_SOURCE_DIR}/src/lib")
    target_compile_definitions(EngineTest PRIVATE ENGINE_HAS_THREADS=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_PLUGINHOST_VST3=1)
    target_link_libraries(EngineTest PRIVATE
        juce::juce_audio_formats juce::juce_audio_devices juce::juce_audio_processors juce::juce_gui_basics
        juce::juce_recommended_config_flags)
endif()
