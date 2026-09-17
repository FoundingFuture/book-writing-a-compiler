# No file that a user receives holds a path of the machine that built it.
# Debug information and __FILE__ carry one unless the build maps it away.
#
#   cmake -DROOT=<repository> -DRUNTIME=<runtime archive> -DSTRINGS=<tool>
#         -P tests/run_no_paths.cmake

file(GLOB_RECURSE shipped "${RUNTIME}/lib/*.a" "${RUNTIME}/lib/*.lib"
     "${RUNTIME}/lib/*.o" "${RUNTIME}/lib/*.obj")
if(shipped STREQUAL "")
    message(FATAL_ERROR "no library under ${RUNTIME}/lib")
endif()
list(APPEND shipped "${ANTIC}")

get_filename_component(root "${ROOT}" ABSOLUTE)
set(found "")
foreach(file IN LISTS shipped)
    execute_process(COMMAND "${STRINGS}" "${file}" OUTPUT_VARIABLE text
                    ERROR_VARIABLE ignored)
    string(FIND "${text}" "${root}" at)
    if(NOT at EQUAL -1)
        get_filename_component(name "${file}" NAME)
        string(APPEND found "${name} holds ${root}\n")
    endif()
endforeach()
if(NOT found STREQUAL "")
    message(FATAL_ERROR "${found}")
endif()
