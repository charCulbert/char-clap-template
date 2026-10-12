# Our own drawing for a gpu::View (libs/gpu): shapes, lines and text with
# WebGPU (draw2d.h), flexbox-style layout with Clay (layout.h), and knobs,
# sliders and drag areas bound to the plugin's parameters (ui.h).
#
#   include(libs/chardraw/chardraw.cmake)
#   chardraw_add_to(<target>)      the impl target natively, the page target in WCLAP
include_guard(DIRECTORY)
include(FetchContent)

set(CHARDRAW_DIR "${CMAKE_CURRENT_LIST_DIR}")
# stb_truetype rasterizes the font, stb_image decodes images; Clay lays out
# boxes. All single headers.
FetchContent_Declare(stb
    URL https://github.com/nothings/stb/archive/2c980bb59875b0d32144a71867fbdebb2f77cd20.tar.gz
    URL_HASH SHA256=9a955b1b49a4410088a2e0ee2a9c057c3c907d0c1d75454144cb980aca0ba515
    SOURCE_SUBDIR none) # headers only: don't run its CMakeLists
# Clay at the commit after v0.14 that raises its scroll containers (each
# .clip box) from 10 to 100 a frame: docked windows clip many boxes. Before
# its next API change (ids out of the element declaration).
FetchContent_Declare(clay
    URL https://github.com/nicbarker/clay/archive/7f767d230191bf494d6e158190f0afbff68f8e2a.tar.gz
    URL_HASH SHA256=cad7eb3d3b150f319402362dd393b8100b81ebd68476d8b46ef5153665b7c93f
    SOURCE_SUBDIR none) # its CMakeLists builds examples we don't want
# nanosvg parses SVG and rasterizes it, for Draw2D::addSvg. Two single headers.
FetchContent_Declare(nanosvg
    URL https://github.com/memononen/nanosvg/archive/239e102ec2c691f2902e20ace2ed36ee4a35cfe6.tar.gz
    URL_HASH SHA256=2bc68bdb518d7800252042e5cad50a0ab321596f0cbf49ef2a752926329063d2
    SOURCE_SUBDIR none) # headers only
FetchContent_MakeAvailable(stb clay nanosvg)

# stb_textedit 1.14 discarding a redo record moves one undo record past the
# end of its array (undoing with a long history). Dear ImGui's copy has the
# fix: move one record fewer. ui_text.cpp includes this patched copy.
file(READ "${stb_SOURCE_DIR}/stb_textedit.h" textedit)
set(overrun "(size_t) ((STB_TEXTEDIT_UNDOSTATECOUNT - state->redo_point)*sizeof(state->undo_rec[0]))")
string(FIND "${textedit}" "${overrun}" at)
if(at EQUAL -1)
    message(FATAL_ERROR "stb_textedit.h changed: check whether discard_redo still needs the patch in chardraw.cmake")
endif()
string(REPLACE "${overrun}" "(size_t) ((STB_TEXTEDIT_UNDOSTATECOUNT - state->redo_point - 1)*sizeof(state->undo_rec[0]))" textedit "${textedit}")
file(WRITE "${CMAKE_BINARY_DIR}/stb-patched/stb_textedit.h.new" "${textedit}")
file(COPY_FILE "${CMAKE_BINARY_DIR}/stb-patched/stb_textedit.h.new" "${CMAKE_BINARY_DIR}/stb-patched/stb_textedit.h" ONLY_IF_DIFFERENT)
set(CHARDRAW_PATCHED "${CMAKE_BINARY_DIR}/stb-patched")

function(chardraw_add_to target)
    target_sources(${target} PRIVATE "${CHARDRAW_DIR}/draw2d.cpp" "${CHARDRAW_DIR}/layout.cpp" "${CHARDRAW_DIR}/ui.cpp"
        "${CHARDRAW_DIR}/ui_access.cpp" "${CHARDRAW_DIR}/ui_controls.cpp" "${CHARDRAW_DIR}/ui_panels.cpp" "${CHARDRAW_DIR}/ui_text.cpp")
    if(MSVC) # a role with no name (roleName's switch) is an error
        set_source_files_properties("${CHARDRAW_DIR}/ui_access.cpp" PROPERTIES COMPILE_OPTIONS /we4062)
    else()
        set_source_files_properties("${CHARDRAW_DIR}/ui_access.cpp" PROPERTIES COMPILE_OPTIONS -Werror=switch)
    endif()
    # Third-party headers: their own warnings aren't the plugin's to fix.
    target_include_directories(${target} SYSTEM PRIVATE "${CHARDRAW_PATCHED}" "${stb_SOURCE_DIR}" "${clay_SOURCE_DIR}" "${nanosvg_SOURCE_DIR}/src")
    # Clay's API is designated initializers that leave the rest of a struct at
    # zero (CLAY({.layout = ...}), CLAY_SIZING_FIT(0)); every use would warn.
    if(NOT MSVC)
        target_compile_options(${target} PRIVATE -Wno-missing-field-initializers)
    endif()
endfunction()
