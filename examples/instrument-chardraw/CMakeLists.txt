# Instrument chardraw: a polyphonic saw synth with a lowpass filter and a peak
# meter, its interface drawn with chardraw on WebGPU.
#
#   cmake -B build && cmake --build build     CLAP, VST3, AU (macOS), standalone
#   WCLAP: see README.md for the one extra flag (the WASI SDK toolchain).
cmake_minimum_required(VERSION 3.28)

# macOS plugins here are universal (Apple silicon and Intel).
if(CMAKE_HOST_APPLE AND NOT CMAKE_SYSTEM_NAME AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()

project(InstrumentChardraw VERSION 1.0.0 LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJC OBJCXX)
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # build/compile_commands.json, where clangd finds the downloaded headers
if(MSVC) # clap-wrapper and the VST3 SDK use the DLL runtime (/MD)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux") # VST3 modules link static libraries
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

set(LIBS "${CMAKE_CURRENT_SOURCE_DIR}/../../libs")
include(${LIBS}/gpu/gpu.cmake)
include(${LIBS}/chardraw/chardraw.cmake)

# WCLAP builds this file a second time with Emscripten, for the page the host shows.
if(GPU_PAGE)
    gpu_add_page(page ASSETS resources) # the font, built into the page
    chardraw_add_to(page)
    target_sources(page PRIVATE ui/PluginView.cpp)
    return()
endif()

include(${LIBS}/core/core.cmake)

add_library(InstrumentChardraw_impl STATIC plugin.cpp)
core_add_to(InstrumentChardraw_impl)
gpu_add_to(InstrumentChardraw_impl)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "WASI") # a WCLAP's interface is the page, built with Emscripten
    chardraw_add_to(InstrumentChardraw_impl)
    target_sources(InstrumentChardraw_impl PRIVATE ui/PluginView.cpp)
endif()

set(formats CLAP VST3)
if(APPLE)
    list(APPEND formats AUV2)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
    set(formats WCLAP)
endif()

make_clapfirst_plugins(
    TARGET_NAME InstrumentChardraw
    IMPL_TARGET InstrumentChardraw_impl
    ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/entry.cpp"
    OUTPUT_NAME "Instrument chardraw"
    BUNDLE_IDENTIFIER "com.charlieculbert.instrumentchardraw"
    BUNDLE_VERSION "${PROJECT_VERSION}"
    AUV2_MANUFACTURER_NAME "Charlie Culbert"
    AUV2_MANUFACTURER_CODE "ChCu"
    AUV2_SUBTYPE_CODE "InCd"
    AUV2_INSTRUMENT_TYPE "aumu"
    PLUGIN_FORMATS ${formats}
    WINDOWS_FOLDER_VST3 TRUE # a folder, so its resources can travel inside it
    STANDALONE_CONFIGURATIONS standalone "Instrument chardraw" "com.charlieculbert.instrumentchardraw"
    COPY_AFTER_BUILD FALSE)

# resources/ (its font) ships inside each plugin. The WCLAP adds page/: the
# interface built with Emscripten, which the host shows.
if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
    gpu_wclap_resources(resources InstrumentChardraw resources)
    core_ship_resources(InstrumentChardraw "${resources}" DEPENDS InstrumentChardraw_resources)
else()
    core_ship_resources(InstrumentChardraw "${CMAKE_CURRENT_SOURCE_DIR}/resources")
endif()
