# Link tests/programs/return42.anti for TARGET on this host with lld and
# check the object file format of the executable with llvm-objdump. The
# format names the architecture. Run with cmake -P and these values:
#   ANTIC         the antic executable
#   LLVM_MC       the llvm-mc executable
#   LLVM_OBJDUMP  the llvm-objdump executable
#   RUNTIME       the runtime directory
#   SOURCE        the .anti file
#   WORK          a directory for the executable
#   TARGET        the target name
#   FORMAT        the file format that llvm-objdump prints

if(NOT EXISTS "${RUNTIME}/sysroot/${TARGET}")
    message("SKIP: the runtime archive has no sysroot for ${TARGET}")
    return()
endif()
file(MAKE_DIRECTORY "${WORK}")
set(exe "${WORK}/return42-${TARGET}")
execute_process(
    COMMAND "${ANTIC}" --target "${TARGET}" --llvm-mc "${LLVM_MC}"
            --runtime "${RUNTIME}" -o "${exe}" "${SOURCE}"
    RESULT_VARIABLE status ERROR_VARIABLE err)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "antic failed for ${TARGET}\n${err}")
endif()
execute_process(COMMAND "${LLVM_OBJDUMP}" -h "${exe}"
    RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT status EQUAL 0 OR NOT out MATCHES "file format ${FORMAT}\n")
    message(FATAL_ERROR "the executable for ${TARGET} is not ${FORMAT}\n${out}${err}")
endif()
