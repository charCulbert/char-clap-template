# A plugin interface that is a web page. Hosts with clap.webview show the page
# themselves (WCLAP hosts always do); elsewhere CHOC's WebView shows it inside
# the host's window. Either way the page talks to the plugin with
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
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(WEBKIT REQUIRED IMPORTED_TARGET gtk+-3.0 webkit2gtk-4.1 x11)
        target_sources(${target} PRIVATE "${WEBVIEW_DIR}/native_linux.cpp")
        target_link_libraries(${target} PUBLIC PkgConfig::WEBKIT)
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
