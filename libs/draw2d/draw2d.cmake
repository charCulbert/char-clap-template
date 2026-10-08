# Our own drawing for a gpu::View (libs/gpu): shapes, lines and text with
# WebGPU (draw2d.h), flexbox-style layout with Clay (layout.h), and knobs,
# sliders and drag areas bound to the plugin's parameters (ui.h).
#
#   include(libs/draw2d/draw2d.cmake)
#   draw2d_add_to(<target>)      the impl target natively, the page target in WCLAP
include_guard(DIRECTORY)
include(FetchContent)

set(DRAW2D_DIR "${CMAKE_CURRENT_LIST_DIR}")
# stb_truetype rasterizes the font, stb_image decodes images; Clay lays out
# boxes. All single headers.
FetchContent_Declare(stb
    URL https://github.com/nothings/stb/archive/2c980bb59875b0d32144a71867fbdebb2f77cd20.tar.gz
    URL_HASH SHA256=9a955b1b49a4410088a2e0ee2a9c057c3c907d0c1d75454144cb980aca0ba515
    SOURCE_SUBDIR none) # headers only: don't run its CMakeLists
FetchContent_Declare(clay
    URL https://github.com/nicbarker/clay/archive/refs/tags/v0.14.tar.gz
    URL_HASH SHA256=ee8f6477020dd72afe8cf6f8d3ab6855980028289a2b677a1423d17aab983585
    SOURCE_SUBDIR none) # its CMakeLists builds examples we don't want
# nanosvg parses SVG and rasterizes it, for Draw2D::addSvg. Two single headers.
FetchContent_Declare(nanosvg
    URL https://github.com/memononen/nanosvg/archive/239e102ec2c691f2902e20ace2ed36ee4a35cfe6.tar.gz
    URL_HASH SHA256=2bc68bdb518d7800252042e5cad50a0ab321596f0cbf49ef2a752926329063d2
    SOURCE_SUBDIR none) # headers only
FetchContent_MakeAvailable(stb clay nanosvg)

# stb_textedit 1.14 discarding a redo record moves one undo record past the
# end of its array (undoing with a long history). Dear ImGui's copy has the
# fix: move one record fewer. ui.cpp includes this patched copy.
file(READ "${stb_SOURCE_DIR}/stb_textedit.h" textedit)
set(overrun "(size_t) ((STB_TEXTEDIT_UNDOSTATECOUNT - state->redo_point)*sizeof(state->undo_rec[0]))")
string(FIND "${textedit}" "${overrun}" at)
if(at EQUAL -1)
    message(FATAL_ERROR "stb_textedit.h changed: check whether discard_redo still needs the patch in draw2d.cmake")
endif()
string(REPLACE "${overrun}" "(size_t) ((STB_TEXTEDIT_UNDOSTATECOUNT - state->redo_point - 1)*sizeof(state->undo_rec[0]))" textedit "${textedit}")
file(WRITE "${CMAKE_BINARY_DIR}/stb-patched/stb_textedit.h.new" "${textedit}")
file(COPY_FILE "${CMAKE_BINARY_DIR}/stb-patched/stb_textedit.h.new" "${CMAKE_BINARY_DIR}/stb-patched/stb_textedit.h" ONLY_IF_DIFFERENT)
set(DRAW2D_PATCHED "${CMAKE_BINARY_DIR}/stb-patched")

function(draw2d_add_to target)
    target_sources(${target} PRIVATE "${DRAW2D_DIR}/draw2d.cpp" "${DRAW2D_DIR}/layout.cpp" "${DRAW2D_DIR}/ui.cpp")
    target_include_directories(${target} PRIVATE "${DRAW2D_PATCHED}" "${stb_SOURCE_DIR}" "${clay_SOURCE_DIR}" "${nanosvg_SOURCE_DIR}/src")
endfunction()
