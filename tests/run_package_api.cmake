# The installers drive the CMake scripts that a package carries, so both
# sides agree on one number. tools/package-api holds it, a package carries
# a copy, and each installer names the number it needs.
#
#   cmake -DROOT=<repository> -P tests/run_package_api.cmake

file(READ "${ROOT}/tools/package-api" api)
string(STRIP "${api}" api)
if(NOT api MATCHES "^[0-9]+$")
    message(FATAL_ERROR "tools/package-api is `${api}`, not a number")
endif()

foreach(installer install.sh install.ps1)
    file(STRINGS "${ROOT}/tools/${installer}" line
         REGEX "PACKAGE_API *= *[0-9]+")
    if(line STREQUAL "")
        message(FATAL_ERROR "tools/${installer} names no PACKAGE_API")
    endif()
    string(REGEX REPLACE "^.*PACKAGE_API *= *([0-9]+).*$" "\\1" needs "${line}")
    if(NOT needs EQUAL api)
        message(FATAL_ERROR "tools/${installer} needs package API ${needs}, "
                            "and tools/package-api holds ${api}. Both rise "
                            "together, or an installer drives a package that "
                            "does not know its options.")
    endif()
endforeach()

# The packer puts the file into the package, where the installer reads it.
file(READ "${ROOT}/tools/pack-anti.cmake" packer)
if(NOT packer MATCHES "package-api")
    message(FATAL_ERROR "tools/pack-anti.cmake leaves package-api out of "
                        "the package, so no installer can read it")
endif()
