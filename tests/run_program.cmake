# Compile one test program with antic, run it and compare the result with
# the expected file beside the source. Run with cmake -P and these values:
#   ANTIC     the antic executable
#   LLVM_MC   the llvm-mc executable
#   RUNTIME   the runtime directory, holding lib/<target>/libanti_rt.a
#   SOURCE    the .anti file
#   WORK      a directory for the executable and intermediate files
#   OBJECTS   optional object files to link, separated by commas
#   OPTIONS   optional options of antic, separated by commas
#
# The expected file starts with the line "exit N", the process exit code.
# Every byte after that line is the expected standard output. An optional
# NAME.args file beside the source holds one command-line argument per line.

if(NOT EXISTS "${LLVM_MC}")
    message(FATAL_ERROR
        "llvm-mc not found at '${LLVM_MC}'. Configure with -DANTIC_LLVM_MC=<path>.")
endif()

get_filename_component(name "${SOURCE}" NAME_WE)
get_filename_component(dir "${SOURCE}" DIRECTORY)
set(expected_file "${dir}/${name}.expected")
set(exe "${WORK}/${name}")
file(MAKE_DIRECTORY "${WORK}")

string(REPLACE "," ";" objects "${OBJECTS}")
string(REPLACE "," ";" options "${OPTIONS}")
execute_process(
    COMMAND "${ANTIC}" --llvm-mc "${LLVM_MC}" --runtime "${RUNTIME}" ${options}
            -o "${exe}" "${SOURCE}" ${objects}
    RESULT_VARIABLE status
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "antic failed with ${status}\n${out}${err}")
endif()
# Warnings are errors: antic, llvm-mc and the linker must print nothing.
if(NOT out STREQUAL "" OR NOT err STREQUAL "")
    message(FATAL_ERROR "antic printed output\n${out}${err}")
endif()

set(program_args "")
if(EXISTS "${dir}/${name}.args")
    file(STRINGS "${dir}/${name}.args" program_args ENCODING UTF-8)
endif()
execute_process(
    COMMAND "${exe}" ${program_args}
    RESULT_VARIABLE exit_code
    OUTPUT_VARIABLE stdout)

file(READ "${expected_file}" expected)
if(NOT expected MATCHES "^exit ([0-9]+)\n")
    message(FATAL_ERROR "${expected_file} does not start with 'exit N'")
endif()
set(expected_exit "${CMAKE_MATCH_1}")
string(REGEX REPLACE "^exit [0-9]+\n" "" expected_stdout "${expected}")

if(NOT exit_code STREQUAL expected_exit)
    message(FATAL_ERROR "exit code ${exit_code}, expected ${expected_exit}")
endif()
if(NOT stdout STREQUAL expected_stdout)
    message(FATAL_ERROR "standard output differs\nexpected:\n${expected_stdout}\ngot:\n${stdout}")
endif()
