# A plugin interface drawn with WebGPU, as a gpu::View (view.h). Natively, Dawn
# (Google's WebGPU) draws it straight into the host's window. In WCLAP the
# host shows a web page instead: the same View compiled to wasm with
# Emscripten, drawing with the browser's WebGPU.
#
#   include(libs/gpu/gpu.cmake)
#   gpu_add_to(<impl target>)                  the presenter (clap.gui side)
#   gpu_add_page(<target> ASSETS <folder>)     the WCLAP page, in the Emscripten build
#   gpu_wclap_resources(<out var> <name> <folder>)   from a WCLAP build: <folder>
#                                              plus that page as page/, to ship
#
# The View reads its files (fonts) with gpu::loadAsset: natively from the
# plugin's resources (core_ship_resources); in the page, from copies built
# into it (ASSETS), since loadAsset can't wait for a download.
include_guard(DIRECTORY)
include(FetchContent)

set(GPU_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(GPU_DAWN_RELEASE "https://github.com/google/dawn/releases/download/v20260922.191850")
set(GPU_DAWN_BUILD "Dawn-73cc233a8075446b7d377f2169323f1d2ebc1c89")
# The licences of what each Dawn release and Emscripten version compile in
# are in licenses/, one file per version: a new pin needs a new file.
get_filename_component(GPU_DAWN_VERSION "${GPU_DAWN_RELEASE}" NAME)

if(EMSCRIPTEN)
    # Dawn's WebGPU C++ API for Emscripten, over the browser's WebGPU.
    FetchContent_Declare(emdawnwebgpu
        URL "${GPU_DAWN_RELEASE}/emdawnwebgpu_pkg-v20260922.191850.zip"
        URL_HASH SHA256=381a56505a68db3a8f34ec43eefa87214a45b3b94a438624297fd6186c8db257)
    FetchContent_MakeAvailable(emdawnwebgpu)
elseif(NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
    # Dawn's prebuilt release: one static library.
    if(APPLE)
        FetchContent_Declare(dawn URL "${GPU_DAWN_RELEASE}/${GPU_DAWN_BUILD}-macos-latest-Release.tar.gz"
            URL_HASH SHA256=8466aa51e771cdc29c40ef0d1648c7011edb41a8c03892175f4b0ae0ff6b70e2)
        FetchContent_Declare(dawn_intel URL "${GPU_DAWN_RELEASE}/${GPU_DAWN_BUILD}-macos-15-intel-Release.tar.gz"
            URL_HASH SHA256=56c4c40ed37054278d073959d13994c7f43b0ca436186d62210e84322cae6682)
        FetchContent_MakeAvailable(dawn dawn_intel)
    elseif(WIN32)
        FetchContent_Declare(dawn URL "${GPU_DAWN_RELEASE}/${GPU_DAWN_BUILD}-windows-latest-Release.tar.gz"
            URL_HASH SHA256=38d4b019e0748821f00df38f16d47094087c1ee3a642737af4c929d45a1a72d9)
        FetchContent_MakeAvailable(dawn)
    else()
        FetchContent_Declare(dawn URL "${GPU_DAWN_RELEASE}/${GPU_DAWN_BUILD}-ubuntu-latest-Release.tar.gz"
            URL_HASH SHA256=f40bfaa23c149d2728fe094ac9dad7c4f6b7340c29d752301ae6be51d6080125)
        FetchContent_MakeAvailable(dawn)
    endif()
    set_property(GLOBAL APPEND PROPERTY CORE_THIRD_PARTY_LICENSES
        "dawn=${GPU_DIR}/licenses/dawn-${GPU_DAWN_VERSION}.txt" "dawn_intel=")
    if(NOT TARGET dawn::webgpu_dawn)
        find_package(Threads REQUIRED) # Dawn's config uses Threads::Threads without finding it
        file(GLOB dawn_config_dir "${dawn_SOURCE_DIR}/lib*/cmake/Dawn") # lib/ or, on Linux, lib64/
        find_package(Dawn CONFIG REQUIRED PATHS ${dawn_config_dir} NO_DEFAULT_PATH GLOBAL)
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
    endif()
endif()

# AccessKit: the view's accessibility tree to the system's screen-reader API
# (NSAccessibility, UI Automation). Its prebuilt static libraries; Linux not
# yet (its adapter is far larger, and the plugins would have to keep its
# symbols to themselves).
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR WIN32) # macOS (not iOS) and Windows
    FetchContent_Declare(accesskit
        URL https://github.com/AccessKit/accesskit-c/releases/download/0.23.1/accesskit-c-0.23.1.zip
        URL_HASH SHA256=35b7ca8a6f1e038b5da35e1e9e5a0adaed9bfcf21e1496d29598fbbadcc7043f
        SOURCE_SUBDIR none) # prebuilt: its CMakeLists builds from Rust source
    FetchContent_MakeAvailable(accesskit)
    set_property(GLOBAL APPEND PROPERTY CORE_THIRD_PARTY_LICENSES
        "accesskit=${accesskit_SOURCE_DIR}/LICENSE-MIT" "accesskit-chromium=${accesskit_SOURCE_DIR}/LICENSE.chromium")
    if(NOT TARGET gpu_accesskit)
        add_library(gpu_accesskit STATIC IMPORTED GLOBAL)
        set_target_properties(gpu_accesskit PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${accesskit_SOURCE_DIR}/include")
        if(APPLE) # arm64 and x86_64 separately, as Dawn
            set(universal "${CMAKE_BINARY_DIR}/accesskit-universal/libaccesskit.a")
            if(NOT EXISTS "${universal}")
                file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/accesskit-universal")
                execute_process(COMMAND lipo -create -output "${universal}"
                    "${accesskit_SOURCE_DIR}/lib/macos/arm64/static/libaccesskit.a"
                    "${accesskit_SOURCE_DIR}/lib/macos/x86_64/static/libaccesskit.a"
                    COMMAND_ERROR_IS_FATAL ANY)
            endif()
            set_target_properties(gpu_accesskit PROPERTIES IMPORTED_LOCATION "${universal}"
                INTERFACE_LINK_LIBRARIES "-framework AppKit;-framework Foundation;-framework CoreFoundation;objc")
        else()
            if(CMAKE_GENERATOR_PLATFORM MATCHES "^(ARM64|arm64)$" OR CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64)$")
                set(arch arm64)
            elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
                set(arch x86)
            else()
                set(arch x86_64)
            endif()
            set_target_properties(gpu_accesskit PROPERTIES IMPORTED_LOCATION "${accesskit_SOURCE_DIR}/lib/windows/${arch}/msvc/static/accesskit.lib"
                INTERFACE_LINK_LIBRARIES "bcrypt;ntdll;propsys;runtimeobject;uiautomationcore;userenv;ws2_32")
        endif()
    endif()
endif()

# The presenter. In WCLAP it only hands the interface to the host's page.
function(gpu_add_to target)
    target_sources(${target} PRIVATE "${GPU_DIR}/gui.cpp")
    if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
        return()
    endif()
    target_sources(${target} PRIVATE "${GPU_DIR}/context.cpp" "${GPU_DIR}/params.cpp")
    target_link_libraries(${target} PUBLIC dawn::webgpu_dawn)
    if(APPLE)
        target_sources(${target} PRIVATE "${GPU_DIR}/native_mac.mm" "${GPU_DIR}/accesskit_tree.cpp")
        target_link_libraries(${target} PUBLIC gpu_accesskit)
        set_source_files_properties("${GPU_DIR}/native_mac.mm" PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        # Objective-C classes share one namespace across every plugin a host
        # loads, so each plugin names its own (see native_mac.mm).
        string(MAKE_C_IDENTIFIER "${PROJECT_NAME}_${target}_" prefix)
        target_compile_definitions(${target} PRIVATE GPU_OBJC_PREFIX=${prefix})
        target_link_libraries(${target} PUBLIC "-framework AppKit" "-framework QuartzCore")
    elseif(WIN32)
        target_sources(${target} PRIVATE "${GPU_DIR}/native_win.cpp" "${GPU_DIR}/accesskit_tree.cpp")
        target_link_libraries(${target} PUBLIC gpu_accesskit)
        target_link_libraries(${target} PUBLIC advapi32 ole32 shell32) # RegGetValueW (darkMode); drag and drop
    else()
        find_package(X11 REQUIRED)
        target_sources(${target} PRIVATE "${GPU_DIR}/native_linux.cpp")
        target_link_libraries(${target} PUBLIC X11::X11)
    endif()
endfunction()

# The WCLAP page: an Emscripten executable <target> (gui.js + gui.wasm) plus
# index.html, in ${CMAKE_BINARY_DIR}/page. Add the View's sources to <target>.
# ASSETS: a folder built into the wasm, read with gpu::loadAsset.
function(gpu_add_page target)
    cmake_parse_arguments(ARG "" "ASSETS" "" ${ARGN})
    set(port "--use-port=${emdawnwebgpu_SOURCE_DIR}/emdawnwebgpu.port.py")
    add_executable(${target} "${GPU_DIR}/web/main.cpp" "${GPU_DIR}/context.cpp" "${GPU_DIR}/params.cpp"
                             "${GPU_DIR}/../core/messages.cpp")
    target_include_directories(${target} PRIVATE "${GPU_DIR}/..")
    target_compile_options(${target} PRIVATE ${port})
    target_link_options(${target} PRIVATE ${port} -sALLOW_MEMORY_GROWTH
        -sEXPORTED_FUNCTIONS=_main,_malloc,_free,_gpuReceive,_gpuPaste)
    set_target_properties(${target} PROPERTIES OUTPUT_NAME gui SUFFIX ".js"
                                               RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/page")
    if(ARG_ASSETS)
        get_filename_component(assets "${ARG_ASSETS}" ABSOLUTE)
        target_link_options(${target} PRIVATE "--embed-file=${assets}@/assets")
        # Relink when an asset changes (the linker reads them; CMake doesn't know).
        file(GLOB_RECURSE files CONFIGURE_DEPENDS "${assets}/*")
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS ${files})
    endif()
    add_custom_target(${target}_html ALL COMMAND ${CMAKE_COMMAND} -E copy
        "${GPU_DIR}/web/index.html" "${CMAKE_BINARY_DIR}/page/index.html")
endfunction()

# From a WCLAP build: configures this project again with Emscripten (emcmake,
# with GPU_PAGE set) so it builds its gpu_add_page target, then assembles
# <folder> plus that page, as page/, in the build folder's resources/. Sets
# <out var> to that folder; target <name>_resources makes it.
function(gpu_wclap_resources out name folder)
    include(ExternalProject)
    find_program(EMCMAKE emcmake REQUIRED)
    find_program(EMCC emcc REQUIRED)
    execute_process(COMMAND "${EMCC}" --version OUTPUT_VARIABLE version COMMAND_ERROR_IS_FATAL ANY)
    string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" version "${version}")
    set_property(GLOBAL APPEND PROPERTY CORE_THIRD_PARTY_LICENSES
        "emscripten=${GPU_DIR}/licenses/emscripten-${version}.txt"
        "emdawnwebgpu=${GPU_DIR}/licenses/emdawnwebgpu-${GPU_DAWN_VERSION}.txt")
    ExternalProject_Add(${name}_page
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
        BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/page-build"
        CONFIGURE_COMMAND "${EMCMAKE}" "${CMAKE_COMMAND}" -S <SOURCE_DIR> -B <BINARY_DIR>
            -DGPU_PAGE=ON -DCMAKE_BUILD_TYPE=Release
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR>
        INSTALL_COMMAND ""
        BUILD_ALWAYS ON)
    get_filename_component(folder "${folder}" ABSOLUTE)
    set(staging "${CMAKE_CURRENT_BINARY_DIR}/resources")
    add_custom_target(${name}_resources
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${staging}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${folder}" "${staging}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_BINARY_DIR}/page-build/page" "${staging}/page"
        DEPENDS ${name}_page
        VERBATIM)
    set(${out} "${staging}" PARENT_SCOPE)
endfunction()
