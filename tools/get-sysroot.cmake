# Install the sysroot of each target that lld links against into
# <dir>/<target>/, and the licence of each component into <dir>/licenses/.
#
#   cmake -DDEST=<dir> -DLLVM_BIN=<dir> -DTARGETS=<target>[;<target>]
#         [-DACCEPT_LICENSE=yes] -P tools/get-sysroot.cmake
#
# linux-x86_64, linux-arm64: musl and the compiler-rt builtins from the
#   Alpine packages of tools/sysroot-pins, checked against their digests.
# macos-arm64, macos-x86_64: the .tbd stubs of libSystem from the newest
#   SDK of the Command Line Tools for Xcode that ld64.lld in LLVM_BIN
#   reads. Only on a Mac, because Apple licenses the SDK for its own
#   hardware.
# windows-x86_64, windows-arm64: the MSVC CRT and the Windows SDK import
#   libraries, which xwin downloads at the versions of tools/sysroot-pins.
#   Microsoft licenses them to the user, so the script runs xwin only with
#   ACCEPT_LICENSE=yes. It installs the pinned xwin when the path has none.
cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED DEST OR NOT DEFINED LLVM_BIN OR NOT DEFINED TARGETS)
    message(FATAL_ERROR "usage: cmake -DDEST=<dir> -DLLVM_BIN=<dir> "
                        "-DTARGETS=<target>[;<target>] "
                        "[-DACCEPT_LICENSE=yes] -P tools/get-sysroot.cmake")
endif()

set(tools_dir "${CMAKE_CURRENT_LIST_DIR}")
file(STRINGS "${tools_dir}/sysroot-pins" pins REGEX "^[A-Z]")
foreach(line IN LISTS pins)
    string(REGEX REPLACE "^([^=]+)=(.*)$" "\\1;\\2" pair "${line}")
    list(GET pair 0 key)
    list(GET pair 1 value)
    set("${key}" "${value}")
endforeach()
file(MAKE_DIRECTORY "${DEST}/licenses")

# SPLAT is empty for a run that does everything, script for one that
# writes the xwin command and stops, and done for one that checks the
# tree after the caller ran it.
if(NOT DEFINED SPLAT)
    set(SPLAT "")
endif()
set(SPLAT_SCRIPT "${DEST}/.download/splat.sh")
if(CMAKE_HOST_WIN32)
    set(SPLAT_SCRIPT "${DEST}/.download/splat.cmd")
endif()
if(SPLAT STREQUAL "script")
    file(MAKE_DIRECTORY "${DEST}/.download")
    file(WRITE "${SPLAT_SCRIPT}" "")
endif()

# Download url to file and stop unless its digest is the expected one.
function(fetch url file digest)
    if(NOT EXISTS "${file}")
        file(DOWNLOAD "${url}" "${file}" STATUS status SHOW_PROGRESS)
        list(GET status 0 code)
        list(GET status 1 text)
        if(NOT code EQUAL 0)
            file(REMOVE "${file}")
            message(FATAL_ERROR "${url}: ${text}")
        endif()
    endif()
    file(SHA256 "${file}" actual)
    if(NOT actual STREQUAL digest)
        message(FATAL_ERROR "${file}: SHA-256 ${actual}, expected ${digest}")
    endif()
endfunction()

# The digest of the regular files under dir: their SHA-256 lines in the
# order of their paths, hashed once more. xwin adds symbolic links for
# other spellings on a case-sensitive file system, so links stay out.
function(tree_digest dir out)
    file(GLOB_RECURSE found LIST_DIRECTORIES false RELATIVE "${dir}"
         "${dir}/*")
    list(SORT found)
    set(lines "")
    foreach(name IN LISTS found)
        if(NOT IS_SYMLINK "${dir}/${name}")
            file(SHA256 "${dir}/${name}" one)
            string(APPEND lines "${one}  ./${name}\n")
        endif()
    endforeach()
    string(SHA256 digest "${lines}")
    set("${out}" "${digest}" PARENT_SCOPE)
endfunction()

