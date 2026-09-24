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

include(FetchContent)

# How the plugin draws its interface:
#   web  an HTML page (webUI/): a WebView natively, the host's page in WCLAP
#   gpu  a gpu::View (ui/FilterView.cpp) drawn with WebGPU: Dawn straight into
#        the host's window natively; the same code compiled to wasm in WCLAP
set(SIMPLECHAR_UI web CACHE STRING "Plugin interface: web or gpu")
set_property(CACHE SIMPLECHAR_UI PROPERTY STRINGS web gpu)

set(DAWN_RELEASE "https://github.com/google/dawn/releases/download/v20260922.191850")

# stb_truetype rasterizes the gpu interface's font (gpu/draw2d.cpp).
if(SIMPLECHAR_UI STREQUAL "gpu" OR SIMPLECHAR_GPU_PAGE)
    FetchContent_Declare(stb
        URL https://github.com/nothings/stb/archive/2c980bb59875b0d32144a71867fbdebb2f77cd20.tar.gz
        URL_HASH SHA256=9a955b1b49a4410088a2e0ee2a9c057c3c907d0c1d75454144cb980aca0ba515)
    # Clay lays the gpu interface out like flexbox (gpu/layout.cpp).
    FetchContent_Declare(clay
        URL https://github.com/nicbarker/clay/archive/refs/tags/v0.14.tar.gz
        URL_HASH SHA256=ee8f6477020dd72afe8cf6f8d3ab6855980028289a2b677a1423d17aab983585)
    FetchContent_Populate(clay) # one header; its CMakeLists builds examples we don't want
    FetchContent_MakeAvailable(stb)
endif()

# This plugin's gpu interface: the only plugin-specific sources in the gpu build.
set(SIMPLECHAR_GPU_VIEW ui/FilterView.cpp)
set(SIMPLECHAR_GPU_SOURCES gpu/context.cpp gpu/draw2d.cpp gpu/layout.cpp gpu/params.cpp gpu/ui.cpp)
set(DAWN_BUILD "Dawn-73cc233a8075446b7d377f2169323f1d2ebc1c89")

# =============================================================================
# The gpu interface's WCLAP page: the View built with Emscripten. The WCLAP
# build configures this file a second time with SIMPLECHAR_GPU_PAGE (below).
# =============================================================================

