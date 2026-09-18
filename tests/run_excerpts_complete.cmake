# Every C and CMake fence of the book stands in a file of the repository.
# A fence that no file holds is left out of tests/excerpts.txt, and the
# test of it then stops running rather than failing. This test runs the
# script that writes the index and fails when it drops one.
#
#   cmake -DROOT=<repository> -DPYTHON=<python3> \
#         -P tests/run_excerpts_complete.cmake

execute_process(COMMAND "${PYTHON}" "${ROOT}/tools/scripts/excerpts.py"
                RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "a code listing of the book is in no file of the "
                        "repository, so no test reads it any more:\n${out}${err}")
endif()
