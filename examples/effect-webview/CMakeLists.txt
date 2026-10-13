# Effect WebView: stereo gain and pan with a peak meter, its interface a web page.
#
#   cmake -B build && cmake --build build     CLAP, VST3, AU (macOS), standalone
#   More formats, each with one extra flag (see README.md): WCLAP, and the iOS
#   app with its AUv3.
cmake_minimum_required(VERSION 3.28)

# macOS plugins here are universal (Apple silicon and Intel).
if(CMAKE_HOST_APPLE AND NOT CMAKE_SYSTEM_NAME AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0")
endif()

project(EffectWebView VERSION 1.0.0 LANGUAGES C CXX)
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

add_library(EffectWebView_impl STATIC plugin.cpp)
core_add_to(EffectWebView_impl)
webview_add_to(EffectWebView_impl)

# resources/ (the page in page/) ships as ordinary files inside each plugin.
webview_resources(resources EffectWebView resources)

set(identity
    OUTPUT_NAME "Effect WebView"
    BUNDLE_IDENTIFIER "com.charlieculbert.effectwebview"
    BUNDLE_VERSION "${PROJECT_VERSION}")

# iOS: an AUv3 extension inside an app (configure with -G Xcode -DCMAKE_SYSTEM_NAME=iOS).
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(EffectWebView_auv3 entry.cpp)
    target_link_libraries(EffectWebView_auv3 PRIVATE EffectWebView_impl)
    target_add_auv3_wrapper(TARGET EffectWebView_auv3 ${identity}
        MANUFACTURER_NAME "Charlie Culbert" MANUFACTURER_CODE "ChCu"
        SUBTYPE_CODE "EfWv" INSTRUMENT_TYPE "aufx")
    add_executable(EffectWebView_ios entry.cpp)
    target_link_libraries(EffectWebView_ios PRIVATE EffectWebView_impl)
    target_add_auv3_standalone_ios_wrapper(TARGET EffectWebView_ios
        OUTPUT_NAME "Effect WebView" BUNDLE_IDENTIFIER "com.charlieculbert.effectwebview.ios"
        BUNDLE_VERSION "${PROJECT_VERSION}" AUV3_TARGET EffectWebView_auv3
        AU_TYPE "aufx" AU_SUBTYPE "EfWv" AU_MANUFACTURER "ChCu")
    return()
endif()

set(formats CLAP VST3)
if(APPLE)
    list(APPEND formats AUV2)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
    set(formats WCLAP)
endif()

make_clapfirst_plugins(
    TARGET_NAME EffectWebView
    IMPL_TARGET EffectWebView_impl
    ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/entry.cpp"
    ${identity}
    AUV2_MANUFACTURER_NAME "Charlie Culbert"
    AUV2_MANUFACTURER_CODE "ChCu"
    AUV2_SUBTYPE_CODE "EfWv"
    AUV2_INSTRUMENT_TYPE "aufx"
    PLUGIN_FORMATS ${formats}
    WINDOWS_FOLDER_VST3 TRUE # a folder, so its resources can travel inside it
    STANDALONE_CONFIGURATIONS standalone "Effect WebView" "com.charlieculbert.effectwebview"
    COPY_AFTER_BUILD FALSE)

core_ship_resources(EffectWebView "${resources}" DEPENDS EffectWebView_resources)
