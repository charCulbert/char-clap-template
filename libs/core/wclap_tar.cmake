# cmake -DBUNDLE=<folder> -DARCHIVE=<file> -P wclap_tar.cmake
# Packs everything in <folder> into <file>, with paths relative to <folder>
# ("module.wasm", not "./module.wasm"). Run at build time: what's in the
# folder (a GPU page, say) isn't known when CMake configures.
# Leaves out hidden files: macOS's .DS_Store, at any depth, and anything at
# the top starting with "." (the folder is a copy, so they can go).
file(GLOB_RECURSE junk "${BUNDLE}/.DS_Store")
if(junk)
    file(REMOVE ${junk})
endif()
file(GLOB entries RELATIVE "${BUNDLE}" "${BUNDLE}/*")
list(FILTER entries EXCLUDE REGEX "^\\.")
file(REMOVE "${ARCHIVE}") # a new file, not the old one rewritten: file pickers then show it as new
list(SORT entries)
execute_process(
    COMMAND ${CMAKE_COMMAND} -E tar czf "${ARCHIVE}" --format=gnutar ${entries}
    WORKING_DIRECTORY "${BUNDLE}"
    COMMAND_ERROR_IS_FATAL ANY)
