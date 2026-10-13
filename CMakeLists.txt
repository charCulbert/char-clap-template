# Every example, plus the tests, in one build: for working on this repository.
# (Each example also builds by itself; see README.md.) Downloads happen once.
#
#   cmake -B build && cmake --build build && ctest --test-dir build
cmake_minimum_required(VERSION 3.28)

# A plugin uses the template's libs/, not this file: tools/new-plugin.py writes
# the fetch for one.
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    message(FATAL_ERROR "char-clap-template isn't built as part of another project. Fetch it "
        "with SOURCE_SUBDIR none, so this file doesn't run, and include its "
        "libs/<lib>/<lib>.cmake files, as a project made by its tools/new-plugin.py does.")
endif()

if(CMAKE_HOST_APPLE AND NOT CMAKE_SYSTEM_NAME AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()

project(ClapExamples LANGUAGES C CXX)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # build/compile_commands.json, where clangd finds the downloaded headers

add_subdirectory(examples/effect-headless)
add_subdirectory(examples/effect-webview)
add_subdirectory(examples/effect-chardraw)
add_subdirectory(examples/instrument-webview)
add_subdirectory(examples/instrument-chardraw)
add_subdirectory(examples/chardraw-gallery) # a page per chardraw feature, not a starting point

if(NOT CMAKE_SYSTEM_NAME MATCHES "WASI|iOS")
    enable_testing()
    add_subdirectory(tests)
endif()
