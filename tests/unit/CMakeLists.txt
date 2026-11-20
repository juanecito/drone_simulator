# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT

add_executable(ds_unit_tests
    graphics_test.cpp
    common_test.cpp
    terrain_test.cpp
    astro_test.cpp
    command_line_test.cpp
    flight_test.cpp
    geo_test.cpp
    ground_test.cpp
    input_test.cpp
    items_test.cpp
    json_schema_test.cpp
    log_test.cpp
    mesh_loader_test.cpp
    missions_test.cpp
    mission_play_test.cpp
    modules_test.cpp
    net_test.cpp
    physics_test.cpp
    raster_source_test.cpp
    renderer_test.cpp
    replay_test.cpp
    sim_test.cpp
    terrain_files_test.cpp
    version_test.cpp
    weather_test.cpp
    wind_test.cpp)
target_link_libraries(ds_unit_tests PRIVATE
    GTest::gtest_main
    ds::common ds::geo ds::astro ds::weather ds::storage ds::net ds::terrain ds::items
    ds::input ds::physics ds::missions ds::graphics ds::sim
    ds::app_ui)
target_compile_definitions(ds_unit_tests PRIVATE
    DS_TEST_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
    DS_TEST_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
# GDAL writes the raster fixtures (SPECIFICATION §9: fixtures generated in-test).
target_link_libraries(ds_unit_tests PRIVATE GDAL::GDAL)
ds_set_warnings(ds_unit_tests)
gtest_discover_tests(ds_unit_tests
    TEST_PREFIX "unit."
    PROPERTIES LABELS unit
    DISCOVERY_MODE PRE_TEST)

# Storage tests need a QCoreApplication (Qt SQL driver plugins), hence their own main().
add_executable(ds_storage_unit_tests qt_core_main.cpp storage_test.cpp)
target_link_libraries(ds_storage_unit_tests PRIVATE GTest::gtest ds::storage)
ds_set_warnings(ds_storage_unit_tests)
gtest_discover_tests(ds_storage_unit_tests
    TEST_PREFIX "unit."
    PROPERTIES LABELS unit
    DISCOVERY_MODE PRE_TEST)
