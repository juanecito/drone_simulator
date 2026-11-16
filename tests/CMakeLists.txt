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

# --- Mission validation (FR-MIS-16) --------------------------------------------------------
add_test(NAME cli.validate_mission
    COMMAND drone_simulator
        --validate-mission ${PROJECT_SOURCE_DIR}/tests/fixtures/missions/precision-landing-01.mission.json
        --validate-mission ${PROJECT_SOURCE_DIR}/tests/fixtures/missions/rover-delivery-01.mission.json
        --validate-mission ${PROJECT_SOURCE_DIR}/assets/default-missions/first-flight.mission.json)
set_tests_properties(cli.validate_mission PROPERTIES
    PASS_REGULAR_EXPRESSION "first-flight.mission.json: valid"
    FAIL_REGULAR_EXPRESSION "invalid"
    LABELS "integration;cli")

add_test(NAME cli.validate_mission_invalid
    COMMAND drone_simulator --validate-mission ${PROJECT_SOURCE_DIR}/tests/fixtures/missions/invalid.mission.json)
set_tests_properties(cli.validate_mission_invalid PROPERTIES
    WILL_FAIL TRUE
    LABELS "integration;cli")

add_test(NAME cli.validate_mission_messages
    COMMAND drone_simulator --validate-mission ${PROJECT_SOURCE_DIR}/tests/fixtures/missions/invalid.mission.json)
set_tests_properties(cli.validate_mission_messages PROPERTIES
    PASS_REGULAR_EXPRESSION "objectives\\[1\\]\\.target: unknown item \"pad2\""
    LABELS "integration;cli")

# --- Generated documentation stays in sync with the mission schema ----------------------
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME docs.mission_reference
        COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/scripts/docs/gen_mission_reference.py --check)
    set_tests_properties(docs.mission_reference PROPERTIES LABELS "unit;docs")
endif()
