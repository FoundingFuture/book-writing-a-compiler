# Run antic on a program that it refuses and compare its standard error
# byte for byte with an expected file. Run with cmake -P and these values:
#   ANTIC      the antic executable
#   DIRECTORY  the directory to run in, so that messages name the file alone
#   COMMAND    the arguments, separated by commas
#   EXPECTED   the file with the expected messages

string(REPLACE "," ";" arguments "${COMMAND}")
execute_process(
    COMMAND "${ANTIC}" ${arguments}
    WORKING_DIRECTORY "${DIRECTORY}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
if(status EQUAL 0)
    message(FATAL_ERROR "antic accepted the program\n${out}")
endif()
file(READ "${EXPECTED}" expected)
if(NOT err STREQUAL expected)
    message(FATAL_ERROR "messages differ from ${EXPECTED}\ngot:\n${err}")
endif()
