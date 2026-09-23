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

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

# =============================================================================
# Plugin identity and shared implementation
# =============================================================================

set(CLAP_NAME "simpleCharClap")
set(CLAP_BUNDLE_ID "com.charlieculbert.simplecharclap")

# All formats share this plugin implementation.
add_library(simpleCharClap_impl STATIC plugin.cpp)
target_link_libraries(simpleCharClap_impl PUBLIC clap)
set_target_properties(simpleCharClap_impl PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(simpleCharClap_impl PRIVATE /W4)
    # Keep the portable C string functions without MSVC-only replacements.
    target_compile_definitions(simpleCharClap_impl PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(simpleCharClap_impl PRIVATE -Wall -Wextra -Wno-unused-parameter)
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
    # A resource directory tells clap-wrapper to bundle and tar the WCLAP.
    set(SIMPLECHAR_FORMATS WCLAP)
    set(SIMPLECHAR_RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/wclap-resources")
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
    RESOURCE_DIRECTORY "${SIMPLECHAR_RESOURCES}"
    # Triplet: target suffix, app name, CLAP descriptor ID (plugin.cpp).
    STANDALONE_CONFIGURATIONS standalone "${CLAP_NAME}" "charlieculbert.simpleCharClap"
    COPY_AFTER_BUILD FALSE
)

if(SIMPLECHAR_BUILD_AAX AND APPLE)
    # The pinned wrapper misparses its empty AAX asset-output argument.
    set_target_properties(simpleCharClap_aax PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()
