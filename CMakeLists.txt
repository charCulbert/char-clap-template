cmake_minimum_required(VERSION 3.28)

# =============================================================================
# Platform and compiler setup
# =============================================================================

# iOS uses its own SDK/architecture, supplied when configuring that build.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum iOS version")
    endif()
elseif((CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR
        (CMAKE_HOST_APPLE AND NOT CMAKE_SYSTEM_NAME))
       AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()

project(simpleCharClap LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJC OBJCXX)
endif()

# Keep clap-wrapper and the VST3 SDK on the same MSVC runtime (/MD).
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Linux VST3 shared modules link clap-wrapper and SDK static libraries.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# =============================================================================
# Dependencies: CLAP SDK and clap-wrapper
# =============================================================================

include(FetchContent)

# Fetch the CLAP SDK
FetchContent_Declare(
    clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG 1.2.10
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(clap)

# Fetch clap-wrapper and the VST3 SDK it needs.
set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES TRUE)
FetchContent_Declare(
    clap-wrapper
    GIT_REPOSITORY https://github.com/free-audio/clap-wrapper.git
    GIT_TAG 2bc329c774b4584918c2150dda918afd5e6b2b87
)
FetchContent_MakeAvailable(clap-wrapper)

# CHOC's WebView shows the web UI in hosts that don't provide a WebView
# themselves. Desktop only: WCLAP hosts always show the page.
if(NOT EMSCRIPTEN AND NOT CMAKE_SYSTEM_NAME MATCHES "WASI|iOS")
    set(SIMPLECHAR_NATIVE_WEBVIEW ON)
    FetchContent_Declare(
        choc
        GIT_REPOSITORY https://github.com/Tracktion/choc.git
        GIT_TAG 9606a6615b592605993d92733cdfd6ca6ecbd825
    )
    FetchContent_MakeAvailable(choc)
endif()

# =============================================================================
# Plugin identity and shared implementation
# =============================================================================

set(CLAP_NAME "simpleCharClap")
set(CLAP_BUNDLE_ID "com.charlieculbert.simplecharclap")

# All formats share this plugin implementation. webview/ is the reusable web
# UI plumbing; it knows nothing about this plugin.
add_library(simpleCharClap_impl STATIC plugin.cpp
    webview/gui.cpp webview/messages.cpp webview/resources.cpp)
target_include_directories(simpleCharClap_impl PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(simpleCharClap_impl PUBLIC clap)
if(SIMPLECHAR_NATIVE_WEBVIEW)
    target_compile_definitions(simpleCharClap_impl PRIVATE WEBVIEW_NATIVE=1)
    target_link_libraries(simpleCharClap_impl PRIVATE choc::choc)
    if(APPLE)
        target_sources(simpleCharClap_impl PRIVATE webview/native_mac.mm)
        set_source_files_properties(webview/native_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_link_libraries(simpleCharClap_impl PUBLIC "-framework AppKit" "-framework WebKit")
    elseif(WIN32)
        target_sources(simpleCharClap_impl PRIVATE webview/native_win.cpp)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(WEBKIT REQUIRED IMPORTED_TARGET gtk+-3.0 webkit2gtk-4.1)
        target_sources(simpleCharClap_impl PRIVATE webview/native_linux.cpp)
        target_link_libraries(simpleCharClap_impl PUBLIC PkgConfig::WEBKIT)
    endif()
endif()
set_target_properties(simpleCharClap_impl PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(simpleCharClap_impl PRIVATE /W4)
    # Keep the portable C string functions without MSVC-only replacements.
    target_compile_definitions(simpleCharClap_impl PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(simpleCharClap_impl PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# =============================================================================
# Tests for the web UI plumbing (cmake -DSIMPLECHAR_BUILD_TESTS=ON, then ctest)
# =============================================================================

option(SIMPLECHAR_BUILD_TESTS "Build the resource and message tests" OFF)
if(SIMPLECHAR_BUILD_TESTS)
    enable_testing()
    add_executable(resources_test tests/resources_test.cpp webview/resources.cpp)
    add_test(NAME resources
        COMMAND resources_test "${CMAKE_CURRENT_BINARY_DIR}/resources-test")
    add_executable(messages_test tests/messages_test.cpp webview/messages.cpp)
    add_test(NAME messages COMMAND messages_test)
    find_program(NODE node REQUIRED)
    add_test(NAME messages_js
        COMMAND "${NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/messages.test.mjs"
                "$<TARGET_FILE:messages_test>")
    # The tests are assert()s, so keep them in every build type.
    foreach(test resources_test messages_test)
        target_compile_options(${test} PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
    endforeach()
endif()

# =============================================================================
# iOS: AUv3 extension and containing app
# =============================================================================

# iOS packages the implementation inside an AUv3 extension and containing app.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(simpleCharClap_auv3 entry.cpp)
    target_link_libraries(simpleCharClap_auv3 PRIVATE simpleCharClap_impl)
    target_add_auv3_wrapper(
        TARGET simpleCharClap_auv3
        OUTPUT_NAME "${CLAP_NAME}"
        BUNDLE_IDENTIFIER "${CLAP_BUNDLE_ID}.ios.auv3"
        BUNDLE_VERSION "1.0.0"
        MANUFACTURER_NAME "Charlie Culbert"
        MANUFACTURER_CODE "ChCu"
        SUBTYPE_CODE "SvF1"
        INSTRUMENT_TYPE "aufx"
    )

    # The supplied app embeds the extension but has no live-input effect routing.
    add_executable(simpleCharClap_ios entry.cpp)
    target_link_libraries(simpleCharClap_ios PRIVATE simpleCharClap_impl)
    target_add_auv3_standalone_ios_wrapper(
        TARGET simpleCharClap_ios
        OUTPUT_NAME "${CLAP_NAME}"
        BUNDLE_IDENTIFIER "${CLAP_BUNDLE_ID}.ios"
        BUNDLE_VERSION "1.0.0"
        AUV3_TARGET simpleCharClap_auv3
        AU_TYPE "aufx"
        AU_SUBTYPE "SvF1"
        AU_MANUFACTURER "ChCu"
    )

    add_custom_target(simpleCharClap_all DEPENDS simpleCharClap_ios)
    return()
endif()

# =============================================================================
# Desktop and WebAssembly formats
# =============================================================================

if(EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "WASI")
    set(SIMPLECHAR_FORMATS WCLAP)
else()
    # Windows/Linux need neither Apple SDKs nor Audio Unit targets.
    set(SIMPLECHAR_FORMATS CLAP VST3)
    if(APPLE)
        list(APPEND SIMPLECHAR_FORMATS AUV2)
        if(CMAKE_GENERATOR STREQUAL "Xcode")
            list(APPEND SIMPLECHAR_FORMATS AUV3)
        endif()
    endif()
endif()

option(SIMPLECHAR_BUILD_AAX "Build AAX using a locally installed SDK" OFF)
if(SIMPLECHAR_BUILD_AAX)
    if(NOT CLAP_WRAPPER_CAN_BUILD_AAX)
        message(FATAL_ERROR "AAX requires a supported macOS or Windows toolchain")
    endif()
    if(NOT EXISTS "${AAX_SDK_ROOT}/Interfaces/AAX.h")
        message(FATAL_ERROR "Set AAX_SDK_ROOT to your installed AAX SDK")
    endif()
    list(APPEND SIMPLECHAR_FORMATS AAX)
endif()

make_clapfirst_plugins(
    TARGET_NAME simpleCharClap
    IMPL_TARGET simpleCharClap_impl
    ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/entry.cpp"
    OUTPUT_NAME "${CLAP_NAME}"
    BUNDLE_IDENTIFIER "${CLAP_BUNDLE_ID}"
    BUNDLE_VERSION "1.0.0"
    AUV2_MANUFACTURER_NAME "Charlie Culbert"
    AUV2_MANUFACTURER_CODE "ChCu"
    AUV2_SUBTYPE_CODE "SvF1"
    AUV2_INSTRUMENT_TYPE "aufx"
    PLUGIN_FORMATS ${SIMPLECHAR_FORMATS}
    # One folder per VST3 on Windows too, so its web UI can travel inside it.
    WINDOWS_FOLDER_VST3 TRUE
    # Triplet: target suffix, app name, CLAP descriptor ID (plugin.cpp).
    STANDALONE_CONFIGURATIONS standalone "${CLAP_NAME}" "charlieculbert.simpleCharClap"
    COPY_AFTER_BUILD FALSE
)

if(SIMPLECHAR_BUILD_AAX AND APPLE)
    # The pinned wrapper misparses its empty AAX asset-output argument.
    set_target_properties(simpleCharClap_aax PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()

# =============================================================================
# Web UI files
# =============================================================================

# Every format ships the webUI folder as ordinary files, where
# webview/resources.cpp looks for it. The copy runs on every build, so edits,
# additions and deletions in webUI/ need no relink.
set(SIMPLECHAR_WEB_UI "${CMAKE_CURRENT_SOURCE_DIR}/webUI")

if(TARGET simpleCharClap_wclap)
    # A WCLAP is a .tar.gz of module.wasm plus resources, at the archive root.
    # (The pinned clap-wrapper's own archive adds an enclosing folder.)
    set(bundle "$<TARGET_FILE_DIR:simpleCharClap_wclap>/${CLAP_NAME}.wclap")
    add_custom_target(simpleCharClap_wclap_bundle ALL
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${bundle}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${bundle}"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:simpleCharClap_wclap>" "${bundle}/module.wasm"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${SIMPLECHAR_WEB_UI}" "${bundle}/webUI"
        COMMAND ${CMAKE_COMMAND} -E chdir "${bundle}"
                ${CMAKE_COMMAND} -E tar czf "../${CLAP_NAME}.wclap.tar.gz" --format=gnutar module.wasm webUI
        DEPENDS simpleCharClap_wclap
        VERBATIM)
endif()

foreach(target simpleCharClap_clap simpleCharClap_vst3 simpleCharClap_auv2 simpleCharClap_auv3
               simpleCharClap_standalone simpleCharClap_aax)
    if(NOT TARGET ${target})
        continue()
    endif()
    if(APPLE OR target MATCHES "_vst3|_aax")
        set(destination "$<TARGET_FILE_DIR:${target}>/../Resources/webUI")  # <bundle>/Contents/Resources
    else()
        set(destination "$<TARGET_FILE:${target}>.resources/webUI")        # next to a loose binary
    endif()
    add_custom_target(${target}_webUI
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${destination}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${SIMPLECHAR_WEB_UI}" "${destination}"
        VERBATIM)
    add_dependencies(${target} ${target}_webUI)
endforeach()

