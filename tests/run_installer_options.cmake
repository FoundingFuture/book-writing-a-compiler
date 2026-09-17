# The installers and the uninstallers of the two shells take the same
# options, because a reader of one page uses either. A spelling that only
# one of them knows fails the other with no message worth reading.
#
#   cmake -DROOT=<repository> -P tests/run_installer_options.cmake

set(scripts install.sh install.ps1 uninstall.sh uninstall.ps1)
# The processor of the package, which defaults to the processor of this
# machine. Each spelling stands in every script.
set(options --arm --arm64 --intel --x86_64 ANTI_ARCH)

foreach(script IN LISTS scripts)
    file(READ "${ROOT}/tools/${script}" text)
    foreach(option IN LISTS options)
        string(FIND "${text}" "${option}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR "tools/${script} does not know `${option}`, "
                                "which the other installers take")
        endif()
    endforeach()
endforeach()

# A package of another processor installs beside the one of this machine
# rather than over it, so both directories stay usable.
foreach(script install.sh uninstall.sh)
    file(READ "${ROOT}/tools/${script}" text)
    string(FIND "${text}" ".anti-" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "tools/${script} writes no directory of its own "
                            "for a package of another processor")
    endif()
endforeach()
foreach(script install.ps1 uninstall.ps1)
    file(READ "${ROOT}/tools/${script}" text)
    string(FIND "${text}" ".anti-" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "tools/${script} writes no directory of its own "
                            "for a package of another processor")
    endif()
endforeach()