if(SIMPLECHAR_GPU_PAGE)
    FetchContent_Declare(emdawnwebgpu
        URL "${DAWN_RELEASE}/emdawnwebgpu_pkg-v20260922.191850.zip"
        URL_HASH SHA256=381a56505a68db3a8f34ec43eefa87214a45b3b94a438624297fd6186c8db257)
    FetchContent_MakeAvailable(emdawnwebgpu)
    set(webgpu_port "--use-port=${emdawnwebgpu_SOURCE_DIR}/emdawnwebgpu.port.py")
    add_executable(gui gpu/web/main.cpp ${SIMPLECHAR_GPU_SOURCES} ${SIMPLECHAR_GPU_VIEW} webview/messages.cpp)
    target_include_directories(gui PRIVATE "${stb_SOURCE_DIR}" "${clay_SOURCE_DIR}")
    target_compile_options(gui PRIVATE ${webgpu_port})
    # ui/assets/ (the font) goes inside gui.wasm, readable at /assets.
    target_link_options(gui PRIVATE ${webgpu_port} -sALLOW_MEMORY_GROWTH
        "--embed-file=${CMAKE_CURRENT_SOURCE_DIR}/ui/assets@/assets"
        -sEXPORTED_FUNCTIONS=_main,_malloc,_free,_gpuReceive)
    set_target_properties(gui PROPERTIES SUFFIX ".js" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/page")
    # Relink when an embedded asset changes (the linker reads them, CMake doesn't know).
    file(GLOB_RECURSE ui_assets CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ui/assets/*")
    set_property(TARGET gui APPEND PROPERTY LINK_DEPENDS ${ui_assets})
    # Copied on every build, so editing the page needs no recompile.
    add_custom_target(page_html ALL COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_CURRENT_SOURCE_DIR}/gpu/web/index.html" "${CMAKE_BINARY_DIR}/page/index.html")
    return()
endif()

# =============================================================================
# Dependencies: CLAP SDK and clap-wrapper
# =============================================================================

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
if(NOT EMSCRIPTEN AND NOT CMAKE_SYSTEM_NAME MATCHES "WASI|iOS" AND SIMPLECHAR_UI STREQUAL "gpu")
    set(SIMPLECHAR_NATIVE_GPU ON)
    # Dawn, Google's WebGPU, as its prebuilt release (one static library).
    if(APPLE)
        FetchContent_Declare(dawn URL "${DAWN_RELEASE}/${DAWN_BUILD}-macos-latest-Release.tar.gz"
            URL_HASH SHA256=8466aa51e771cdc29c40ef0d1648c7011edb41a8c03892175f4b0ae0ff6b70e2)
        FetchContent_Declare(dawn_intel URL "${DAWN_RELEASE}/${DAWN_BUILD}-macos-15-intel-Release.tar.gz"
            URL_HASH SHA256=56c4c40ed37054278d073959d13994c7f43b0ca436186d62210e84322cae6682)
        FetchContent_MakeAvailable(dawn dawn_intel)
    elseif(WIN32)
        FetchContent_Declare(dawn URL "${DAWN_RELEASE}/${DAWN_BUILD}-windows-latest-Release.tar.gz"
            URL_HASH SHA256=38d4b019e0748821f00df38f16d47094087c1ee3a642737af4c929d45a1a72d9)
        FetchContent_MakeAvailable(dawn)
    else()
        FetchContent_Declare(dawn URL "${DAWN_RELEASE}/${DAWN_BUILD}-ubuntu-latest-Release.tar.gz"
            URL_HASH SHA256=f40bfaa23c149d2728fe094ac9dad7c4f6b7340c29d752301ae6be51d6080125)
        FetchContent_MakeAvailable(dawn)
    endif()
    find_package(Threads REQUIRED) # Dawn's config uses Threads::Threads without finding it
    file(GLOB dawn_config_dir "${dawn_SOURCE_DIR}/lib*/cmake/Dawn") # lib/ or, on Linux, lib64/
    find_package(Dawn CONFIG REQUIRED PATHS ${dawn_config_dir} NO_DEFAULT_PATH)
    if(APPLE)
        # Dawn ships arm64 and x86_64 separately; plugins here are universal.
        set(universal "${CMAKE_BINARY_DIR}/dawn-universal/libwebgpu_dawn.a")
        if(NOT EXISTS "${universal}")
            file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/dawn-universal")
            execute_process(COMMAND lipo -create -output "${universal}"
                "${dawn_SOURCE_DIR}/lib/libwebgpu_dawn.a" "${dawn_intel_SOURCE_DIR}/lib/libwebgpu_dawn.a"
                COMMAND_ERROR_IS_FATAL ANY)
        endif()
        set_target_properties(dawn::webgpu_dawn PROPERTIES IMPORTED_LOCATION_RELEASE "${universal}")
    endif()
elseif(NOT EMSCRIPTEN AND NOT CMAKE_SYSTEM_NAME MATCHES "WASI|iOS")
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
add_library(simpleCharClap_impl STATIC plugin.cpp webview/messages.cpp webview/resources.cpp)
target_include_directories(simpleCharClap_impl PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(simpleCharClap_impl PUBLIC clap)
if(SIMPLECHAR_NATIVE_GPU)
    # gpu/ is the reusable Dawn presenter; ui/ is this plugin's interface.
    target_compile_definitions(simpleCharClap_impl PRIVATE SIMPLECHAR_GPU_UI=1)
    target_sources(simpleCharClap_impl PRIVATE gpu/gui.cpp ${SIMPLECHAR_GPU_SOURCES} ${SIMPLECHAR_GPU_VIEW})
    target_include_directories(simpleCharClap_impl PRIVATE "${stb_SOURCE_DIR}" "${clay_SOURCE_DIR}")
    target_link_libraries(simpleCharClap_impl PUBLIC dawn::webgpu_dawn)
    if(APPLE)
        target_sources(simpleCharClap_impl PRIVATE gpu/native_mac.mm)
        set_source_files_properties(gpu/native_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_link_libraries(simpleCharClap_impl PUBLIC "-framework AppKit" "-framework QuartzCore")
    elseif(WIN32)
        target_sources(simpleCharClap_impl PRIVATE gpu/native_win.cpp)
    else()
        find_package(X11 REQUIRED)
        target_sources(simpleCharClap_impl PRIVATE gpu/native_linux.cpp)
        target_link_libraries(simpleCharClap_impl PUBLIC X11::X11)
    endif()
else()
    # web interface; the WebView presenter is also the WCLAP host-page path.
    target_sources(simpleCharClap_impl PRIVATE webview/gui.cpp)
endif()
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

if(TARGET simpleCharClap_wclap AND SIMPLECHAR_UI STREQUAL "gpu")
    # The page is the gpu::View compiled to wasm; this file builds it again with Emscripten.
    include(ExternalProject)
    find_program(EMCMAKE emcmake REQUIRED)
    ExternalProject_Add(simpleCharClap_gpu_page
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
        BINARY_DIR "${CMAKE_BINARY_DIR}/gpu-page"
        CONFIGURE_COMMAND "${EMCMAKE}" "${CMAKE_COMMAND}" -S <SOURCE_DIR> -B <BINARY_DIR>
            -DSIMPLECHAR_GPU_PAGE=ON -DCMAKE_BUILD_TYPE=Release
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR>
        INSTALL_COMMAND ""
        BUILD_ALWAYS ON)
    set(SIMPLECHAR_WEB_UI "${CMAKE_BINARY_DIR}/gpu-page/page")
endif()

if(TARGET simpleCharClap_wclap)
    # Browsers download the module, so leave DWARF debug info out of Release builds.
    target_link_options(simpleCharClap_wclap PRIVATE $<$<CONFIG:Release,MinSizeRel>:-Wl,--strip-debug>)

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
    if(TARGET simpleCharClap_gpu_page)
        add_dependencies(simpleCharClap_wclap_bundle simpleCharClap_gpu_page)
    endif()
endif()

# A native gpu build ships ui/assets/ (its font) in the same place instead.
if(SIMPLECHAR_UI STREQUAL "gpu")
    set(SIMPLECHAR_WEB_UI "${CMAKE_CURRENT_SOURCE_DIR}/ui/assets")
endif()

foreach(target simpleCharClap_clap simpleCharClap_vst3 simpleCharClap_auv2 simpleCharClap_auv3
               simpleCharClap_standalone simpleCharClap_aax)
    if(NOT TARGET ${target})
        continue()
    endif()
    if(APPLE OR target MATCHES "_vst3|_aax")
        set(destination "$<TARGET_FILE_DIR:${target}>/../Resources/webUI")  # <bundle>/Contents/Resources
    else()
        # next to a loose binary (TARGET_FILE here would make the target depend on itself)
        set(destination "$<TARGET_FILE_DIR:${target}>/$<TARGET_FILE_NAME:${target}>.resources/webUI")
    endif()
    add_custom_target(${target}_webUI
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${destination}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${SIMPLECHAR_WEB_UI}" "${destination}"
        VERBATIM)
    add_dependencies(${target} ${target}_webUI)
endforeach()

