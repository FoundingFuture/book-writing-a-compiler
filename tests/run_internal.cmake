# The internal level of a module item. A module of the same package sees
# an internal function, and a module of another package does not. Run
# with cmake -P and these values:
#   ANTIC     the antic executable
#   LLVM_MC   the llvm-mc executable
#   RUNTIME   the runtime directory
#   ROOT      the search root that holds the sources
#   WORK      a directory for the outputs

file(MAKE_DIRECTORY "${WORK}")
execute_process(
    COMMAND "${ANTIC}" -c -I "${ROOT}" --package-name com.example
            -o "${WORK}/core.antl" "${ROOT}/com/example/core.anti"
    RESULT_VARIABLE status ERROR_VARIABLE err)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "antic -c failed with ${status}\n${err}")
endif()
# A module of the same package compiles against it.
execute_process(
    COMMAND "${ANTIC}" -c -I "${ROOT}" --package-name com.example
            -o "${WORK}/near.antl" "${ROOT}/com/example/near.anti"
            "${WORK}/core.antl"
    RESULT_VARIABLE status ERROR_VARIABLE err)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "the package cannot see its own internal item\n${err}")
endif()
# A module of another package does not.
execute_process(
    COMMAND "${ANTIC}" -S -I "${ROOT}" -o "${WORK}/far.s"
            "${ROOT}/far.anti" "${WORK}/core.antl"
    RESULT_VARIABLE status ERROR_VARIABLE err)
if(status EQUAL 0)
    message(FATAL_ERROR "another package reached the internal item")
endif()
if(NOT err MATCHES "has no public item `tuning`")
    message(FATAL_ERROR "unexpected message\n${err}")
endif()
