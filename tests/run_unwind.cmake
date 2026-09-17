# Decode the Windows unwind data of a program and compare it byte for byte
# with an expected file. Run with cmake -P and these values:
#   ANTIC         the antic executable
#   LLVM_MC       the llvm-mc executable
#   LLVM_READOBJ  the llvm-readobj executable
#   TARGET        windows-x86_64 or windows-arm64
#   TRIPLE        the llvm-mc triple of the target
#   SOURCE        the .anti file
#   WORK          a directory for the assembly and the object
#   EXPECTED      the decoded unwind data without the line that names the file

function(run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE status
        OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT status EQUAL 0 OR NOT err STREQUAL "")
        message(FATAL_ERROR "${ARGN} failed with ${status}\n${out}${err}")
    endif()
    set(run_out "${out}" PARENT_SCOPE)
endfunction()

file(MAKE_DIRECTORY "${WORK}")
get_filename_component(base "${SOURCE}" NAME_WE)
set(assembly "${WORK}/${base}.${TARGET}.s")
set(object "${WORK}/${base}.${TARGET}.obj")
run("${ANTIC}" -S --target "${TARGET}" -o "${assembly}" "${SOURCE}")
run("${LLVM_MC}" "-triple=${TRIPLE}" -filetype=obj -o "${object}" "${assembly}")
run("${LLVM_READOBJ}" --unwind "${object}")
string(REGEX REPLACE "^\nFile: [^\n]*\n" "" decoded "${run_out}")
file(READ "${EXPECTED}" expected)
if(NOT decoded STREQUAL expected)
    message(FATAL_ERROR "the unwind data differs from ${EXPECTED}\n${decoded}")
endif()
