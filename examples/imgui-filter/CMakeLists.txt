# ImGui Filter: a state-variable filter whose interface is Dear ImGui on WebGPU.
#
#   cmake -B build && cmake --build build     CLAP, VST3, AU (macOS), standalone
#   WCLAP: see README.md for the one extra flag (the WASI SDK toolchain).
cmake_minimum_required(VERSION 3.28)

# macOS plugins here are universal (Apple silicon and Intel).
if(CMAKE_HOST_APPLE AND NOT CMAKE_SYSTEM_NAME AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()

project(ImGuiFilter VERSION 1.0.0 LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJC OBJCXX)
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC) # clap-wrapper and the VST3 SDK use the DLL runtime (/MD)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux") # VST3 modules link static libraries
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

set(LIBS "${CMAKE_CURRENT_SOURCE_DIR}/../../libs")
include(${LIBS}/gpu/gpu.cmake)

# Dear ImGui (MIT) and its WebGPU renderer. It has no CMakeLists: compile its files.
include(FetchContent)
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz
    URL_HASH SHA256=21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99)
FetchContent_MakeAvailable(imgui)

# The interface, native and in the WCLAP page.
function(add_view target)
    target_sources(${target} PRIVATE ui/FilterView.cpp
        ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_wgpu.cpp)
    target_include_directories(${target} PRIVATE ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    if(APPLE AND NOT EMSCRIPTEN)
        # ImGui's WebGPU backend: Dawn natively (Emscripten picks it itself); on
        # macOS it includes Cocoa, so it compiles as Objective-C++.
        target_compile_definitions(${target} PRIVATE IMGUI_IMPL_WEBGPU_BACKEND_DAWN)
        set_source_files_properties(${imgui_SOURCE_DIR}/backends/imgui_impl_wgpu.cpp
            TARGET_DIRECTORY ${target} PROPERTIES COMPILE_OPTIONS "-xobjective-c++")
    elseif(NOT EMSCRIPTEN)
        target_compile_definitions(${target} PRIVATE IMGUI_IMPL_WEBGPU_BACKEND_DAWN)
    endif()
endfunction()

# WCLAP builds this file a second time with Emscripten, for the page the host shows.
if(GPU_PAGE)
    gpu_add_page(page ASSETS ui/assets)
    add_view(page)
    return()
endif()

include(${LIBS}/core/core.cmake)

add_library(ImGuiFilter_impl STATIC plugin.cpp)
core_add_to(ImGuiFilter_impl)
gpu_add_to(ImGuiFilter_impl)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
    add_view(ImGuiFilter_impl)
endif()

set(formats CLAP VST3)
if(APPLE)
    list(APPEND formats AUV2)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
    set(formats WCLAP)
endif()

make_clapfirst_plugins(
    TARGET_NAME ImGuiFilter
    IMPL_TARGET ImGuiFilter_impl
    ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/entry.cpp"
    OUTPUT_NAME "ImGui Filter"
    BUNDLE_IDENTIFIER "com.charlieculbert.imguifilter"
    BUNDLE_VERSION "${PROJECT_VERSION}"
    AUV2_MANUFACTURER_NAME "Charlie Culbert"
    AUV2_MANUFACTURER_CODE "ChCu"
    AUV2_SUBTYPE_CODE "ImFl"
    AUV2_INSTRUMENT_TYPE "aufx"
    PLUGIN_FORMATS ${formats}
    WINDOWS_FOLDER_VST3 TRUE # a folder, so ui/ can travel inside it
    STANDALONE_CONFIGURATIONS standalone "ImGui Filter" "com.charlieculbert.imguifilter"
    COPY_AFTER_BUILD FALSE)

# The files the interface reads (its font) go inside each plugin, as ui/. The
# WCLAP instead carries the page: the interface built with Emscripten.
if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
    gpu_wclap_page(page ImGuiFilter)
    core_ship_ui(ImGuiFilter "${page}" DEPENDS ImGuiFilter_page)
else()
    core_ship_ui(ImGuiFilter "${CMAKE_CURRENT_SOURCE_DIR}/ui/assets")
endif()
