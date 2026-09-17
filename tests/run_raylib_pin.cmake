# The raylib version and digest live in tools/raylib-pin alone. The
# download script reads them from there, so no second copy can drift.
#
#   cmake -DROOT=<repository> -P tests/run_raylib_pin.cmake

set(pin "${ROOT}/tools/raylib-pin")
set(script "${ROOT}/tools/get-raylib.cmake")
foreach(file "${pin}" "${script}")
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "${file} is missing")
    endif()
endforeach()

file(STRINGS "${pin}" version REGEX "^RAYLIB_VERSION=")
file(STRINGS "${pin}" digest REGEX "^RAYLIB_DIGEST=")
string(REGEX REPLACE "^RAYLIB_VERSION=" "" version "${version}")
string(REGEX REPLACE "^RAYLIB_DIGEST=" "" digest "${digest}")
if(NOT version MATCHES "^[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "RAYLIB_VERSION is `${version}`, expected a version")
endif()
string(LENGTH "${digest}" length)
if(NOT digest MATCHES "^[0-9a-f]+$" OR NOT length EQUAL 64)
    message(FATAL_ERROR "RAYLIB_DIGEST is `${digest}`, expected 64 hex digits")
endif()

# A user downloads over HTTPS and has no other way in.
file(STRINGS "${script}" urls REGEX "://")
foreach(line IN LISTS urls)
    if(NOT line MATCHES "https://")
        message(FATAL_ERROR "tools/get-raylib.cmake reads `${line}`, "
                            "which is not HTTPS")
    endif()
endforeach()

file(READ "${script}" text)
foreach(literal "${version}" "${digest}")
    if(text MATCHES "${literal}")
        message(FATAL_ERROR "tools/get-raylib.cmake spells `${literal}`, "
                            "which belongs in tools/raylib-pin alone")
    endif()
endforeach()
