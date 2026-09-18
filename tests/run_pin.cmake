# A version lives in tools/<name>-version and the archive of each host in
# tools/<name>-pin, which spells the version as @VERSION@. No copy of
# either can drift, and every archive comes over HTTPS.
#
#   cmake -DROOT=<repository> -DNAME=llvm|cmake -P tests/run_llvm_pin.cmake

set(hosts macos-arm64 linux-x86_64 linux-arm64 windows-x86_64
          windows-arm64)
if(NAME STREQUAL "cmake")
    list(APPEND hosts macos-x86_64)
endif()
set(pin "${ROOT}/tools/${NAME}-pin")
set(script "${ROOT}/tools/get-${NAME}.cmake")
file(READ "${ROOT}/tools/${NAME}-version" version)
string(STRIP "${version}" version)
if(NOT version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "tools/${NAME}-version is `${version}`")
endif()

foreach(host IN LISTS hosts)
    file(STRINGS "${pin}" url REGEX "^${host}-url=")
    file(STRINGS "${pin}" digest REGEX "^${host}-digest=")
    string(REGEX REPLACE "^${host}-url=" "" url "${url}")
    string(REGEX REPLACE "^${host}-digest=" "" digest "${digest}")
    if(NOT url MATCHES "@VERSION@")
        message(FATAL_ERROR "${host} has no archive with @VERSION@ in it")
    endif()
    # A user downloads over HTTPS and has no other way in.
    if(NOT url MATCHES "^https://")
        message(FATAL_ERROR "${host} is served from `${url}`, not over HTTPS")
    endif()
    string(LENGTH "${digest}" length)
    if(NOT digest MATCHES "^[0-9a-f]+$" OR NOT length EQUAL 64)
        message(FATAL_ERROR "${host} has the digest `${digest}`")
    endif()
endforeach()

file(READ "${pin}" text)
if(text MATCHES "${version}")
    message(FATAL_ERROR "tools/${NAME}-pin spells ${version}, which belongs "
                        "in tools/${NAME}-version alone")
endif()
if(NOT EXISTS "${script}")
    return()
endif()
file(READ "${script}" text)
if(text MATCHES "${version}" OR text MATCHES "https://")
    message(FATAL_ERROR "tools/get-${NAME}.cmake spells a version or a URL, "
                        "which belong in tools/${NAME}-version and "
                        "tools/${NAME}-pin")
endif()