function(linux_sysroot target arch musl_digest rt_digest)
    set(root "${DEST}/${target}")
    set(work "${DEST}/.download/${target}")
    file(MAKE_DIRECTORY "${work}" "${root}/usr/lib")
    fetch("${ALPINE_URL}/${arch}/musl-dev-${MUSL_APK}.apk"
          "${work}/musl-dev.apk" "${musl_digest}")
    fetch("${ALPINE_URL}/${arch}/compiler-rt-${COMPILER_RT_APK}.apk"
          "${work}/compiler-rt.apk" "${rt_digest}")
    file(REMOVE_RECURSE "${work}/usr" "${root}/usr/include")
    file(ARCHIVE_EXTRACT INPUT "${work}/musl-dev.apk" DESTINATION "${work}"
         PATTERNS "usr/include/*" "usr/lib/*")
    file(COPY "${work}/usr/include" DESTINATION "${root}/usr")
    foreach(name crt1.o crti.o crtn.o rcrt1.o Scrt1.o libc.a)
        file(COPY "${work}/usr/lib/${name}" DESTINATION "${root}/usr/lib")
    endforeach()
    set(builtins
        "usr/lib/llvm${COMPILER_RT_MAJOR}/lib/clang/${COMPILER_RT_MAJOR}/lib/${arch}-alpine-linux-musl/libclang_rt.builtins-${arch}.a")
    file(ARCHIVE_EXTRACT INPUT "${work}/compiler-rt.apk" DESTINATION "${work}"
         PATTERNS "${builtins}")
    file(COPY_FILE "${work}/${builtins}"
         "${root}/usr/lib/libclang_rt.builtins.a")
    fetch("${MUSL_SOURCE_URL}" "${DEST}/.download/musl.tar.gz"
          "${MUSL_SOURCE_DIGEST}")
    file(ARCHIVE_EXTRACT INPUT "${DEST}/.download/musl.tar.gz"
         DESTINATION "${DEST}/.download"
         PATTERNS "musl-${MUSL_VERSION}/COPYRIGHT")
    file(COPY_FILE "${DEST}/.download/musl-${MUSL_VERSION}/COPYRIGHT"
         "${DEST}/licenses/musl.txt")
    fetch("${COMPILER_RT_LICENSE_URL}" "${DEST}/licenses/compiler-rt.txt"
          "${COMPILER_RT_LICENSE_DIGEST}")
endfunction()

