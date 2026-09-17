# The ABI probe on the host: compile probe.c with the C compiler and
# probe.anti with antic, run both and compare their output byte for byte.
# Run with cmake -P and these values:
#   ANTIC     the antic executable
#   LLVM_MC   the llvm-mc executable
#   RUNTIME   the runtime directory
#   SOURCES   tests/abi
#   WORK      a directory for the executables

file(MAKE_DIRECTORY "${WORK}")
execute_process(COMMAND cc -std=c11 -o "${WORK}/probe_c" "${SOURCES}/probe.c"
    RESULT_VARIABLE status ERROR_VARIABLE err)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "cc probe.c failed\n${err}")
endif()
execute_process(
    COMMAND "${ANTIC}" --llvm-mc "${LLVM_MC}" --runtime "${RUNTIME}"
            -o "${WORK}/probe_anti" "${SOURCES}/probe.anti"
    RESULT_VARIABLE status ERROR_VARIABLE err)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "antic probe.anti failed\n${err}")
endif()
execute_process(COMMAND "${WORK}/probe_c" OUTPUT_VARIABLE from_c)
execute_process(COMMAND "${WORK}/probe_anti" OUTPUT_VARIABLE from_anti)
if(NOT from_c STREQUAL from_anti)
    message(FATAL_ERROR "the layouts differ\nC:\n${from_c}\nAnti:\n${from_anti}")
endif()
