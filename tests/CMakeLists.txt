# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT
#
# Unit tests (tests/unit) and integration tests (tests/integration), run with CTest.
# GoogleTest is a build-time dependency only; it is never installed or packaged.

include(FetchContent)
FetchContent_Declare(googletest
    URL https://github.com/google/googletest/releases/download/v1.15.2/googletest-1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL
    SYSTEM)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

find_package(Qt6 REQUIRED COMPONENTS Test)

include(GoogleTest)

add_subdirectory(unit)
add_subdirectory(integration)

# --- Command-line smoke tests of the real executable (no display needed) -------
add_test(NAME cli.version COMMAND drone_simulator --version)
set_tests_properties(cli.version PROPERTIES
    PASS_REGULAR_EXPRESSION "^Drone Simulator ${PROJECT_VERSION}\n$"
    LABELS "integration;cli")

add_test(NAME cli.help COMMAND drone_simulator --help)
set_tests_properties(cli.help PROPERTIES
    PASS_REGULAR_EXPRESSION "Usage: drone_simulator"
    LABELS "integration;cli")

add_test(NAME cli.bad_log_level COMMAND drone_simulator --log-level loud)
set_tests_properties(cli.bad_log_level PROPERTIES
    WILL_FAIL TRUE
    LABELS "integration;cli")
