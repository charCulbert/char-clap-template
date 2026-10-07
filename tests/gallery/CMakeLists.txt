# GUI Gallery: every feature of the GPU interface (libs/gpu, libs/draw2d), one
# page each, for checking it in real hosts. Natively only, CLAP and standalone.
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
include(${LIBS}/core/core.cmake)

add_library(GuiGallery_impl STATIC plugin.cpp ui/GalleryView.cpp)
core_add_to(GuiGallery_impl)
gpu_add_to(GuiGallery_impl)
draw2d_add_to(GuiGallery_impl)

make_clapfirst_plugins(
    TARGET_NAME GuiGallery
    IMPL_TARGET GuiGallery_impl
    ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/entry.cpp"
    OUTPUT_NAME "GUI Gallery"
    BUNDLE_IDENTIFIER "com.charlieculbert.guigallery"
    BUNDLE_VERSION "${PROJECT_VERSION}"
    PLUGIN_FORMATS CLAP
    STANDALONE_CONFIGURATIONS standalone "GUI Gallery" "com.charlieculbert.guigallery"
    COPY_AFTER_BUILD FALSE)

# Slide's fonts: Barlow Semi Condensed for names, IBM Plex Mono for numbers.
core_ship_resources(GuiGallery "${CMAKE_CURRENT_SOURCE_DIR}/resources")
