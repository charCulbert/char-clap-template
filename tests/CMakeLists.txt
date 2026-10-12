# Tests for the shared code and the examples' DSP (not copied into new plugins).
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
add_executable(spectrogram_test spectrogram_test.cpp)
target_include_directories(spectrogram_test PRIVATE ${SIGNALSMITH_DSP_INCLUDE})
add_test(NAME spectrogram COMMAND spectrogram_test)

# The GPU interface's drawing, layout and controls (libs/chardraw). draw2d_test
# needs a WebGPU adapter (a software one will do) and skips without one.
include(../libs/gpu/gpu.cmake)
include(../libs/chardraw/chardraw.cmake)
add_executable(ui_test ui_test.cpp ../libs/gpu/params.cpp ../libs/core/messages.cpp)
chardraw_add_to(ui_test)
target_link_libraries(ui_test PRIVATE dawn::webgpu_dawn)
add_test(NAME ui COMMAND ui_test)
add_executable(draw2d_test draw2d_test.cpp ../libs/gpu/context.cpp ../libs/gpu/params.cpp ../libs/core/messages.cpp)
chardraw_add_to(draw2d_test)
target_link_libraries(draw2d_test PRIVATE dawn::webgpu_dawn)
add_test(NAME draw2d COMMAND draw2d_test "${CMAKE_CURRENT_SOURCE_DIR}/gallery/resources/fonts/BarlowSemiCondensed-Medium.ttf")
set_tests_properties(draw2d PROPERTIES SKIP_RETURN_CODE 77)
add_executable(context_test context_test.cpp ../libs/gpu/context.cpp)
target_link_libraries(context_test PRIVATE dawn::webgpu_dawn)
add_test(NAME context COMMAND context_test)
set_tests_properties(context PROPERTIES SKIP_RETURN_CODE 77)

# The tests are assert()s, so keep them in every build type.
foreach(test resources_test messages_test spectrum_test spectrogram_test ui_test draw2d_test context_test)
    target_compile_options(${test} PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
endforeach()
