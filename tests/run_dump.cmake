# Run antic with a dump option and compare its standard output byte for
# byte with an expected file. Run with cmake -P and these values:
#   ANTIC     the antic executable
#   OPTION    the dump option, such as --dump-tokens
#   SOURCE    the .anti file
#   EXPECTED  the file with the expected output
#   LIBRARIES optional library files, separated by commas

#   COMMAND   instead of OPTION and SOURCE, all arguments separated by commas
#   OUTPUT    optional file that the command writes, compared instead of
#             the standard output, which must then be empty

string(REPLACE "," ";" libraries "${LIBRARIES}")
if(DEFINED COMMAND)
    string(REPLACE "," ";" arguments "${COMMAND}")
else()
    set(arguments "${OPTION}" "${SOURCE}" ${libraries})
endif()
execute_process(
    COMMAND "${ANTIC}" ${arguments}
    RESULT_VARIABLE status
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
if(NOT status EQUAL 0 OR NOT err STREQUAL "")
    message(FATAL_ERROR "antic ${OPTION} failed with ${status}\n${err}")
endif()
file(READ "${EXPECTED}" expected)
if(DEFINED OUTPUT)
    if(NOT out STREQUAL "")
        message(FATAL_ERROR "antic printed output\n${out}")
    endif()
    file(READ "${OUTPUT}" out)
endif()
if(NOT out STREQUAL expected)
    message(FATAL_ERROR "output differs from ${EXPECTED}\ngot:\n${out}")
endif()
