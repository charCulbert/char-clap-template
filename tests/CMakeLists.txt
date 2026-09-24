# Tests for the shared code and the analyzer's DSP (not copied into new plugins).
# Built by the top-level CMakeLists.txt, or alone:
#   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
cmake_minimum_required(VERSION 3.28)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(ClapExampleTests LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    enable_testing()
endif()
add_executable(resources_test resources_test.cpp ../libs/core/resources.cpp)
add_test(NAME resources COMMAND resources_test "${CMAKE_CURRENT_BINARY_DIR}/resources-test")

add_executable(messages_test messages_test.cpp ../libs/core/messages.cpp)
add_test(NAME messages COMMAND messages_test)
# The JavaScript codec (libs/webview/js) against the C++ one.
find_program(NODE node)
if(NODE)
    add_test(NAME messages_js COMMAND "${NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/messages.test.mjs" "$<TARGET_FILE:messages_test>")
endif()

include(../examples/gpu-analyzer/signalsmith-dsp.cmake)
add_executable(spectrum_test spectrum_test.cpp)
target_include_directories(spectrum_test PRIVATE ${SIGNALSMITH_DSP_INCLUDE})
add_test(NAME spectrum COMMAND spectrum_test)

# The tests are assert()s, so keep them in every build type.
foreach(test resources_test messages_test spectrum_test)
    target_compile_options(${test} PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
endforeach()
