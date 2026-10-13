# core_declare(<name> URL <url> URL_HASH SHA256=<hash> [FetchContent_Declare arguments...])
#
# FetchContent_Declare for an archive, which can come from a download cache
# shared by every project on the machine. With CPM_SOURCE_CACHE set (an
# environment variable, or -D), the archive is kept in
# <CPM_SOURCE_CACHE>/downloads/, and the next project unpacks that copy
# instead of downloading it again. clap-wrapper's CPM keeps the SDKs it
# downloads (VST3, AudioUnit, RtAudio, RtMidi) under the same folder, so one
# setting covers everything:
#   export CPM_SOURCE_CACHE=$HOME/.cache/CPM
# Without it, this is FetchContent_Declare.
include_guard(GLOBAL)
include(FetchContent)

# Sets <out> to the download cache's folder, or to "" when there is none.
function(core_download_cache out)
    set(cache "$ENV{CPM_SOURCE_CACHE}")
    if(CPM_SOURCE_CACHE)
        set(cache "${CPM_SOURCE_CACHE}")
    endif()
    if(cache)
        file(TO_CMAKE_PATH "${cache}" cache) # Windows' backslashes
    endif()
    set(${out} "${cache}" PARENT_SCOPE)
endfunction()

function(core_declare name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "URL;URL_HASH" "")
    core_download_cache(cache)
    set(url "${ARG_URL}")
    if(cache AND ARG_URL_HASH MATCHES "^SHA256=([0-9a-fA-F]+)$")
        string(SUBSTRING "${CMAKE_MATCH_1}" 0 16 key)
        get_filename_component(file "${ARG_URL}" NAME)
        set(kept "${cache}/downloads/${key}/${file}")
        if(NOT EXISTS "${kept}")
            message(STATUS "Downloading ${name} into ${cache}/downloads")
            # Under another name until it's whole, so an interrupted download
            # isn't taken for a kept one.
            file(DOWNLOAD "${ARG_URL}" "${kept}.part" EXPECTED_HASH "${ARG_URL_HASH}" STATUS status)
            list(GET status 0 code)
            if(code EQUAL 0)
                file(RENAME "${kept}.part" "${kept}")
            else()
                file(REMOVE "${kept}.part")
            endif()
        endif()
        if(EXISTS "${kept}")
            set(url "${kept}")
        endif()
    endif()
    FetchContent_Declare(${name} URL "${url}" URL_HASH "${ARG_URL_HASH}" ${ARG_UNPARSED_ARGUMENTS})
endfunction()
