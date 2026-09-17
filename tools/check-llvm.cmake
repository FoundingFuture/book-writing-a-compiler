# Check that llvm-mc, lld, llvm-ar and llvm-readobj in LLVM_BIN are the LLVM
# release of tools/llvm-version, and fail otherwise. With DEST set, copy them and
# llvm-objdump there, which is how the runtime archive carries the pinned
# tools. Run with cmake -P and these values:
#   LLVM_BIN  the directory of the installed tools
#   DEST      optional directory of the runtime archive's tools

file(READ "${CMAKE_CURRENT_LIST_DIR}/llvm-version" pin)
string(STRIP "${pin}" pin)
if(CMAKE_HOST_WIN32)
    set(exe ".exe")
endif()
set(failed "")
foreach(tool llvm-mc llvm-ar llvm-readobj ld.lld ld64.lld lld-link)
    execute_process(COMMAND "${LLVM_BIN}/${tool}${exe}" --version
        OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE status)
    string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" found "${out}${err}")
    if(found STREQUAL pin)
        message(STATUS "${tool} ${found}")
    else()
        string(APPEND failed "${tool} has version '${found}', the pin is ${pin}\n")
    endif()
endforeach()
if(NOT failed STREQUAL "")
    message(FATAL_ERROR "${failed}")
endif()
if(DEFINED DEST)
    file(MAKE_DIRECTORY "${DEST}")
    foreach(tool llvm-mc llvm-ar llvm-objdump llvm-readobj lld ld.lld ld64.lld
            lld-link)
        if(EXISTS "${LLVM_BIN}/${tool}${exe}")
            file(COPY "${LLVM_BIN}/${tool}${exe}" DESTINATION "${DEST}")
        endif()
    endforeach()
    file(WRITE "${DEST}/llvm-version" "${pin}\n")
endif()
