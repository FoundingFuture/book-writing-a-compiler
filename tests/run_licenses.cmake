# Link a program and find the licence notice anti_licenses in it by its
# markers. Run with cmake -P and these values:
#   ANTIC     the antic executable
#   LLVM_MC   the llvm-mc executable
#   RUNTIME   the runtime directory
#   SOURCE    the .anti file
#   WORK      a directory for the executable
#   WANTED    optional list of patterns for the marker and package lines
#   EXPECTED  optional file with the whole notice that the program holds

file(MAKE_DIRECTORY "${WORK}")
execute_process(
    COMMAND "${ANTIC}" --llvm-mc "${LLVM_MC}" --runtime "${RUNTIME}"
            --package-name com.example.hello --package-version 2.0.0
            --license MIT -o "${WORK}/licensed" "${SOURCE}"
    RESULT_VARIABLE status ERROR_VARIABLE err)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "antic failed\n${err}")
endif()
file(STRINGS "${WORK}/licensed" lines REGEX "ANTI_LICENSES|^package ")
set(wanted "ANTI_LICENSES_BEGIN;package anti.rt [0-9.]+ 0BSD;package com.example.hello 2.0.0 MIT")
if(DEFINED WANTED)
    string(REPLACE "|" ";" wanted "${WANTED}")
endif()
string(JOIN ";" got ${lines})
if(NOT got MATCHES "^${wanted}")
    message(FATAL_ERROR "the notice in the program is\n${got}")
endif()
if(DEFINED EXPECTED)
    file(READ "${WORK}/licensed" program HEX)
    file(READ "${EXPECTED}" notice HEX)
    string(FIND "${program}" "${notice}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "the program does not hold the notice of ${EXPECTED}")
    endif()
endif()
