# WebView Synth: a sine synth whose interface is a web page.
#
#   cmake -B build && cmake --build build     CLAP, VST3, AU (macOS), standalone
#   More formats, each with one extra flag (see README.md): WCLAP, AUv3 (macOS,
#   with -G Xcode), iOS (AUv3 and app), AAX (with your AAX SDK).
cmake_minimum_required(VERSION 3.28)

# macOS plugins here are universal (Apple silicon and Intel).
if(CMAKE_HOST_APPLE AND NOT CMAKE_SYSTEM_NAME AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0")
endif()

project(WebViewSynth VERSION 1.0.0 LANGUAGES C CXX)
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
include(${LIBS}/core/core.cmake)
include(${LIBS}/webview/webview.cmake)

add_library(WebViewSynth_impl STATIC plugin.cpp)
core_add_to(WebViewSynth_impl)
webview_add_to(WebViewSynth_impl)

# resources/ (the page in page/) ships as ordinary files inside each plugin.
webview_resources(resources WebViewSynth resources)

set(identity
    OUTPUT_NAME "WebView Synth"
    BUNDLE_IDENTIFIER "com.charlieculbert.webviewsynth"
    BUNDLE_VERSION "${PROJECT_VERSION}")

# iOS: an AUv3 extension inside an app (configure with -G Xcode -DCMAKE_SYSTEM_NAME=iOS).
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(WebViewSynth_auv3 entry.cpp)
    target_link_libraries(WebViewSynth_auv3 PRIVATE WebViewSynth_impl)
    target_add_auv3_wrapper(TARGET WebViewSynth_auv3 ${identity}
        MANUFACTURER_NAME "Charlie Culbert" MANUFACTURER_CODE "ChCu"
        SUBTYPE_CODE "WvSy" INSTRUMENT_TYPE "aumu")
    add_executable(WebViewSynth_ios entry.cpp)
    target_link_libraries(WebViewSynth_ios PRIVATE WebViewSynth_impl)
    target_add_auv3_standalone_ios_wrapper(TARGET WebViewSynth_ios
        OUTPUT_NAME "WebView Synth" BUNDLE_IDENTIFIER "com.charlieculbert.webviewsynth.ios"
        BUNDLE_VERSION "${PROJECT_VERSION}" AUV3_TARGET WebViewSynth_auv3
        AU_TYPE "aumu" AU_SUBTYPE "WvSy" AU_MANUFACTURER "ChCu")
    return()
endif()

set(formats CLAP VST3)
if(APPLE)
    list(APPEND formats AUV2)
    if(CMAKE_GENERATOR STREQUAL "Xcode") # AUv3 needs an app extension, which only Xcode builds
        list(APPEND formats AUV3)
    endif()
endif()
if(AAX_SDK_ROOT) # -DAAX_SDK_ROOT=<your AAX SDK>: not redistributable, so never fetched
    if(NOT EXISTS "${AAX_SDK_ROOT}/Interfaces/AAX.h")
        message(FATAL_ERROR "AAX_SDK_ROOT (${AAX_SDK_ROOT}) is not an AAX SDK")
    endif()
    list(APPEND formats AAX)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
    set(formats WCLAP)
endif()

make_clapfirst_plugins(
    TARGET_NAME WebViewSynth
    IMPL_TARGET WebViewSynth_impl
    ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/entry.cpp"
    ${identity}
    AUV2_MANUFACTURER_NAME "Charlie Culbert"
    AUV2_MANUFACTURER_CODE "ChCu"
    AUV2_SUBTYPE_CODE "WvSy"
    AUV2_INSTRUMENT_TYPE "aumu"
    PLUGIN_FORMATS ${formats}
    WINDOWS_FOLDER_VST3 TRUE # a folder, so its resources can travel inside it
    STANDALONE_CONFIGURATIONS standalone "WebView Synth" "com.charlieculbert.webviewsynth"
    COPY_AFTER_BUILD FALSE)

if(TARGET WebViewSynth_aax AND APPLE)
    # The pinned clap-wrapper misparses its empty AAX asset-output argument.
    set_target_properties(WebViewSynth_aax PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()

core_ship_resources(WebViewSynth "${resources}" DEPENDS WebViewSynth_resources)
