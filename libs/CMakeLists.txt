# Builds the third-party libraries of the runtime archive as static
# libraries for one target triple. Run once per target, from CI or by hand.
# Libraries are added in the chapters that need them: the thread pool
# runtime, raylib, miniaudio, Mbed TLS, PCRE2.

cmake_minimum_required(VERSION 3.20)

project(antic-runtime LANGUAGES C)

set(ANTIC_RUNTIME_TARGET "" CACHE STRING
    "Target directory name, for example macos-arm64 or windows-x86_64")

if(ANTIC_RUNTIME_TARGET STREQUAL "")
    message(FATAL_ERROR "Set ANTIC_RUNTIME_TARGET, for example -DANTIC_RUNTIME_TARGET=macos-arm64")
endif()

set(ANTIC_RUNTIME_OUTPUT
    "${CMAKE_BINARY_DIR}/runtime/lib/${ANTIC_RUNTIME_TARGET}")
file(MAKE_DIRECTORY "${ANTIC_RUNTIME_OUTPUT}")
