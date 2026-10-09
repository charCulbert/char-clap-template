# Every example, plus the tests, in one build: for working on this repository.
# (Each example also builds by itself; see README.md.) Downloads happen once.
#
#   cmake -B build && cmake --build build && ctest --test-dir build
cmake_minimum_required(VERSION 3.28)

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

add_subdirectory(examples/headless-gain)
add_subdirectory(examples/webview-synth)
add_subdirectory(examples/gpu-analyzer)
add_subdirectory(examples/gpu-spectrogram)
add_subdirectory(examples/imgui-filter)

if(NOT CMAKE_SYSTEM_NAME MATCHES "WASI|iOS")
    enable_testing()
    add_subdirectory(tests)
    add_subdirectory(tests/gallery) # a test bench for the GPU interface, not an example
endif()
