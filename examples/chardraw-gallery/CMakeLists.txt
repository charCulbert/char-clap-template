# chardraw Gallery: every feature of the GPU interface (libs/gpu, libs/chardraw), one
# page each, for checking it in real hosts. CLAP, VST3 and a standalone app natively, and a WCLAP.
#
#   cmake -B build && cmake --build build
cmake_minimum_required(VERSION 3.28)

if(CMAKE_HOST_APPLE AND NOT CMAKE_SYSTEM_NAME AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()

project(ChardrawGallery VERSION 1.0.0 LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJC OBJCXX)
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

set(LIBS "${CMAKE_CURRENT_SOURCE_DIR}/../../libs")
include(${LIBS}/gpu/gpu.cmake)
include(${LIBS}/chardraw/chardraw.cmake)

# WCLAP builds this file a second time with Emscripten, for the page the host shows.
if(GPU_PAGE)
    gpu_add_page(page ASSETS resources) # its fonts and images, built into the page
    chardraw_add_to(page)
    target_sources(page PRIVATE ui/GalleryView.cpp)
    return()
endif()

include(${LIBS}/core/core.cmake)

add_library(ChardrawGallery_impl STATIC plugin.cpp)
core_add_to(ChardrawGallery_impl)
gpu_add_to(ChardrawGallery_impl)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "WASI") # a WCLAP's interface is the page, built with Emscripten
    chardraw_add_to(ChardrawGallery_impl)
    target_sources(ChardrawGallery_impl PRIVATE ui/GalleryView.cpp)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # GCC 13 at -O2 reports a write past the end of the 2-element
        # core::Value array of an initializer list in data(), from inside
        # std::map's move constructor (stl_tree.h:199). It isn't there: a
        # false positive on std::map and std::variant, so off for this file only.
        set_source_files_properties(ui/GalleryView.cpp PROPERTIES COMPILE_OPTIONS -Wno-array-bounds)
    endif()
endif()

set(formats CLAP VST3) # VST3 for the hosts without CLAP (Ableton Live)
if(APPLE)
    list(APPEND formats AUV2)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
    set(formats WCLAP)
endif()

make_clapfirst_plugins(
    TARGET_NAME ChardrawGallery
    IMPL_TARGET ChardrawGallery_impl
    ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/entry.cpp"
    OUTPUT_NAME "chardraw Gallery"
    BUNDLE_IDENTIFIER "com.charlieculbert.chardrawgallery"
    BUNDLE_VERSION "${PROJECT_VERSION}"
    AUV2_MANUFACTURER_NAME "Charlie Culbert"
    AUV2_MANUFACTURER_CODE "ChCu"
    AUV2_SUBTYPE_CODE "ChGa"
    AUV2_INSTRUMENT_TYPE "aufx"
    PLUGIN_FORMATS ${formats}
    WINDOWS_FOLDER_VST3 TRUE # a folder, so its resources can travel inside it
    STANDALONE_CONFIGURATIONS standalone "chardraw Gallery" "com.charlieculbert.chardrawgallery"
    COPY_AFTER_BUILD FALSE)

# Slide's fonts (https://github.com/charCulbert/slide): Barlow Semi Condensed
# for names, IBM Plex Mono for numbers.
if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
    gpu_wclap_resources(resources ChardrawGallery resources)
    core_ship_resources(ChardrawGallery "${resources}" DEPENDS ChardrawGallery_resources)
else()
    core_ship_resources(ChardrawGallery "${CMAKE_CURRENT_SOURCE_DIR}/resources")
endif()
