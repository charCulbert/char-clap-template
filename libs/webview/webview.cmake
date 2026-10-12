# A plugin interface that is a web page. Hosts with clap.webview show the page
# themselves (WCLAP hosts always do); elsewhere a native WebView shows it inside
# the host's window (CHOC's on macOS and Windows; on Linux WebKitGTK, in a
# helper process: see native_linux.cpp). Either way the page talks to the plugin with
# js/messages.js, and the plugin serves the page's files through clap.webview.
#
#   include(libs/webview/webview.cmake)
#   webview_add_to(<impl target>)             the presenter (clap.gui side)
#   webview_resources(<out var> <name> <folder>)   <folder> plus page/lib/messages.js, to ship
include_guard(DIRECTORY)
include(FetchContent)

set(WEBVIEW_DIR "${CMAKE_CURRENT_LIST_DIR}")
if(NOT CMAKE_SYSTEM_NAME MATCHES "WASI|iOS")
    set(WEBVIEW_NATIVE ON)
    FetchContent_Declare(choc
        GIT_REPOSITORY https://github.com/Tracktion/choc.git
        GIT_TAG 9606a6615b592605993d92733cdfd6ca6ecbd825)
    FetchContent_MakeAvailable(choc)
endif()

function(webview_add_to target)
    target_sources(${target} PRIVATE "${WEBVIEW_DIR}/gui.cpp")
    if(NOT WEBVIEW_NATIVE)
        return()
    endif()
    target_link_libraries(${target} PRIVATE choc::choc)
    if(APPLE)
        target_sources(${target} PRIVATE "${WEBVIEW_DIR}/native_mac.mm")
        set_source_files_properties("${WEBVIEW_DIR}/native_mac.mm" PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_link_libraries(${target} PUBLIC "-framework AppKit" "-framework WebKit")
    elseif(WIN32)
        target_sources(${target} PRIVATE "${WEBVIEW_DIR}/native_win.cpp")
    else()
        # No GTK or WebKit to link: a helper process finds them at run time. It
        # is the plugin's own binary run as a program, from this entry point.
        target_sources(${target} PRIVATE "${WEBVIEW_DIR}/native_linux.cpp")
        target_link_libraries(${target} PUBLIC ${CMAKE_DL_LIBS})
        target_link_options(${target} PUBLIC
            "$<$<IN_LIST:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY;SHARED_LIBRARY>:LINKER:-e,webview_helper_start>")
    endif()
endfunction()

# Assembles the plugin's resources to ship: <folder> (its page in page/, and
# any other files) plus js/messages.js as page/lib/messages.js, in the build
# folder's resources/, rebuilt on every build by target <name>_resources.
# Sets <out var> to that folder, for core_ship_resources.
function(webview_resources out name folder)
    get_filename_component(folder "${folder}" ABSOLUTE)
    set(staging "${CMAKE_CURRENT_BINARY_DIR}/resources")
    add_custom_target(${name}_resources
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${staging}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${folder}" "${staging}"
        COMMAND ${CMAKE_COMMAND} -E copy "${WEBVIEW_DIR}/js/messages.js" "${staging}/page/lib/messages.js"
        VERBATIM)
    set(${out} "${staging}" PARENT_SCOPE)
endfunction()