function(macos_sysroot target arch)
    if(NOT CMAKE_HOST_APPLE)
        message(FATAL_ERROR "${target}: Apple licenses the SDK for its own "
                            "hardware, so the stubs come from a Mac")
    endif()
    set(sdks "/Library/Developer/CommandLineTools/SDKs")
    file(GLOB found "${sdks}/MacOSX[0-9]*.[0-9]*.sdk")
    if(found STREQUAL "")
        message(FATAL_ERROR "${sdks} holds no SDK. Run xcode-select --install")
    endif()
    list(SORT found COMPARE NATURAL ORDER DESCENDING)
    set(chosen "")
    foreach(path IN LISTS found)
        string(REGEX REPLACE ".*MacOSX(.*)\\.sdk$" "\\1" version "${path}")
        execute_process(
            COMMAND "${LLVM_BIN}/ld64.lld" -arch "${arch}"
                    -platform_version macos 11.0 "${version}"
                    -syslibroot "${path}" -dylib -o "${DEST}/.probe.dylib"
                    -lSystem
            RESULT_VARIABLE linked OUTPUT_QUIET ERROR_QUIET)
        if(linked EQUAL 0)
            set(chosen "${version}")
            set(chosen_path "${path}")
            break()
        endif()
    endforeach()
    file(REMOVE "${DEST}/.probe.dylib")
    if(chosen STREQUAL "")
        message(FATAL_ERROR "no SDK of the Command Line Tools links with "
                            "${LLVM_BIN}/ld64.lld")
    endif()
    set(root "${DEST}/${target}")
    file(REMOVE_RECURSE "${root}")
    file(MAKE_DIRECTORY "${root}/usr/lib")
    file(COPY "${chosen_path}/usr/lib/libSystem.tbd"
              "${chosen_path}/usr/lib/libSystem.B.tbd"
              "${chosen_path}/usr/lib/system"
         DESTINATION "${root}/usr/lib")
    file(WRITE "${root}/sdk-version" "${chosen}\n")
    file(WRITE "${DEST}/licenses/macos-sdk.txt"
"The .tbd stubs in sysroot/macos-arm64 and sysroot/macos-x86_64 are copied
from MacOSX${chosen}.sdk of the Command Line Tools for Xcode on the build Mac.
Apple distributes that SDK under the Xcode and Apple SDKs Agreement.
")
endfunction()

# The xwin program of the pin, from the path or from its own release.
function(xwin_program out)
    find_program(found xwin)
    if(found)
        execute_process(COMMAND "${found}" --version
                        OUTPUT_VARIABLE text OUTPUT_STRIP_TRAILING_WHITESPACE)
        string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" version "${text}")
        if(version STREQUAL XWIN_VERSION)
            set("${out}" "${found}" PARENT_SCOPE)
            return()
        endif()
    endif()
    cmake_host_system_information(RESULT os QUERY OS_NAME)
    cmake_host_system_information(RESULT cpu QUERY OS_PLATFORM)
    string(TOLOWER "${os}" os)
    string(TOLOWER "${cpu}" cpu)
    if(cpu MATCHES "^(arm64|aarch64)$")
        set(cpu arm64)
    elseif(cpu MATCHES "^(x86_64|amd64|x64)$")
        set(cpu x86_64)
    endif()
    set(asset "${XWIN_BIN_${os}-${cpu}}")
    set(digest "${XWIN_BIN_${os}-${cpu}_DIGEST}")
    if(asset STREQUAL "" AND os STREQUAL "windows")
        # xwin publishes no build for Windows on ARM64, and Windows runs an
        # x64 program there under emulation.
        set(asset "${XWIN_BIN_windows-x86_64}")
        set(digest "${XWIN_BIN_windows-x86_64_DIGEST}")
    endif()
    if(asset STREQUAL "")
        message(FATAL_ERROR
                "xwin ${XWIN_VERSION} publishes no build for ${os}-${cpu}. "
                "Run cargo install xwin --locked --version ${XWIN_VERSION}")
    endif()
    set(work "${DEST}/.download/xwin-bin")
    file(MAKE_DIRECTORY "${work}")
    fetch("${XWIN_BIN_URL}/${asset}" "${work}/${asset}" "${digest}")
    file(ARCHIVE_EXTRACT INPUT "${work}/${asset}" DESTINATION "${work}")
    file(GLOB_RECURSE program "${work}/*/xwin" "${work}/*/xwin.exe")
    if(program STREQUAL "")
        message(FATAL_ERROR "${asset} holds no xwin program")
    endif()
    list(GET program 0 program)
    set("${out}" "${program}" PARENT_SCOPE)
endfunction()

# The CRT and the SDK that the Build Tools of Visual Studio installed on
# this machine. A junction needs no privilege where a symbolic link does,
# so the sysroot points at them and downloads nothing. Returns the path of
# the installation, or an empty string when the machine has none.
function(build_tools out)
    set("${out}" "" PARENT_SCOPE)
    if(NOT CMAKE_HOST_WIN32)
        return()
    endif()
    set(where "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer/vswhere.exe")
    if(DEFINED VSWHERE)
        set(where "${VSWHERE}")
    endif()
    if(NOT EXISTS "${where}")
        return()
    endif()
    execute_process(COMMAND "${where}" -nologo -latest -products *
                            -property installationPath
                    OUTPUT_VARIABLE found OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    string(REGEX REPLACE "\r?\n.*$" "" found "${found}")
    if(found STREQUAL "" OR NOT IS_DIRECTORY "${found}")
        return()
    endif()
    set("${out}" "${found}" PARENT_SCOPE)
endfunction()

# The newest directory under root, by version order.
function(newest root out)
    file(GLOB found "${root}/*")
    set(directories "")
    foreach(path IN LISTS found)
        if(IS_DIRECTORY "${path}")
            list(APPEND directories "${path}")
        endif()
    endforeach()
    list(SORT directories COMPARE NATURAL ORDER DESCENDING)
    list(GET directories 0 first)
    set("${out}" "${first}" PARENT_SCOPE)
endfunction()

# Point link at target with a junction, which any account may create.
function(junction link target)
    if(NOT IS_DIRECTORY "${target}")
        message(FATAL_ERROR "${target} is missing, so the sysroot has no "
                            "${link}")
    endif()
    get_filename_component(parent "${link}" DIRECTORY)
    file(MAKE_DIRECTORY "${parent}")
    file(TO_NATIVE_PATH "${link}" from)
    file(TO_NATIVE_PATH "${target}" to)
    execute_process(COMMAND cmd /c mklink /J "${from}" "${to}"
                    RESULT_VARIABLE made OUTPUT_QUIET
                    ERROR_VARIABLE complaint)
    if(NOT made EQUAL 0)
        message(FATAL_ERROR "cannot join ${from} to ${to}: ${complaint}")
    endif()
endfunction()

# Lay the sysroot of a Windows target over the local Build Tools.
function(windows_local target arch install)
    set(root "${DEST}/${target}")
    set(ms x64)
    if(arch STREQUAL "aarch64")
        set(ms arm64)
    endif()
    newest("${install}/VC/Tools/MSVC" msvc)
    set(kits "$ENV{ProgramFiles\(x86\)}/Windows Kits/10")
    if(DEFINED WINDOWS_KITS)
        set(kits "${WINDOWS_KITS}")
    endif()
    newest("${kits}/Include" headers)
    get_filename_component(sdk_version "${headers}" NAME)

    file(REMOVE_RECURSE "${root}")
    junction("${root}/crt/include" "${msvc}/include")
    junction("${root}/crt/lib/${arch}" "${msvc}/lib/${ms}")
    foreach(part ucrt um shared)
        junction("${root}/sdk/include/${part}" "${headers}/${part}")
    endforeach()
    foreach(part ucrt um)
        junction("${root}/sdk/lib/${part}/${arch}"
                 "${kits}/Lib/${sdk_version}/${part}/${ms}")
    endforeach()
    file(WRITE "${DEST}/licenses/windows-sdk.txt"
"The sysroot of ${target} points at the Microsoft C runtime and Windows SDK
${sdk_version} that the Build Tools installed on this machine, under
${install}. Nothing of Microsoft is copied or redistributed.
")
    message(STATUS "${target}: over the Build Tools in ${install}")
endfunction()

# Append the splat of one target to the script that the caller runs. A
# shell gives xwin the terminal that its progress bar looks for.
function(write_splat program target arch options)
    set(quoted "")
    foreach(argument "${program}" --accept-license --cache-dir
            "${DEST}/.download/xwin" --arch "${arch}" --crt-version
            "${XWIN_CRT_VERSION}" --sdk-version "${XWIN_SDK_VERSION}" splat
            --output "${DEST}/${target}" ${options})
        if(CMAKE_HOST_WIN32)
            file(TO_NATIVE_PATH "${argument}" argument)
        endif()
        string(APPEND quoted " \"${argument}\"")
    endforeach()
    file(APPEND "${SPLAT_SCRIPT}" "echo ${target}\n${quoted}\n")
endfunction()

function(windows_sysroot target arch digest)
    build_tools(install)
    if(NOT install STREQUAL "")
        windows_local("${target}" "${arch}" "${install}")
        return()
    endif()
    if(NOT ACCEPT_LICENSE STREQUAL "yes")
        message(FATAL_ERROR
                "${target}: xwin downloads the Microsoft CRT ${XWIN_CRT_VERSION} "
                "and the Windows SDK ${XWIN_SDK_VERSION}, which Microsoft "
                "licenses to you. Pass -DACCEPT_LICENSE=yes to accept their "
                "terms.")
    endif()
    if(CMAKE_HOST_WIN32)
        # xwin links sdk/lib/<version> to its own directory whatever the
        # flags say, and Windows grants a symbolic link only to Developer
        # Mode or to an administrator. One link costs less to try than a
        # gigabyte to download.
        set(probe "${DEST}/.probe-directory")
        file(REMOVE_RECURSE "${probe}" "${probe}-link")
        file(MAKE_DIRECTORY "${probe}")
        file(CREATE_LINK "${probe}" "${probe}-link" SYMBOLIC RESULT linked)
        file(REMOVE_RECURSE "${probe}" "${probe}-link")
        if(NOT linked STREQUAL "0")
            message(FATAL_ERROR
                    "${target}: Windows refuses a symbolic link in ${DEST}, "
                    "and xwin lays out the SDK with one. Turn on Developer "
                    "Mode under Settings, System, For developers, or run "
                    "this command as an administrator. A Developer Command "
                    "Prompt of Visual Studio needs neither, because "
                    "lld-link reads the LIB variable it sets.")
        endif()
    endif()
    # The symlinks of xwin only fix the casing of the SDK for a
    # case-sensitive file system. Windows has none, and it refuses a
    # symlink to a program without the privilege, so they go.
    set(splat_options "")
    if(CMAKE_HOST_WIN32)
        set(splat_options --disable-symlinks)
    endif()
    if(NOT SPLAT STREQUAL "done")
        xwin_program(program)
        file(REMOVE_RECURSE "${DEST}/${target}")
        # DESIGN: the progress bar of xwin asks whether its own standard
        # output is a console and draws nothing when it is not. Under
        # cmake it never is, so SPLAT=script writes the command instead
        # and the caller runs it with a terminal of its own.
        if(SPLAT STREQUAL "script")
            write_splat("${program}" "${target}" "${arch}" "${splat_options}")
            return()
        endif()
        message(STATUS "${target}: xwin downloads about 1 GB and unpacks "
                       "it, which takes minutes without a word")
        execute_process(
            COMMAND "${program}" --accept-license
                    --cache-dir "${DEST}/.download/xwin" --arch "${arch}"
                    --crt-version "${XWIN_CRT_VERSION}"
                    --sdk-version "${XWIN_SDK_VERSION}"
                    splat --output "${DEST}/${target}" ${splat_options}
            RESULT_VARIABLE ran)
        if(NOT ran EQUAL 0)
            message(FATAL_ERROR "${program} failed for ${target}")
        endif()
    endif()
    tree_digest("${DEST}/${target}" actual)
    if(NOT actual STREQUAL digest)
        message(FATAL_ERROR "${DEST}/${target}: SHA-256 of the files "
                            "${actual}, expected ${digest}")
    endif()
    file(WRITE "${DEST}/licenses/windows-sdk.txt"
"The import libraries in sysroot/windows-x86_64 and sysroot/windows-arm64
come from the Microsoft C runtime ${XWIN_CRT_VERSION} and the Windows SDK
${XWIN_SDK_VERSION}, fetched with xwin ${XWIN_VERSION}. Microsoft distributes
them under the licence terms that xwin shows and that the caller accepted
with ACCEPT_LICENSE.
")
endfunction()

set(wrote_script FALSE)
foreach(target IN LISTS TARGETS)
    if(target MATCHES "^windows-" AND SPLAT STREQUAL "script")
        set(wrote_script TRUE)
    endif()
    if(target STREQUAL "linux-x86_64")
        linux_sysroot("${target}" x86_64 "${MUSL_DEV_X86_64}"
                      "${COMPILER_RT_X86_64}")
    elseif(target STREQUAL "linux-arm64")
        linux_sysroot("${target}" aarch64 "${MUSL_DEV_AARCH64}"
                      "${COMPILER_RT_AARCH64}")
    elseif(target STREQUAL "macos-arm64")
        macos_sysroot("${target}" arm64)
    elseif(target STREQUAL "macos-x86_64")
        macos_sysroot("${target}" x86_64)
    elseif(target STREQUAL "windows-x86_64")
        windows_sysroot("${target}" x86_64 "${XWIN_TREE_X86_64}")
    elseif(target STREQUAL "windows-arm64")
        windows_sysroot("${target}" aarch64 "${XWIN_TREE_AARCH64}")
    else()
        message(FATAL_ERROR "unknown target ${target}")
    endif()
    if(NOT (target MATCHES "^windows-" AND SPLAT STREQUAL "script"))
        message(STATUS "${DEST}/${target}")
    endif()
endforeach()
if(wrote_script)
    message(STATUS "splat script: ${SPLAT_SCRIPT}")
endif()
