# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT
#
# Integration tests with a real QApplication:
#   ds_gui_tests — widgets only, on Qt's "offscreen" platform (no display needed).
#   ds_gl_tests  — OpenGL 4.5 core context, viewport and renderer on the native platform
#                  (X11/Wayland; use xvfb-run on headless machines). Qt's "offscreen" platform
#                  creates legacy GL contexts, so these tests are skipped — not failed — when no
#                  display or no OpenGL 4.5 core context is available.

# Sanitizer builds run 3–5 times slower: the longest end-to-end runs (a mission driven at
# dusk takes about 56 s under ASan on a loaded machine) need more than the normal 60 s.
if(DS_ENABLE_SANITIZERS OR DS_ENABLE_COVERAGE)
    set(DS_INTEGRATION_TIMEOUT 180)
else()
    set(DS_INTEGRATION_TIMEOUT 60)
endif()

function(ds_add_integration_test target)
    cmake_parse_arguments(ARG "FORCE_OFFSCREEN" "" "SOURCES" ${ARGN})
    add_executable(${target} test_main.cpp ${ARG_SOURCES})
    target_link_libraries(${target} PRIVATE GTest::gtest ds::app_ui ds::graphics ds::storage Qt6::Test
                                            GDAL::GDAL)
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/tests/support)
    if(ARG_FORCE_OFFSCREEN)
        target_compile_definitions(${target} PRIVATE DS_TEST_FORCE_OFFSCREEN)
    endif()
    ds_set_warnings(${target})
    gtest_discover_tests(${target}
        TEST_PREFIX "integration."
        PROPERTIES LABELS integration TIMEOUT ${DS_INTEGRATION_TIMEOUT}
        DISCOVERY_MODE PRE_TEST)
endfunction()

ds_add_integration_test(ds_gui_tests FORCE_OFFSCREEN SOURCES main_window_test.cpp settings_test.cpp
    scene_panels_test.cpp simulation_panels_test.cpp missions_panel_test.cpp
    mission_pack_test.cpp history_test.cpp)
target_compile_definitions(ds_gui_tests PRIVATE DS_TEST_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
ds_add_integration_test(ds_gl_tests SOURCES opengl_test.cpp scene_render_test.cpp)
target_link_libraries(ds_gl_tests PRIVATE ds::terrain ds::astro)
target_compile_definitions(ds_gl_tests PRIVATE DS_TEST_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
ds_add_integration_test(ds_storage_integration_tests FORCE_OFFSCREEN SOURCES storage_files_test.cpp
    mission_library_test.cpp)
target_compile_definitions(ds_storage_integration_tests PRIVATE
    DS_TEST_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
ds_add_integration_test(ds_mission_flight_tests FORCE_OFFSCREEN SOURCES mission_flight_test.cpp)
target_compile_definitions(ds_mission_flight_tests PRIVATE
    DS_TEST_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
ds_add_integration_test(ds_mission_flow_tests FORCE_OFFSCREEN SOURCES mission_flow_test.cpp
    file_terrain_test.cpp online_terrain_test.cpp data_providers_test.cpp lidar_terrain_test.cpp
    online_imagery_test.cpp)
# The LAS / LAZ fixtures are written with LASzip.
target_link_libraries(ds_mission_flow_tests PRIVATE LASzip::LASzip)
