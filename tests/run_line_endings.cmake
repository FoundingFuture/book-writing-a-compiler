# A test compares the output of a program with the bytes of its expected
# file. A carriage return in either one fails that comparison, and a
# checkout on Windows adds one to every line of a text file unless
# .gitattributes forbids it.
#
#   cmake -DROOT=<repository> -P tests/run_line_endings.cmake

file(READ "${ROOT}/.gitattributes" rules)
foreach(rule "* text=auto eol=lf")
    string(FIND "${rules}" "${rule}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR ".gitattributes has no rule `${rule}`, so a "
                            "checkout on Windows may write carriage returns")
    endif()
endforeach()

file(GLOB_RECURSE files "${ROOT}/tests/programs/*.expected"
     "${ROOT}/tests/programs/*.anti" "${ROOT}/tests/std/*.expected")
foreach(file IN LISTS files)
    file(READ "${file}" text HEX)
    if(text MATCHES "0d0a")
        get_filename_component(name "${file}" NAME)
        message(FATAL_ERROR "${name} holds a carriage return before a line "
                            "feed, which no test of it can match")
    endif()
endforeach()
