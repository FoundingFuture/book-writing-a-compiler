# Compile a library with antic -c and compare its bytes with a hex listing,
# 16 bytes per line. Run with cmake -P and these values:
#   ANTIC     the antic executable
#   SOURCE    the .anti file
#   ROOT      the search root that holds SOURCE
#   OPTIONS   more options of antic, separated by |
#   OUTPUT    the .antl file to write
#   EXPECTED  the hex listing

string(REPLACE "|" ";" options "${OPTIONS}")
execute_process(
    COMMAND "${ANTIC}" -c -I "${ROOT}" ${options} -o "${OUTPUT}" "${SOURCE}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
if(NOT status EQUAL 0 OR NOT out STREQUAL "" OR NOT err STREQUAL "")
    message(FATAL_ERROR "antic -c failed with ${status}\n${out}${err}")
endif()
file(READ "${OUTPUT}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "\\1 " hex "${hex}")
string(REGEX REPLACE "(([0-9a-f][0-9a-f] )([0-9a-f][0-9a-f] )([0-9a-f][0-9a-f] )([0-9a-f][0-9a-f] )([0-9a-f][0-9a-f] )([0-9a-f][0-9a-f] )([0-9a-f][0-9a-f] )([0-9a-f][0-9a-f] )([0-9a-f][0-9a-f] )([0-9a-f][0-9a-f] )([0-9a-f][0-9a-f] )([0-9a-f][0-9a-f] )([0-9a-f][0-9a-f] )([0-9a-f][0-9a-f] )([0-9a-f][0-9a-f] )([0-9a-f][0-9a-f] ))" "\\1\n" hex "${hex}")
string(REGEX REPLACE " \n" "\n" hex "${hex}")
string(REGEX REPLACE " $" "\n" hex "${hex}")
file(READ "${EXPECTED}" expected)
if(NOT hex STREQUAL expected)
    message(FATAL_ERROR "${OUTPUT} differs from ${EXPECTED}\ngot:\n${hex}")
endif()
