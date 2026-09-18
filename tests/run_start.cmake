# Link the assembly of a program that returns a value with the first
# rt/start.c of chapter 1, run it and compare the exit code with the
# expected file beside the source. Run with cmake -P and these values:
#   ANTIC     the antic executable
#   LLVM_MC   the llvm-mc executable
#   START     the start.c of chapter 1
#   SOURCE    the .anti file
#   WORK      a directory for the files

function(run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE status
        OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT status EQUAL 0 OR NOT err STREQUAL "")
        message(FATAL_ERROR "${ARGN} failed with ${status}\n${out}${err}")
    endif()
    set(run_out "${out}" PARENT_SCOPE)
endfunction()

file(MAKE_DIRECTORY "${WORK}")
run("${ANTIC}" --print-host-target)
string(STRIP "${run_out}" host)
set(triples "macos-arm64|arm64-apple-macos" "macos-x86_64|x86_64-apple-macos"
    "linux-x86_64|x86_64-unknown-linux-gnu"
    "linux-arm64|aarch64-unknown-linux-gnu")
foreach(entry IN LISTS triples)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 name)
    if(name STREQUAL host)
        list(GET parts 1 triple)
    endif()
endforeach()
get_filename_component(name "${SOURCE}" NAME_WE)
get_filename_component(dir "${SOURCE}" DIRECTORY)
run("${ANTIC}" -S -o "${WORK}/${name}.s" "${SOURCE}")
run("${LLVM_MC}" "-triple=${triple}" -filetype=obj -o "${WORK}/${name}.o"
    "${WORK}/${name}.s")
run(cc "${START}" "${WORK}/${name}.o" -o "${WORK}/${name}")
execute_process(COMMAND "${WORK}/${name}" RESULT_VARIABLE exit_code)
file(READ "${dir}/${name}.expected" expected)
string(REGEX MATCH "^exit ([0-9]+)" line "${expected}")
if(NOT exit_code STREQUAL CMAKE_MATCH_1)
    message(FATAL_ERROR "exit code ${exit_code}, expected ${CMAKE_MATCH_1}")
endif()
