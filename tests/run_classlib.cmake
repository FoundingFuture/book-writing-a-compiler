# Compile a library whose interface holds a class and an enum, then a
# program that uses it, and run the program. Run with cmake -P and these
# values:
#   ANTIC     the antic executable
#   LLVM_MC   the llvm-mc executable
#   RUNTIME   the runtime directory
#   ROOT      the search root that holds the sources
#   WORK      a directory for the outputs

file(MAKE_DIRECTORY "${WORK}")
execute_process(
    COMMAND "${ANTIC}" -c -I "${ROOT}" -o "${WORK}/shapes.antl"
            "${ROOT}/com/example/shapes.anti"
    RESULT_VARIABLE status ERROR_VARIABLE err)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "antic -c failed with ${status}\n${err}")
endif()
execute_process(
    COMMAND "${ANTIC}" -I "${ROOT}" --llvm-mc "${LLVM_MC}"
            --runtime "${RUNTIME}" -o "${WORK}/client" "${ROOT}/client.anti"
            "${WORK}/shapes.antl"
    RESULT_VARIABLE status ERROR_VARIABLE err)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "antic failed with ${status}\n${err}")
endif()
execute_process(COMMAND "${WORK}/client"
    RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "the program exited with ${status}\n${err}")
endif()
if(NOT out STREQUAL "5 0 25\n200 1\n")
    message(FATAL_ERROR "the program printed\n${out}")
endif()
