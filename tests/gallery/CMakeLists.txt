# GUI Gallery: every feature of the GPU interface (libs/gpu, libs/draw2d), one
# page each, for checking it in real hosts. CLAP and VST3 natively, and a WCLAP.
#
#   cmake -B build && cmake --build build
cmake_minimum_required(VERSION 3.28)

if(CMAKE_HOST_APPLE AND NOT CMAKE_SYSTEM_NAME AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()

project(GuiGallery VERSION 1.0.0 LANGUAGES C CXX)
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
include(${LIBS}/draw2d/draw2d.cmake)

# WCLAP builds this file a second time with Emscripten, for the page the host shows.
if(GPU_PAGE)
    gpu_add_page(page ASSETS resources) # its fonts and images, built into the page
    draw2d_add_to(page)
    target_sources(page PRIVATE ui/GalleryView.cpp)
    return()
endif()

include(${LIBS}/core/core.cmake)

add_library(GuiGallery_impl STATIC plugin.cpp)
core_add_to(GuiGallery_impl)
gpu_add_to(GuiGallery_impl)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "WASI") # a WCLAP's interface is the page, built with Emscripten
    draw2d_add_to(GuiGallery_impl)
    target_sources(GuiGallery_impl PRIVATE ui/GalleryView.cpp)
endif()

set(formats CLAP VST3) # VST3 for the hosts without CLAP (Ableton Live)
if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
    set(formats WCLAP)
endif()

make_clapfirst_plugins(
    TARGET_NAME GuiGallery
    IMPL_TARGET GuiGallery_impl
    ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/entry.cpp"
    OUTPUT_NAME "GUI Gallery"
    BUNDLE_IDENTIFIER "com.charlieculbert.guigallery"
    BUNDLE_VERSION "${PROJECT_VERSION}"
    PLUGIN_FORMATS ${formats}
    WINDOWS_FOLDER_VST3 TRUE # a folder, so its resources can travel inside it
    STANDALONE_CONFIGURATIONS standalone "GUI Gallery" "com.charlieculbert.guigallery"
    COPY_AFTER_BUILD FALSE)

# Slide's fonts: Barlow Semi Condensed for names, IBM Plex Mono for numbers.
if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
    gpu_wclap_resources(resources GuiGallery resources)
    core_ship_resources(GuiGallery "${resources}" DEPENDS GuiGallery_resources)
else()
    core_ship_resources(GuiGallery "${CMAKE_CURRENT_SOURCE_DIR}/resources")
endif()
