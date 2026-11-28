cmake_minimum_required(VERSION 3.25)

project(Somike
    VERSION 0.2.0
    DESCRIPTION "Route application audio into a physical microphone on Windows"
    LANGUAGES CXX RC
)

# Pre-release label ("" for a final release). The full version string is
# PROJECT_VERSION + suffix; the release workflow refuses a tag that differs
# from "v<full version>".
set(SOMIKE_VERSION_SUFFIX "-beta.3")
set(SOMIKE_VERSION_FULL "${PROJECT_VERSION}${SOMIKE_VERSION_SUFFIX}")

if(NOT WIN32)
    message(FATAL_ERROR "Somike targets Windows only.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "Somike must be built for 64-bit: the APO is loaded by the 64-bit audiodg.exe.")
endif()

option(SOMIKE_BUILD_TESTS "Build the unit tests" ${PROJECT_IS_TOP_LEVEL})
option(SOMIKE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Static CRT everywhere: the APO is loaded into audiodg.exe and the app ships
# as a standalone .exe, so neither may depend on the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# PDBs for every configuration: crashes inside audiodg.exe are only
# debuggable with symbols.
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT ProgramDatabase)

# All binaries side by side: somike.exe finds somike-apoctl.exe and
# somike_apo.dll next to itself.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(cmake/SomikeTargetOptions.cmake)

configure_file(cmake/version.h.in ${CMAKE_BINARY_DIR}/generated/somike/version.h @ONLY)
add_library(somike_version INTERFACE)
add_library(somike::version ALIAS somike_version)
target_include_directories(somike_version INTERFACE ${CMAKE_BINARY_DIR}/generated)

# Libraries (bottom-up)
add_subdirectory(src/protocol)   # shared-memory layout, header-only
add_subdirectory(src/dsp)        # ring reader/writer, resampler, mixer, injector
add_subdirectory(src/common)     # Win32 helpers
add_subdirectory(src/installer)  # endpoint inspection + elevated install logic
add_subdirectory(src/core)       # capture, routing, settings (no UI)

# Binaries
add_subdirectory(src/apo)        # somike_apo.dll (runs inside audiodg.exe)
add_subdirectory(src/apoctl)     # somike-apoctl.exe (install / uninstall / status)
add_subdirectory(src/app)        # somike.exe (desktop UI)
add_subdirectory(tools/selftest) # somike-selftest.exe (hardware end-to-end check)

if(SOMIKE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# `cmake --install` produces the portable folder that is also the release zip:
# somike.exe, somike-apoctl.exe and somike_apo.dll must stay side by side.
install(TARGETS somike somike_apoctl somike_selftest RUNTIME DESTINATION .)
install(TARGETS somike_apo LIBRARY DESTINATION .)
install(FILES README.md LICENSE CHANGELOG.md DESTINATION .)
