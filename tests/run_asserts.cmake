# The five behaviours of `assert`. Run with cmake -P and these values:
#   ANTIC     the antic executable
#   LLVM_MC   the llvm-mc executable
#   RUNTIME   the runtime archive
#   SOURCE    a program whose assertion fails
#   WORK      a directory for the files
#   STRINGS   a tool that lists the strings of a file
#
# A release build runs the program to its end and carries no text of the
# assertion. A dev build, and any build with --asserts, prints the text
# and aborts. --no-asserts drops them in dev mode too.

function(build name)
    execute_process(COMMAND "${ANTIC}" ${ARGN} --llvm-mc "${LLVM_MC}"
                            --runtime "${RUNTIME}" -o "${WORK}/${name}"
                            "${SOURCE}"
                    RESULT_VARIABLE status ERROR_VARIABLE err)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "antic failed for ${name}\n${err}")
    endif()
endfunction()

file(MAKE_DIRECTORY "${WORK}")

# Release is the default, so the assertion is gone.
build(release)
execute_process(COMMAND "${WORK}/release" RESULT_VARIABLE code
                OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 7)
    message(FATAL_ERROR "the release build exited with ${code}, expected 7"
                        "\n${out}${err}")
endif()
execute_process(COMMAND "${STRINGS}" "${WORK}/release" OUTPUT_VARIABLE text)
if(text MATCHES "assertion failed")
    message(FATAL_ERROR "the release build carries the text of an assertion")
endif()

# --asserts overrides the mode, and the failure names file, line and the
# source of the condition.
build(forced --asserts)
execute_process(COMMAND "${WORK}/forced" RESULT_VARIABLE code
                ERROR_VARIABLE err)
if(code EQUAL 7)
    message(FATAL_ERROR "the assertion did not stop the program")
endif()
if(NOT err MATCHES "asserts\\.anti:[0-9]+: assertion failed: n > 0")
    message(FATAL_ERROR "the failure printed `${err}`")
endif()

# Dev mode keeps them without a flag, and --no-asserts drops them.
execute_process(COMMAND "${ANTIC}" --dev -S -o "${WORK}/dev.s" "${SOURCE}"
                RESULT_VARIABLE status)
file(READ "${WORK}/dev.s" dev)
if(NOT status EQUAL 0 OR NOT dev MATCHES "assert_failed")
    message(FATAL_ERROR "dev mode dropped the assertion")
endif()
execute_process(COMMAND "${ANTIC}" --dev --no-asserts -S
                        -o "${WORK}/off.s" "${SOURCE}" RESULT_VARIABLE status)
file(READ "${WORK}/off.s" off)
if(NOT status EQUAL 0 OR off MATCHES "assert_failed")
    message(FATAL_ERROR "--no-asserts kept the assertion")
endif()

# DESIGN: the decision belongs to the build that compiles the program. A
# library file carries the assertion, and the same library gives a release
# program without it and a program built with --asserts one that traps.
get_filename_component(here "${SOURCE}" DIRECTORY)
file(MAKE_DIRECTORY "${WORK}/root/com/example" "${WORK}/lib/com/example")
file(WRITE "${WORK}/root/com/example/checked.anti"
     "pub fn halve(n: int) -> int\n{\n\tassert(n > 0);\n\treturn n / 2;\n}\n")
execute_process(COMMAND "${ANTIC}" -c -I "${WORK}/root"
                        -o "${WORK}/lib/com/example/checked.antl"
                        "${WORK}/root/com/example/checked.anti"
                RESULT_VARIABLE status ERROR_VARIABLE err)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "antic -c failed\n${err}")
endif()
file(WRITE "${WORK}/consumer.anti"
     "import com.example.checked;\n\nfn main() -> int\n{\n\treturn checked.halve(0);\n}\n")
foreach(case "quiet;" "trap;--asserts")
    string(REPLACE ";" " " ignored "${case}")
    list(GET case 0 name)
    list(LENGTH case parts)
    set(flag "")
    if(parts GREATER 1)
        list(GET case 1 flag)
    endif()
    execute_process(COMMAND "${ANTIC}" ${flag} --llvm-mc "${LLVM_MC}"
                            --runtime "${RUNTIME}" -I "${WORK}/lib"
                            -o "${WORK}/${name}" "${WORK}/consumer.anti"
                    RESULT_VARIABLE status ERROR_VARIABLE err)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "antic failed for ${name}\n${err}")
    endif()
    execute_process(COMMAND "${WORK}/${name}" RESULT_VARIABLE code
                    ERROR_VARIABLE ran)
    execute_process(COMMAND "${STRINGS}" "${WORK}/${name}"
                    OUTPUT_VARIABLE text)
    if(name STREQUAL "quiet")
        if(NOT code EQUAL 0 OR text MATCHES "assertion failed")
            message(FATAL_ERROR "the library assertion reached a release "
                                "program, exit ${code}")
        endif()
    elseif(NOT ran MATCHES "assertion failed: n > 0")
        message(FATAL_ERROR "--asserts did not keep the library assertion"
                            "\n${ran}")
    endif()
endforeach()
